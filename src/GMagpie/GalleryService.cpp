#include "pch.h"
#include "App.h"
#include "AppSettings.h"
#include "CommonSharedConstants.h"
#include "EffectsService.h"
#include "GalleryService.h"
#include "Logger.h"
#include "NvidiaVfxParameters.h"
#include "ScalingService.h"
#include "StrHelper.h"
#include "ToastService.h"
#include "Win32Helper.h"
#include <optional>
#include <shellapi.h>
#include <unordered_set>
#include <wincodec.h>

using namespace winrt;
using namespace winrt::GMagpie::implementation;

namespace fs = std::filesystem;

namespace GMagpie {

// Строки интерфейса галереи — из ресурсов, как весь остальной UI (все четыре
// поддерживаемые локали). Вызывается только с UI-потока.
static std::wstring _ResourceString(const wchar_t* key) {
	return std::wstring{
		ResourceLoader::GetForViewIndependentUse(CommonSharedConstants::APP_RESOURCE_MAP_ID)
			.GetString(key)
	};
}

// Поддерживаемые растровые форматы; HEIC/AVIF доступны, если в системе стоят
// соответствующие декодеры WIC/WinRT
static const std::unordered_set<std::wstring> SUPPORTED_EXTENSIONS = {
	L".jpg", L".jpeg", L".jpe", L".jfif", L".png", L".bmp",
	L".gif", L".tif", L".tiff", L".webp", L".heic", L".heif", L".avif"
};

// Цикл апскейлеров для Num6: HLSL-ресемплеры без сторонних SDK плюс RTX VSR
// (по явной просьбе пользователя). VSR — нативный эффект: он участвует только в
// сборках с включённым RTX Video (MP_ENABLE_RTX_VIDEO_DENOISE), иначе выбор убивал
// сессию на инициализации рендерера. Остальные проверяются на доступность при
// запуске галереи (GetEffect + CanScale); отсутствующий — пропускается с логом.
static const std::vector<std::string> UPSCALER_CANDIDATES = {
	"Bicubic", "Lanczos", "Jinc", "NIS\\NIS"
#ifdef MP_ENABLE_RTX_VIDEO_DENOISE
	, std::string(NVIDIA_VFX_VSR_ID)
#endif
};

// Человекочитаемое имя апскейлера для списка и тоста: id эффектов вида
// «Папка\\Имя» показываем без папки, у VSR — принятое короткое имя «RTX VSR».
// В конфигурацию и сравнения уходит по-прежнему полный id.
static std::wstring UpscalerDisplayName(const std::string& name) {
	std::string shown = name;
	if (const auto sep = shown.rfind('\\'); sep != std::string::npos) {
		shown = shown.substr(sep + 1);
	}
	if (shown == "RTXVideo_VSR") {
		shown = "RTX VSR";
	}
	return StrHelper::UTF8ToUTF16(shown);
}

// Оверлеи показа (строка состояния, список апскейлера) живут в topmost-полосе, но
// окно вывода при каждом показе страницы заново подтверждает свой z-порядок
// (журнал: «foreground-change», «step=first-display status=applied») — и оверлей тихо
// проваливается под фото: скриншот-прогон показывал кадр без списка и без счётчика.
// Тост GMagpie держится над фото именно потому, что topmost; layered-окно тонуло под
// DComp-владельцем даже с верным owner (проверено зондом z-порядка), поэтому окна
// обычные, owned и с таймером-страховкой (200 мс): цепочка GW_HWNDPREV проверяет,
// не оказалась ли она под окном вывода, и возвращает окно на вершину полосы.
// GetWindow-цепочка — дешёвая операция; окно вывода ищется FindWindowW'ом.
static bool IsBelowOutputWindow(HWND hwnd) noexcept {
	HWND output = FindWindowW(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr);
	if (!output || output == hwnd) {
		return false;
	}
	for (int hops = 0; hops < 4096; ++hops) {
		hwnd = GetWindow(hwnd, GW_HWNDPREV);
		if (!hwnd) {
			return false;
		}
		if (hwnd == output) {
			return true;
		}
	}
	return false;
}

constexpr UINT_PTR Z_REASSERT_TIMER = 1;
// Постоянная строка информации о показе: номер фото, всего файлов и процент
// масштабирования (100% — родной размер). Отдельное мини-окно поверх кадра:
// topmost, click-through, не видно в Alt+Tab. Кадр отрисовывается на GPU и
// own-текст поверх него не нарисовать, поэтому окно app-side; внутри всё GDI,
// так как это ровно одна строка текста. Доступ только с UI-потока.
// EXIF-ориентация 5..8 означает «кадр сохранён повёрнутым»: исход в эффект-цепь
// идёт уже повёрнутым (источник декодирует с RespectExifOrientation), значит для
// отображения ширина и высота меняются местами. Иначе проценты на вертикальном
// телефонном снимке переворачиваются (вместо «уменьшить» показывают «увеличить»).
// Чистая функция — проверяется CPU-тестом.
static bool PhotoAppliesExifRotation(unsigned short orientation) noexcept {
	return orientation >= 5 && orientation <= 8;
}

namespace gallery_status {

constexpr wchar_t CLASS_NAME[] = L"GMagpie.GalleryStatus";

constexpr int PADDING_X = 16;
constexpr int PADDING_Y = 10;
constexpr int MARGIN = 24;          // отступ от угла кадра
constexpr COLORREF BG_COLOR = RGB(16, 16, 16);
constexpr COLORREF FG_COLOR = RGB(240, 240, 240);

std::wstring g_text;
int g_textW = 0;
int g_textH = 0;
wil::unique_hwnd g_hwnd;
wil::unique_hfont g_font;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		RECT rc;
		GetClientRect(hWnd, &rc);
		static const wil::unique_hbrush background{ CreateSolidBrush(BG_COLOR) };
		FillRect(hdc, &rc, background.get());
		if (g_font && !g_text.empty()) {
			SetBkMode(hdc, TRANSPARENT);
			SetTextColor(hdc, FG_COLOR);
			const auto oldFont = SelectObject(hdc, g_font.get());
			// NODRAWTEXT в Common.Post.props — центрируем вручную размерами из замера
			TextOutW(hdc, (rc.right - rc.left - g_textW) / 2,
				(rc.bottom - rc.top - g_textH) / 2, g_text.c_str(), static_cast<int>(g_text.size()));
			SelectObject(hdc, oldFont);
		}
		EndPaint(hWnd, &ps);
		return 0;
	}
	case WM_TIMER:
		// Страховка z-порядка: если окно вывода перетянул на себя кадр, вернуться
		if (IsBelowOutputWindow(hWnd)) {
			Logger::Get().Info("Галерея: z-страховка вернула окно показа над кадром");
			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		return 0;
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Путь к фото -> пиксельный размер. Только заголовок (WIC DecodeMetadataCacheOnDemand),
// полного декодирования нет
bool TryGetImageSize(const std::wstring& path, UINT& width, UINT& height) {
	com_ptr<IWICImagingFactory> wic = try_create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
	if (!wic) {
		return false;
	}
	com_ptr<IWICBitmapDecoder> decoder;
	if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnDemand, decoder.put()))) {
		return false;
	}
	com_ptr<IWICBitmapFrameDecode> frame;
	if (FAILED(decoder->GetFrame(0, frame.put()))) {
		return false;
	}
	UINT w = 0;
	UINT h = 0;
	if (FAILED(frame->GetSize(&w, &h)) || !w || !h) {
		return false;
	}
	// GetSize возвращает размер закодированного потока БЕЗ EXIF-поворота, а на
	// экран кадр уходит повёрнутым — приведём проценты к отображаемому аспекту
	unsigned short orientation = 0;
	if (com_ptr<IWICMetadataQueryReader> reader;
		SUCCEEDED(frame->GetMetadataQueryReader(reader.put()))) {
		PROPVARIANT value{};
		if (SUCCEEDED(reader->GetMetadataByName(L"System.Photo.Orientation", &value))
			&& value.vt == VT_UI2) {
			orientation = value.uiVal;
		}
		PropVariantClear(&value);
	}
	if (PhotoAppliesExifRotation(orientation)) {
		std::swap(w, h);
	}
	width = w;
	height = h;
	return true;
}

void Update(const std::wstring& text, const RECT& frameRect) {
	g_text = text;

	static std::once_flag s_classOnce;
	std::call_once(s_classOnce, [] {
		WNDCLASSEXW wc{ .cbSize = sizeof(wc) };
		wc.lpfnWndProc = WndProc;
		wc.hInstance = GetModuleHandle(nullptr);
		wc.lpszClassName = CLASS_NAME;
		if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			Logger::Get().Win32Error("Регистрация класса окна состояния не удалась");
		}
	});

	// Холостой owner вредит: окно вывода живёт в topmost-полосе и при каждом показе
	// страницы перетверждает свой z-порядок (WM_WINDOWPOSCHANGING в ScalingWindow),
	// а owned-оверлей OS возвращает к владельцу — ровно под кадр, каким бы topmost он
	// ни был (замерено: таймер крутил TOPMOST, окно стояло слотом ниже вывода; так же
	// падает под кадр и тост после первой же страницы). Поэтому окно без владельца:
	// держится над кадром topmost-полосой и таймером-страховкой, а без WS_EX_LAYERED —
	// layered тонул под DComp-представлением даже без владельца. Фон непрозрачный.
	if (!g_hwnd) {
		g_hwnd.reset(CreateWindowExW(
			WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
			CLASS_NAME, L"", WS_POPUP,
			0, 0, 0, 0, nullptr, nullptr, GetModuleHandle(nullptr), nullptr));
		if (!g_hwnd) {
			Logger::Get().Win32Error("Создание окна состояния галереи не удалось");
			return;
		}
	}

	if (!g_font) {
		// кегль фиксированный в DIP, растёт по dpi окна
		const int fontDpi = GetDpiForWindow(g_hwnd.get());
		g_font.reset(CreateFontW(-MulDiv(13, fontDpi, 96), 0, 0, 0, FW_SEMIBOLD,
			FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
	}

	const int dpi = GetDpiForWindow(g_hwnd.get());

	// GetDC(nullptr) освобождается только через ReleaseDC — не обёрткой DeleteDC
	HDC hdc = GetDC(nullptr);
	HGDIOBJ oldFont = g_font ? SelectObject(hdc, g_font.get()) : nullptr;
	SIZE sz{};
	if (GetTextExtentPoint32W(hdc, g_text.c_str(), static_cast<int>(g_text.size()), &sz)) {
		g_textW = sz.cx;
		g_textH = sz.cy;
	}
	if (oldFont) {
		SelectObject(hdc, oldFont);
	}
	ReleaseDC(nullptr, hdc);

	const int w = g_textW + 2 * PADDING_X * dpi / 96;
	const int h = g_textH + 2 * PADDING_Y * dpi / 96;
	const int margin = MARGIN * dpi / 96;
	// правый нижний угол кадра с отступом
	const int x = frameRect.right - margin - w;
	const int y = frameRect.bottom - margin - h;

	// Вершина topmost-полосы: см. IsBelowOutputWindow — окно вывода регулярно
	// подтверждает свой z-порядок, оверлей обязан быть выше полосы этого окна.
	if (!SetWindowPos(g_hwnd.get(), HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE)) {
		Logger::Get().Win32Error("Размещение окна состояния галереи не удалось");
		return;
	}
	Logger::Get().Info(fmt::format(
		"Галерея: окно состояния размещено, topmost={}, окно=0x{:X}",
		(GetWindowLongPtrW(g_hwnd.get(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
		reinterpret_cast<uintptr_t>(g_hwnd.get())));
	ShowWindow(g_hwnd.get(), SW_SHOWNOACTIVATE);
	SetTimer(g_hwnd.get(), Z_REASSERT_TIMER, 200, nullptr); // см. IsBelowOutputWindow
	InvalidateRect(g_hwnd.get(), nullptr, FALSE);
}

void Hide() noexcept {
	g_hwnd.reset();
	g_font.reset();
	g_text.clear();
}

} // namespace gallery_status

// Список выбора апскейлера (Num6): в отличие от строки состояния — многострочный,
// с заголовком и подсветкой текущей позиции. Сам он не исчезает: закрывается только
// повторным Num6, Esc или F11 — выбор стрелкой оставляет список открытым. Окно такое
// же, как у gallery_status: topmost, click-through, не видно в Alt+Tab; всё GDI.
// Доступ только с UI-потока.
namespace gallery_picker {

constexpr wchar_t CLASS_NAME[] = L"GMagpie.GalleryPicker";

constexpr int PADDING_X = 20;
constexpr int PADDING_Y = 14;
constexpr int ROW_GAP = 4;        // между строками, в DIP
constexpr int SEL_INSET = 8;      // подсветка шире текста
constexpr COLORREF BG_COLOR = RGB(16, 16, 16);
constexpr COLORREF FG_COLOR = RGB(225, 225, 225);
constexpr COLORREF HEADER_COLOR = RGB(150, 150, 150);
constexpr COLORREF SEL_BG_COLOR = RGB(240, 240, 240);
constexpr COLORREF SEL_FG_COLOR = RGB(16, 16, 16);

std::wstring g_header;
std::vector<std::wstring> g_rows;
int g_selected = -1;
// Геометрия считается в Update (там есть DC и dpi), WndProc только рисует по ней
RECT g_headerPos{};
std::vector<RECT> g_rowBg;      // прямоугольник подсветки строки
std::vector<POINT> g_rowText;   // левый верх текста строки
int g_rowH = 0;
wil::unique_hwnd g_hwnd;
wil::unique_hfont g_font;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		RECT rc;
		GetClientRect(hWnd, &rc);
		static const wil::unique_hbrush background{ CreateSolidBrush(BG_COLOR) };
		static const wil::unique_hbrush selection{ CreateSolidBrush(SEL_BG_COLOR) };
		FillRect(hdc, &rc, background.get());
		if (g_font) {
			SetBkMode(hdc, TRANSPARENT);
			const auto oldFont = SelectObject(hdc, g_font.get());
			// NODRAWTEXT в Common.Post.props — только TextOutW по заранее посчитанным позициям
			SetTextColor(hdc, HEADER_COLOR);
			TextOutW(hdc, g_headerPos.left, g_headerPos.top,
				g_header.c_str(), static_cast<int>(g_header.size()));
			for (size_t i = 0; i < g_rows.size() && i < g_rowBg.size(); ++i) {
				if (static_cast<int>(i) == g_selected) {
					FillRect(hdc, &g_rowBg[i], selection.get());
					SetTextColor(hdc, SEL_FG_COLOR);
				} else {
					SetTextColor(hdc, FG_COLOR);
				}
				TextOutW(hdc, g_rowText[i].x, g_rowText[i].y,
					g_rows[i].c_str(), static_cast<int>(g_rows[i].size()));
			}
			SelectObject(hdc, oldFont);
		}
		EndPaint(hWnd, &ps);
		return 0;
	}
	case WM_TIMER:
		// Страховка z-порядка: если окно вывода перетянул на себя кадр, вернуться
		if (IsBelowOutputWindow(hWnd)) {
			Logger::Get().Info("Галерея: z-страховка вернула окно показа над кадром");
			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		}
		return 0;
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void Update(std::wstring header, std::vector<std::wstring> rows, int selected, const RECT& frameRect) {
	g_header = std::move(header);
	g_rows = std::move(rows);
	g_selected = selected;

	static std::once_flag s_classOnce;
	std::call_once(s_classOnce, [] {
		WNDCLASSEXW wc{ .cbSize = sizeof(wc) };
		wc.lpfnWndProc = WndProc;
		wc.hInstance = GetModuleHandle(nullptr);
		wc.lpszClassName = CLASS_NAME;
		if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			Logger::Get().Win32Error("Регистрация класса окна выбора апскейлера не удалась");
		}
	});

	// См. окно состояния: без владельца, topmost-полоса + таймер; layered тонул под
	// DComp-представлением (замерено зондом)
	if (!g_hwnd) {
		g_hwnd.reset(CreateWindowExW(
			WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
			CLASS_NAME, L"", WS_POPUP,
			0, 0, 0, 0, nullptr, nullptr, GetModuleHandle(nullptr), nullptr));
		if (!g_hwnd) {
			Logger::Get().Win32Error("Создание окна выбора апскейлера не удалось");
			return;
		}
	}

	if (!g_font) {
		const int fontDpi = GetDpiForWindow(g_hwnd.get());
		g_font.reset(CreateFontW(-MulDiv(14, fontDpi, 96), 0, 0, 0, FW_SEMIBOLD,
			FALSE, FALSE, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"));
	}

	const int dpi = GetDpiForWindow(g_hwnd.get());
	const int padx = PADDING_X * dpi / 96;
	const int pady = PADDING_Y * dpi / 96;
	const int gap = ROW_GAP * dpi / 96;
	const int inset = SEL_INSET * dpi / 96;

	// Замеряем все строки тем же шрифтом, каким рисуем (GetDC(nullptr) освобождается
	// только через ReleaseDC — не обёрткой DeleteDC)
	std::vector<SIZE> sizes(g_rows.size() + 1);
	HDC hdc = GetDC(nullptr);
	const HGDIOBJ oldFont = g_font ? SelectObject(hdc, g_font.get()) : nullptr;
	GetTextExtentPoint32W(hdc, g_header.c_str(), static_cast<int>(g_header.size()), &sizes[0]);
	for (size_t i = 0; i < g_rows.size(); ++i) {
		GetTextExtentPoint32W(hdc, g_rows[i].c_str(), static_cast<int>(g_rows[i].size()), &sizes[i + 1]);
	}
	if (oldFont) {
		SelectObject(hdc, oldFont);
	}
	ReleaseDC(nullptr, hdc);

	g_rowH = sizes[0].cy;
	for (const auto& s : sizes) {
		g_rowH = std::max(g_rowH, static_cast<int>(s.cy));
	}
	int maxTextW = 0;
	for (const auto& s : sizes) {
		maxTextW = std::max(maxTextW, static_cast<int>(s.cx));
	}

	const int w = maxTextW + 2 * padx;
	const int rowsTotal = static_cast<int>(g_rows.size()) + 1; // заголовок включительно
	const int h = 2 * pady + rowsTotal * g_rowH + (rowsTotal - 1) * gap;

	g_headerPos = { padx, pady, 0, 0 };
	g_rowBg.clear();
	g_rowText.clear();
	g_rowBg.reserve(g_rows.size());
	g_rowText.reserve(g_rows.size());
	int y = pady + g_rowH + gap;
	for (size_t i = 0; i < g_rows.size(); ++i) {
		// подсветка — через всю ширину списка, отступами от краёв окна
		g_rowBg.push_back({ padx - inset, y - gap / 2, w - padx + inset, y + g_rowH + gap / 2 });
		g_rowText.push_back({ padx, y });
		y += g_rowH + gap;
	}

	// по центру кадра, чуть выше середины — список не должен перекрывать строку состояния
	const int fw = frameRect.right - frameRect.left;
	const int fh = frameRect.bottom - frameRect.top;
	const int x = frameRect.left + (fw - w) / 2;
	const int py = frameRect.top + (fh - h) / 3;

	if (!SetWindowPos(g_hwnd.get(), HWND_TOPMOST, x, py, w, h, SWP_NOACTIVATE)) {
		Logger::Get().Win32Error("Размещение окна выбора апскейлера не удалось");
		return;
	}
	Logger::Get().Info(fmt::format(
		"Галерея: окно выбора размещено, topmost={}, окно=0x{:X}",
		(GetWindowLongPtrW(g_hwnd.get(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
		reinterpret_cast<uintptr_t>(g_hwnd.get())));
	ShowWindow(g_hwnd.get(), SW_SHOWNOACTIVATE);
	SetTimer(g_hwnd.get(), Z_REASSERT_TIMER, 200, nullptr); // см. IsBelowOutputWindow
	InvalidateRect(g_hwnd.get(), nullptr, FALSE);
}

void Hide() noexcept {
	g_hwnd.reset();
	g_font.reset();
	g_header.clear();
	g_rows.clear();
	g_rowBg.clear();
	g_rowText.clear();
	g_selected = -1;
}

} // namespace gallery_picker

// Плейлист альбома: все поддерживаемые растровые файлы рекурсивно, в стабильном
// порядке по полному пути; регистр расширения не важен. Порядок — это то, что
// пользователь видит как «номера страниц», поэтому он обязан быть детерминированным.
static std::vector<std::wstring> BuildPhotoPlaylist(const std::wstring& folder) {
	std::vector<std::wstring> playlist;
	std::error_code ec;
	fs::recursive_directory_iterator it(folder,
		fs::directory_options::skip_permission_denied, ec);
	if (ec) {
		Logger::Get().Error(fmt::format("Галерея: не удалось открыть папку ({})", StrHelper::UTF16ToUTF8(folder)));
		return playlist;
	}
	for (const auto& entry : it) {
		std::error_code ignore;
		if (!entry.is_regular_file(ignore) || ignore) continue;
		std::wstring ext = entry.path().extension().wstring();
		std::ranges::transform(ext, ext.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
		if (!SUPPORTED_EXTENSIONS.contains(ext)) continue;
		playlist.push_back(entry.path().wstring());
	}
	std::ranges::sort(playlist);
	return playlist;
}

// Сохранённая страница может оказаться за пределами текущего альбома (файлы
// удалили, добавили подпапку) — восстанавливаем ближайшую существующую.
static size_t PhotoResumeIndex(size_t saved, size_t count) noexcept {
	return count == 0 ? 0 : std::min(saved, count - 1);
}

bool GalleryService::Start(std::wstring folder, uint32_t intervalSeconds, int scalingModeIdx, size_t startIndex) {
	_lastStartFailure = GalleryStartFailure::Other;
	if (_running) {
		Logger::Get().Warn("Галерея уже запущена; сначала остановите текущую сессию");
		return false;
	}

	_BuildPlaylist(folder);
	if (_playlist.empty()) {
		Logger::Get().Error(fmt::format("Галерея: в папке нет поддерживаемых изображений ({})",
			StrHelper::UTF16ToUTF8(folder)));
		_lastStartFailure = GalleryStartFailure::NoImages;
		if (AppSettings::Get().GalleryLastFolder().native() == folder) {
			// продолжать больше некуда: сохранённая сессия ведёт в пустую папку
			AppSettings::Get().GalleryLastSession({}, 0);
		}
		return false;
	}

	// восстанавливать можно только на существующую страницу: альбом с прошлого
	// запуска мог измениться (файлы удалены, добавлены подпапки)
	_folder = std::move(folder);
	_index = PhotoResumeIndex(startIndex, _playlist.size());
	_stepDirection = 1;
	if (startIndex >= _playlist.size()) {
		Logger::Get().Warn(fmt::format("Галерея: сохранённая страница {} недоступна ({} файлов), начинаем с {}",
			startIndex + 1, _playlist.size(), _index + 1));
	}

	_upscalerCycle.clear();
	for (const std::string& name : UPSCALER_CANDIDATES) {
		const auto info = EffectsService::Get().GetEffect(StrHelper::UTF8ToUTF16(name));
		if (info && info->CanScale()) {
			_upscalerCycle.push_back(name);
		} else {
			Logger::Get().Warn(fmt::format("Галерея: апскейлер недоступен, пропущен: {}", name));
		}
	}

	if (!ScalingService::Get().StartPhotoSession(_playlist[_index], scalingModeIdx)) {
		// Обычно это «ещё идёт обычное масштабирование»: пользователю нужно
		// дождаться его конца, а не искать картинки в папке
		_lastStartFailure = ScalingService::Get().IsScaling()
			? GalleryStartFailure::SessionBusy : GalleryStartFailure::Other;
		_Cleanup();
		return false;
	}

	// Сессия могла быть остановлена извне (ошибка, завершение приложения) — тогда
	// убираем хуки и таймер. При штатной остановке IsPhotoSession() уже сброшен.
	_scalingChangedRevoker = ScalingService::Get().IsScalingChanged(
		winrt::auto_revoke, [this](bool isScaling) {
			if (isScaling || ScalingService::Get().IsPhotoSession()) {
				return;
			}
			// Снимать подписку из её же обработчика нельзя: Invoke идёт по списку
			// делегатов, а _Cleanup рвёт этот элемент (Event.h прямо это запрещает) —
			// мог бы пропасть следующий подписчик после галереи. Делаем то же самое
			// следующим оборотом диспетчера, когда Invoke вернёт управление.
			App::Get().Dispatcher().TryEnqueue([this] {
				if (_running) {
					_Cleanup();
				}
			});
		});

	_keyboardHook.reset(SetWindowsHookExW(WH_KEYBOARD_LL, _KeyboardHookProc,
		GetModuleHandle(nullptr), 0));
	_mouseHook.reset(SetWindowsHookExW(WH_MOUSE_LL, _MouseHookProc,
		GetModuleHandle(nullptr), 0));
	if (!_keyboardHook || !_mouseHook) {
		Logger::Get().Win32Error("Установка глобальных хуков галереи не удалась");
		_Cleanup();
		ScalingService::Get().StopPhotoSession();
		return false;
	}

	_intervalSeconds = intervalSeconds;
	if (_intervalSeconds > 0) {
		try {
			_autoTimer = App::Get().Dispatcher().CreateTimer();
			_autoTimer.IsRepeating(true);
			_autoTimer.Interval(std::chrono::seconds(_intervalSeconds));
			_autoTimer.Tick([this](auto const&, auto const&) {
				if (_paused || _playlist.empty()) return;
				_stepDirection = 1;
				_index = (_index + 1) % _playlist.size();
				_ShowCurrent();
			});
			_autoTimer.Start();
		} catch (const winrt::hresult_error& e) {
			// Иначе фото-сессия и хуки висели бы до следующей попытки Start
			Logger::Get().ComError("Таймер автолистания галереи не создан", e.code());
			_Cleanup();
			ScalingService::Get().StopPhotoSession();
			return false;
		}
	}

	_lastStartFailure = GalleryStartFailure::None;
	_running = true;
	// Первая страница та же, что и при листании: строка состояния и сохранённое
	// положение должны появляться сразу, а не после первого нажатия
	_UpdateStatusOverlay();
	_PersistSession(true);
	Logger::Get().Info(fmt::format("Галерея запущена: {} файлов, страница {}/{}, интервал {} c, апскейлеров в цикле: {}",
		_playlist.size(), _index + 1, _playlist.size(), _intervalSeconds, _upscalerCycle.size()));
	return true;
}

// Продолжение прошлого просмотра: та же папка и та же страница, что были при
// последней остановке. Ошибкой считается только недоступная папка — пустой
// плейлист или недостижимая страница разбирает Start
bool GalleryService::StartLast(uint32_t intervalSeconds) {
	// Причина отказа актуальна только для текущего запуска: иначе при недоступной
	// папке главной странице показался бы тост с причиной ПРОШЛОГО отказа
	_lastStartFailure = GalleryStartFailure::Other;
	const fs::path folder = AppSettings::Get().GalleryLastFolder();
	std::error_code ec;
	if (folder.empty() || !fs::is_directory(folder, ec)) {
		Logger::Get().Warn(fmt::format("Галерея: продолжать нечего, папка прошлой сессии недоступна ({})",
			StrHelper::UTF16ToUTF8(folder.native())));
		return false;
	}

	return Start(folder.native(), intervalSeconds, -1, AppSettings::Get().GalleryLastIndex());
}

void GalleryService::Stop() {
	if (!_running) return;
	_Cleanup();
	ScalingService::Get().StopPhotoSession();
}

void GalleryService::_BuildPlaylist(const std::wstring& folder) {
	_playlist = BuildPhotoPlaylist(folder);
}

void GalleryService::_ShowCurrent() {
	if (_index >= _playlist.size()) return;
	Logger::Get().Info(fmt::format("Галерея: {}/{} — {}", _index + 1, _playlist.size(),
		StrHelper::UTF16ToUTF8(_playlist[_index])));
	ScalingService::Get().RequestPhoto(_playlist[_index]);
	_PrefetchNext();
	_UpdateStatusOverlay();
	_PersistSession(false);
	ProgressChanged.Invoke(_index + 1, _playlist.size());
}

// Положение нужно, чтобы «Продолжить» на главной странице вернула к тому же фото.
// Пишется вся конфигурация, поэтому при быстром листании — не чаще раза в несколько
// секунд, а при остановке (в том числе внешнюю) всегда.
void GalleryService::_PersistSession(bool force) noexcept {
	if (_folder.empty()) return;

	const uint32_t tick = GetTickCount();
	if (!force && tick - _lastPersistTick < 3000) return;
	_lastPersistTick = tick;

	AppSettings::Get().GalleryLastSession(_folder, static_cast<uint32_t>(_index));
}

// Какой кадр предзагружать следующим — в ту сторону, куда пользователь сейчас
// листает. Обратная прокрутка колесом на «вперёд»-предзагрузке всегда идёт по
// холодному следу: по логам переключения декод занимает p50 ~40 мс и p90 ~63 мс,
// то есть почти всю задержку страницы, и это единственное, что можно убрать вперёд.
static size_t PhotoPrefetchIndex(size_t current, size_t count, int direction) noexcept {
	if (count == 0) {
		return 0;
	}
	return direction < 0 ? (current + count - 1) % count : (current + 1) % count;
}

//  подсказка фоновому предзагрузчику:decode следующую по направлению хода
// фотографию. Это эвристика: попадание не гарантировано (HOME/END, прыжки), зато
// обычный путь декодирования на месте остаётся запасным.
void GalleryService::_PrefetchNext() {
	if (_playlist.empty()) return;
	const size_t next = PhotoPrefetchIndex(_index, _playlist.size(), _stepDirection);
	ScalingService::Get().RequestPhotoPrefetch(_playlist[next]);
}

// Проценты строки состояния: во сколько раз фото будет увеличен или уменьшен при
// подгонке под экран (Fit сохраняет пропорции, поэтому min по сторонам). 100% —
// родной размер файла ровно вписывается в кадр, то есть апскейл не нужен.
static double PhotoScalePercent(UINT width, UINT height, UINT frameWidth, UINT frameHeight) noexcept {
	if (width == 0 || height == 0 || frameWidth == 0 || frameHeight == 0) {
		return 0.0;
	}
	return std::min(static_cast<double>(frameWidth) / width,
		static_cast<double>(frameHeight) / height) * 100.0;
}

// Строка информации: "3 / 5001 · 155%". Проценты показывают, во сколько раз
// фото будет увеличено/уменьшен при подгонке под экран (режим Fit сохраняет
// пропорции, поэтому масштаб = min по сторонам); 100% — родной размер файла
void GalleryService::_UpdateStatusOverlay() {
	std::wstring percent = L"?";

	RECT frameRect{};
	if (HWND output = FindWindowW(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr);
		output && IsWindowVisible(output)) {
		GetWindowRect(output, &frameRect);
	} else {
		// на старте сессии окно может ещё не существовать — берём основной монитор
		frameRect = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
	}

	if (UINT w = 0, h = 0; gallery_status::TryGetImageSize(_playlist[_index], w, h)) {
		percent = fmt::format(L"{:.0f}%", PhotoScalePercent(w, h,
			static_cast<UINT>(frameRect.right - frameRect.left),
			static_cast<UINT>(frameRect.bottom - frameRect.top)));
	}

	gallery_status::Update(
		fmt::format(L"{} / {}  ·  {}", _index + 1, _playlist.size(), percent), frameRect);
}

void GalleryService::_ResetAutoTimer() {
	if (!_autoTimer || _paused || _intervalSeconds == 0) return;
	// Stop+Start — единственный способ сбросить отсчёт у DispatcherQueueTimer
	_autoTimer.Stop();
	_autoTimer.Start();
}

void GalleryService::_SetPaused(bool paused) {
	_paused = paused;
	if (!_autoTimer) return;
	if (_paused) {
		_autoTimer.Stop();
	} else if (_intervalSeconds > 0) {
		// ручное действие не должно порождать мгновенный автопереход
		_ResetAutoTimer();
	}

	// без тоста пауза неотличима от зависшей автопрокрутки
	if (HWND scalingWindow = FindWindowW(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr)) {
		ToastService::Get().ShowMessageOnWindow({},
			_ResourceString(_paused ? L"Gallery_PausedToast" : L"Gallery_AutoPlayToast"), scalingWindow);
	}
}

// Заменив первый эффект, перегружаем цепь на лету. Позиция -1 соответствует
// оригинальному эффекту профиля; выход за границы цикла restores её.
// Позиция в цикле апскейлеров: -1 — «как в профиле» (то, с чем сессию
// запустили), 0..cycleSize-1 — безопасный список ресемплеров. Шаг ±1 с переходом
// через край в обе стороны — им же листается список (Num6). Отдельная функция
// потому, что арифметика со «-1 как первым элементом» — классическое место
// off-by-one. silent=true — без тоста: при выборе из списка список и есть отклик.
static int NextUpscalerCycle(int position, int cycleSize, int direction) noexcept {
	const int count = cycleSize + 1;  // -1..n-1 => n вариантов + оригинал
	return (((position + 1) + direction) % count + count) % count - 1;
}

// Какую страницу показать после удаления текущего файла из плейлиста: на её место
// встаёт следующая (тот же индекс), с последней страницы — сдвиг на одну назад.
// nullopt — альбом опустел, показ нужно остановить.
static std::optional<size_t> PhotoIndexAfterDelete(size_t index, size_t oldSize) noexcept {
	if (oldSize == 0) {
		return std::nullopt;
	}
	const size_t newSize = oldSize - 1;
	if (newSize == 0) {
		return std::nullopt;
	}
	if (index >= newSize) {
		return newSize - 1;
	}
	return index;
}

void GalleryService::_CycleUpscaler(int direction, bool silent) {
	if (_upscalerCycle.empty()) return;

	if (_cyclePos == -2) {
		_originalFirst = ScalingService::Get().PhotoEffectsSnapshot();
		if (_originalFirst.empty()) return;
		// Эффект профиля и строка цикла с тем же id — одна и та же строка списка:
		// для профиля «RTX Video VSR Ultra» список показывал два «RTX VSR», а выбор
		// из цикловой копии терял параметры профиля. Оригинал остаётся первой строкой
		// (_cyclePos = -1), из цикла такой id убирается.
		std::erase(_upscalerCycle, _originalFirst[0].name);
		_cyclePos = -1;
	}

	const int next = NextUpscalerCycle(_cyclePos, (int)_upscalerCycle.size(), direction);
	if (next == _cyclePos) return;

	std::vector<EffectOption> effects = _originalFirst;
	std::wstring displayName;
	if (next == -1) {
		displayName = UpscalerDisplayName(_originalFirst[0].name);
	} else {
		EffectOption& first = effects[0];
		first.name = _upscalerCycle[next];
		first.parameters.clear();
		// единственный ресайл делает первый эффект — как при запуске сессии
		first.scalingType = ScalingType::Fit;
		first.scale = { 1.f, 1.f };
		displayName = UpscalerDisplayName(_upscalerCycle[next]);
	}

	if (!ScalingService::Get().RestartPhotoEffects(std::move(effects))) {
		Logger::Get().Error("Галерея: перегрузка цепи эффектов не удалась");
		return;
	}
	_cyclePos = next;

	// Список открыт — подсвечиваем выбранный немедленно: выбор применяется, не
	// закрывая список, и до перерисовки подсветка оставалась на прошлой строке
	// (видно было только после Num6→Num6)
	if (_pickerVisible) {
		_ShowUpscalerPicker();
	}

	// Смена эффекта делается пересозданием сессии, а новая сессия загружает первую
	// фотографию — сразу наводим обратно на текущую страницу, иначе выбор апскейлера
	// сбрасывал показ на первую страницу альбома
	if (!_playlist.empty()) {
		ScalingService::Get().RequestPhoto(_playlist[_index]);
	}

	if (!silent) {
		if (HWND scalingWindow = FindWindowW(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr)) {
			ToastService::Get().ShowMessageOnWindow({},
				fmt::format(fmt::runtime(_ResourceString(L"Gallery_UpscalerToast")), displayName), scalingWindow);
		}
	}
}

// Delete: файл текущей страницы уходит в корзину (FOF_ALLOWUNDO — случайное удаление
// отменяется проводником), плейлист сжимается на него, показ продолжается следующим
// фото; на последней странице — сдвиг назад; альбом опустел — показ останавливаем.
void GalleryService::_DeleteCurrentPhoto() {
	if (_deleting) return;
	_deleting = true;
	// SHFileOperationW разворачивает модальный насос сообщений: LL-хуки и таймер
	// живы и реентерили бы навигацию/удаление, сдвигая _index под внешним erase
	// (стирание не того файла, а при опустевшем плейлисте — erase за end()).
	const auto clearDeleting = wil::scope_exit([this] { _deleting = false; });
	if (_playlist.empty()) return;
	const std::wstring path = _playlist[_index];

	// SHFileOperationW хочет список путей, удвоенно-нулевой терминатор
	std::wstring from = path;
	from.push_back(L'\0');
	SHFILEOPSTRUCTW op{  // не const: SHFileOperationW пишет fAnyOperationsAborted
		.wFunc = FO_DELETE,
		.pFrom = from.c_str(),
		.fFlags = FOF_ALLOWUNDO | FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI,
	};
	if (const int rc = SHFileOperationW(&op); rc != 0 || op.fAnyOperationsAborted) {
		Logger::Get().Warn(fmt::format(
			"Галерея: файл не удалён (код {}), показ не изменился", rc));
		return;
	}

	// Страховка на будущее: LL-хуки и таймер живут на этом потоке и во время
	// модального насоса могли сдвинуть _index/плейлист — стираем именно свой файл,
	// а не позицию; если состояние пропало (внешний Stop) — выходим без erase за end()
	if (!_running) {
		return;
	}
	const auto it = std::find(_playlist.begin(), _playlist.end(), path);
	if (it == _playlist.end()) {
		return;
	}
	Logger::Get().Info(fmt::format("Галерея: файл удалён в корзину — {}",
		StrHelper::UTF16ToUTF8(path)));
	_playlist.erase(it);
	if (const auto next = PhotoIndexAfterDelete(_index, _playlist.size() + 1)) {
		_index = *next;
		_ShowCurrent();
		_ResetAutoTimer();
	} else {
		Logger::Get().Info("Галерея: альбом пуст после удаления — останавливаем показ");
		Stop();
	}

	if (HWND scalingWindow = FindWindowW(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr)) {
		ToastService::Get().ShowMessageOnWindow({},
			_ResourceString(L"Gallery_DeletedToast"), scalingWindow);
	}
}

// Список апскейлеров: оригинал профиля сверху (это то, с чем запускали сессию),
// ниже — цикл доступных способов (включая RTX VSR, если он есть в системе).
// Подсвечена текущая позиция (_cyclePos = -1 — первая строка). Инициализация
// снимка общая с _CycleUpscaler: список можно открыть и до первой замены.
void GalleryService::_ShowUpscalerPicker() {
	if (_upscalerCycle.empty()) return;

	if (_cyclePos == -2) {
		_originalFirst = ScalingService::Get().PhotoEffectsSnapshot();
		if (_originalFirst.empty()) return;
		// См. _CycleUpscaler: цикловая копия эффекта профиля из списка убирается,
		// оригинал — первая строка (_cyclePos = -1)
		std::erase(_upscalerCycle, _originalFirst[0].name);
		_cyclePos = -1;
	}

	std::vector<std::wstring> rows;
	rows.reserve(_upscalerCycle.size() + 1);
	rows.push_back(UpscalerDisplayName(_originalFirst[0].name));
	for (const auto& name : _upscalerCycle) {
		rows.push_back(UpscalerDisplayName(name));
	}

	RECT frameRect{};
	if (HWND output = FindWindowW(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr);
		output && IsWindowVisible(output)) {
		GetWindowRect(output, &frameRect);
	} else {
		frameRect = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
	}

	_pickerVisible = true;
	gallery_picker::Update(_ResourceString(L"Gallery_PickerHeader"), std::move(rows), _cyclePos + 1, frameRect);
}

void GalleryService::_HideUpscalerPicker() noexcept {
	_pickerVisible = false;
	gallery_picker::Hide();
}

void GalleryService::_Cleanup() noexcept {
	// состояние прошлой сессии сохраняем до того, как всё сброшено
	if (_running) {
		_PersistSession(true);
	}
	try {
		gallery_status::Hide();
		gallery_picker::Hide();
		_keyboardHook.reset();
		_mouseHook.reset();
		if (_autoTimer) {
			_autoTimer.Stop();
			_autoTimer = nullptr;
		}
		_scalingChangedRevoker.Revoke();
	} catch (...) {
		Logger::Get().Error("Галерея: ошибка при остановке");
	}
	_playlist.clear();
	_originalFirst.clear();
	_upscalerCycle.clear();
	_running = false;
	_paused = false;
	_index = 0;
	_intervalSeconds = 0;
	_cyclePos = -2;
	_pickerVisible = false;
	_num6Held = false;
	_wheelAccum = 0;
	_lastPersistTick = 0;
	_stepDirection = 1;
	_folder.clear();
	// главная страница больше не должна показывать прогресс показа
	try {
		ProgressChanged.Invoke(0, 0);
	} catch (...) {
		// _Cleanup — noexcept, исключение подписчика здесь означало бы terminate
		Logger::Get().Error("Подписчик ProgressChanged бросил исключение при остановке галереи");
	}
}

// Что делает клавиша галереи в зависимости от состояния списка апскейлеров —
// вся карта здесь (тип — в GalleryService.h), проверяется тестами на CPU.
// Список намеренно короткий: всё остальное (включая Esc в чужих приложениях)
// должно доставаться обычным программам.
static GalleryKeyAction DecideGalleryKey(unsigned vkCode, bool pickerVisible) noexcept {
	switch (vkCode) {
	case VK_LEFT: case VK_UP:
		return pickerVisible ? GalleryKeyAction::PickerUp : GalleryKeyAction::PagePrev;
	case VK_RIGHT: case VK_DOWN:
		return pickerVisible ? GalleryKeyAction::PickerDown : GalleryKeyAction::PageNext;
	case VK_HOME:
		return GalleryKeyAction::First;
	case VK_END:
		return GalleryKeyAction::Last;
	case VK_SPACE:
		return GalleryKeyAction::TogglePause;
	case VK_NUMPAD6:
		return GalleryKeyAction::TogglePicker;
	case VK_DELETE:
		// в списке Delete не занят (он только выбирает стрелками), так что удаляем
		// фото при любом состоянии
		return GalleryKeyAction::DeletePhoto;
	case VK_ESCAPE:
		return pickerVisible ? GalleryKeyAction::ClosePicker : GalleryKeyAction::Stop;
	case VK_F11:
		// как Esc: со списком — закрыть список, без него — остановить показ.
		// Пользователь ждёт от F11 (полноэкранный) выхода из просмотра, а «чужой»
		// полноэкранный во время показа недоступен в принципе — перехват ничего не отнимает
		return pickerVisible ? GalleryKeyAction::ClosePicker : GalleryKeyAction::Stop;
	default:
		return GalleryKeyAction::PassThrough;
	}
}

LRESULT CALLBACK GalleryService::_KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
	auto& self = Get();
	if (nCode >= 0 && self._running) {
		const auto vkCode = ((KBDLLHOOKSTRUCT*)lParam)->vkCode;
		if (wParam == WM_KEYUP && vkCode == VK_NUMPAD6) {
			// удержание Num6 присылает автоповтор: переключение — одно на нажатие
			self._num6Held = false;
		} else if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
			const auto action = DecideGalleryKey(vkCode, self._pickerVisible);
			// хоткеи принадлежат галерее только пока она в фокусе — иначе клавиши
			// (включая Esc) достаются обычным приложениям
			if (action != GalleryKeyAction::PassThrough && self._IsSessionForeground()) {
				if (action == GalleryKeyAction::TogglePicker) {
					if (self._num6Held) {
						return 1;  // автоповтор удерживаемой клавиши: ничего не переключаем
					}
					self._num6Held = true;
				}
				self._OnKeyAction(action);
				return 1;
			}
		}
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK GalleryService::_MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
	auto& self = Get();
	if (nCode >= 0 && self._running) {
		if (wParam == WM_MOUSEWHEEL) {
			// В WH_MOUSE_LL данные приходят в MSLLHOOKSTRUCT, а не в wParam: величина
			// колеса — старшее слово mouseData (в wParam здесь просто код сообщения,
			// из-за GET_WHEEL_DELTA_WPARAM delta всегда был 0 и колесо молчало).
			const short delta = static_cast<short>(HIWORD(((MSLLHOOKSTRUCT*)lParam)->mouseData));
			if (delta != 0 && self._IsSessionForeground()) {
				self._OnMouseWheel(delta);
				return 1;
			}
		} else if (wParam == WM_MOUSEHWHEEL) {
			// боковое колесо (наклон мыши) листает в те же стороны
			const short delta = static_cast<short>(HIWORD(((MSLLHOOKSTRUCT*)lParam)->mouseData));
			if (delta != 0 && self._IsSessionForeground()) {
				self._OnMouseWheel(delta);
				return 1;
			}
		} else if (wParam == WM_LBUTTONDOWN) {
			// потеряв фокус, галерея недоступна для клавиш — клик по фото
			// возвращает её на передний план (окно click-through, само не активируется)
			if (!self._IsSessionForeground() &&
				self._TryFocusFromPhotoClick(((MSLLHOOKSTRUCT*)lParam)->pt)) {
				return 1;
			}
		}
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// Ввод «наш», только когда галерея в фокусе. Само scaling-окно — WS_EX_NOACTIVATE и
// фокус держит невидимый PhotoStage, поэтому фокусным считается любое из этих окон и
// окно в их owner-цепочке
bool GalleryService::_IsSessionForeground() const noexcept {
	HWND fg = Win32Helper::GetRealForegroundWindow();
	if (!fg) return false;

	const HWND output = FindWindowW(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr);
	const HWND stage = FindWindowW(ScalingService::PHOTO_STAGE_CLASS_NAME, nullptr);
	if (!output && !stage) return false;

	for (int depth = 0; fg && depth < 8; ++depth) {
		if (fg == output || fg == stage) return true;
		fg = GetWindow(fg, GW_OWNER);
	}
	return false;
}

// Клик по фото (в т.ч. по «невидимым» окнам под ним — scaling-окно пропускает
// мышь насквозь) возвращает галерее фокус. Клики по own-окнам приложения и по
// поверх лежащим topmost окнам (always-on-top панели, трей) не трогаем
bool GalleryService::_TryFocusFromPhotoClick(POINT pt) noexcept {
	const HWND output = FindWindowW(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr);
	const HWND stage = FindWindowW(ScalingService::PHOTO_STAGE_CLASS_NAME, nullptr);
	if (!output || !stage || !IsWindowVisible(output)) return false;

	RECT rc;
	if (!GetWindowRect(output, &rc) || !PtInRect(&rc, pt)) return false;

	if (HWND hit = WindowFromPoint(pt)) {
		if (HWND root = GetAncestor(hit, GA_ROOT)) {
			if (root != stage) {
				DWORD pid = 0;
				GetWindowThreadProcessId(root, &pid);
				if (pid == GetCurrentProcessId()) return false;
				if (GetWindowLongPtrW(root, GWL_EXSTYLE) & WS_EX_TOPMOST) return false;
				// остальное лежит под фото — клик до него всё равно не дошёл бы
			}
		}
	}

	// SetForegroundWindow чужого окна верхнего плана почти всегда отклоняется —
	// временно присоединяем вход потока того окна к нашему (классический Win32-приём)
	if (HWND fg = Win32Helper::GetRealForegroundWindow()) {
		if (const DWORD fgThread = GetWindowThreadProcessId(fg, nullptr);
			fgThread && fgThread != GetCurrentThreadId()) {
			if (AttachThreadInput(fgThread, GetCurrentThreadId(), TRUE)) {
				SetForegroundWindow(stage);
				AttachThreadInput(fgThread, GetCurrentThreadId(), FALSE);
				return true;
			}
		}
	}
	SetForegroundWindow(stage);
	return true;
}

// Переход на delta страниц с циклом через границы; направление запоминаем для
// предзагрузки, отсчёт автолистания сбрасываем — ручной ход всегда важнее таймера
void GalleryService::_Navigate(long long delta) {
	const long long count = (long long)_playlist.size();
	_stepDirection = delta < 0 ? -1 : 1;
	long long index = ((long long)_index + delta) % count;
	if (index < 0) index += count;
	_index = (size_t)index;
	_ShowCurrent();
	// любое ручное перелистывание откладывает автопереход на полный интервал
	_ResetAutoTimer();
}

void GalleryService::_OnKeyAction(GalleryKeyAction action) {
	if (!_running || _playlist.empty()) return;

	switch (action) {
	case GalleryKeyAction::PagePrev:
		_Navigate(-1);
		break;
	case GalleryKeyAction::PageNext:
		_Navigate(+1);
		break;
	case GalleryKeyAction::First:
		if (_index != 0) {
			_index = 0;
			_ShowCurrent();
			_ResetAutoTimer();
		}
		break;
	case GalleryKeyAction::Last:
		if (_index != _playlist.size() - 1) {
			_index = _playlist.size() - 1;
			_ShowCurrent();
			_ResetAutoTimer();
		}
		break;
	case GalleryKeyAction::TogglePause:
		_SetPaused(!_paused);
		break;
	case GalleryKeyAction::TogglePicker:
		if (_pickerVisible) {
			_HideUpscalerPicker();
		} else {
			_ShowUpscalerPicker();
		}
		break;
	case GalleryKeyAction::ClosePicker:
		_HideUpscalerPicker();
		break;
	// Выбор применяется сразу, список остаётся открытым: эффект виден на фото,
	// а список позволяет, не листая страницы, попробовать следующий способ
	case GalleryKeyAction::PickerUp:
		_CycleUpscaler(-1, true);
		break;
	case GalleryKeyAction::PickerDown:
		_CycleUpscaler(+1, true);
		break;
	case GalleryKeyAction::DeletePhoto:
		_DeleteCurrentPhoto();
		break;
	case GalleryKeyAction::Stop:
		Stop();
		break;
	case GalleryKeyAction::PassThrough:
		break;
	}
}

// Одна страница на полный щелчок колеса. «Плавные» колёса (манипуляторы Win11,
// тачпад) присылают много мелких событий: доли копятся, а листаем целыми щелчками —
// иначе прокрутка уходит через край. Возвращает число страниц со знаком
// (минус — к предыдущему). Чистая функция: есть что проверить тестом без показа.
static int WheelPageSteps(int& accumulator, short wheelDelta) noexcept {
	accumulator += wheelDelta;

	int steps = 0;
	while (accumulator >= WHEEL_DELTA) {
		accumulator -= WHEEL_DELTA;
		++steps;
	}
	while (accumulator <= -WHEEL_DELTA) {
		accumulator += WHEEL_DELTA;
		--steps;
	}
	return steps;
}

void GalleryService::_OnMouseWheel(short wheelDelta) {
	if (!_running || _playlist.empty()) return;
	const int steps = WheelPageSteps(_wheelAccum, wheelDelta);
	// колесо вниз — следующее фото, вверх — предыдущее; колесо НЕ стрелки:
	// список апскейлеров на него не реагирует и не закрывается
	if (steps != 0) {
		_Navigate(-steps);
	}
}

}
