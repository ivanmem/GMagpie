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
#include <shlobj_core.h>
#include <shobjidl_core.h>
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

bool IsSupportedPhoto(std::wstring_view path) {
	std::wstring ext = fs::path(path).extension().wstring();
	std::ranges::transform(ext, ext.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
	return SUPPORTED_EXTENSIONS.contains(ext);
}

// Цикл апскейлеров для Num6: HLSL-ресемплеры без сторонних SDK плюс RTX VSR
// (по явной просьбе пользователя). VSR — нативный эффект: он участвует только в
// сборках с включённым RTX Video (MP_ENABLE_RTX_VIDEO_DENOISE), иначе выбор убивал
// сессию на инициализации рендерера. Остальные проверяются на доступность при
// запуске галереи (GetEffect + CanScale); отсутствующий — пропускается с логом.
// В меню остались только ACNet (нейросетевой апскейлер, единственный конкурент VSR) и VSR; прочие
// ресемплеры (Lanczos, Jinc, NIS) скрыты по просьбе пользователя. Оригинал профиля - всегда первая строка.
// Не эффект, а сочетание: RTX VSR в режиме Deblur (тот же размер, снимает мыло) и затем обычный RTX VSR.
// На мыльных фото и анимациях один VSR почти не отличается от простого увеличения
static constexpr std::string_view VSR_DEBLUR_ID = "RTXVideo\\RTXVideo_VSR+Deblur";

static const std::vector<std::string> UPSCALER_CANDIDATES = {
	"ACNet"
#ifdef MP_ENABLE_RTX_VIDEO_DENOISE
	, std::string(NVIDIA_VFX_VSR_ID)
	, std::string(VSR_DEBLUR_ID)
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
	} else if (shown == "RTXVideo_VSR+Deblur") {
		shown = "RTX VSR + Deblur";
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

constexpr const wchar_t* CLASS_NAME = CommonSharedConstants::GALLERY_STATUS_WINDOW_CLASS_NAME;

constexpr int PADDING_X = 16;
constexpr int PADDING_Y = 10;
constexpr int MARGIN = 24;          // отступ от угла кадра
constexpr COLORREF BG_COLOR = RGB(16, 16, 16);
constexpr COLORREF FG_COLOR = RGB(240, 240, 240);

// Процент окрашен по состоянию апскейла показанной страницы: красный - ещё не применён (виден оригинал),
// зелёный - применён; апскейл не нужен (фото не меньше экрана) - обычный цвет
constexpr COLORREF PENDING_COLOR = RGB(255, 99, 71);
constexpr COLORREF APPLIED_COLOR = RGB(108, 203, 95);

std::wstring g_text;
int g_textW = 0;
int g_textH = 0;
WPARAM g_upscaleState = CommonSharedConstants::PHOTO_UPSCALE_NOT_NEEDED;
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
			const auto oldFont = SelectObject(hdc, g_font.get());
			// NODRAWTEXT в Common.Post.props — центрируем вручную размерами из замера
			const int x = (rc.right - rc.left - g_textW) / 2;
			const int y = (rc.bottom - rc.top - g_textH) / 2;
			// Процент - после последнего «·»; до него обычным цветом, его - цветом состояния
			const size_t split = g_text.find_last_of(L'\x00B7');
			const size_t percentAt = split == std::wstring::npos ? g_text.size() : split + 1;
			SetTextColor(hdc, FG_COLOR);
			TextOutW(hdc, x, y, g_text.c_str(), static_cast<int>(percentAt));
			SIZE prefix{};
			GetTextExtentPoint32W(hdc, g_text.c_str(), static_cast<int>(percentAt), &prefix);
			SetTextColor(hdc, g_upscaleState == CommonSharedConstants::PHOTO_UPSCALE_PENDING ? PENDING_COLOR
				: g_upscaleState == CommonSharedConstants::PHOTO_UPSCALE_APPLIED ? APPLIED_COLOR : FG_COLOR);
			TextOutW(hdc, x + prefix.cx, y, g_text.c_str() + percentAt,
				static_cast<int>(g_text.size() - percentAt));
			SelectObject(hdc, oldFont);
		}
		EndPaint(hWnd, &ps);
		return 0;
	}
	case CommonSharedConstants::WM_GALLERY_UPSCALE_STATE:
		if (g_upscaleState != wParam) {
			g_upscaleState = wParam;
			InvalidateRect(hWnd, nullptr, FALSE);
		}
		return 0;
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

// Контекстное меню галереи (правая кнопка мыши и Num6) - обычное системное меню: мышь и клавиатура
// работают штатно. Оно модальное, поэтому открывается из очереди диспетчера, не из LL-хука.
// Пока меню открыто, хуки галереи пропускают ввод мимо (_menuOpen).
namespace gallery_menu {

constexpr UINT ID_UPSCALER_BASE = 1000;  // + строка списка (0 - оригинал профиля)
constexpr UINT ID_PAUSE = 2000;
constexpr UINT ID_DELETE = 2001;
constexpr UINT ID_STOP = 2002;
constexpr UINT ID_SAVE_AS = 2003;
constexpr UINT ID_FAVORITE = 2004;

} // namespace gallery_menu

// Плейлист альбома: все поддерживаемые растровые файлы рекурсивно, в стабильном
// порядке по полному пути; регистр расширения не важен. Порядок — это то, что
// пользователь видит как «номера страниц», поэтому он обязан быть детерминированным.
static std::vector<std::wstring> BuildPhotoPlaylist(const std::wstring& folder, bool recursive = true) {
	std::vector<std::wstring> playlist;
	std::error_code ec;
	const auto options = fs::directory_options::skip_permission_denied;
	const auto collect = [&](auto it) {
		for (const auto& entry : it) {
			std::error_code ignore;
			if (!entry.is_regular_file(ignore) || ignore) continue;
			if (!IsSupportedPhoto(entry.path().native())) continue;
			playlist.push_back(entry.path().wstring());
		}
	};
	if (recursive) {
		fs::recursive_directory_iterator it(folder, options, ec);
		if (!ec) collect(it);
	} else {
		fs::directory_iterator it(folder, options, ec);
		if (!ec) collect(it);
	}
	if (ec) {
		Logger::Get().Error(fmt::format("Галерея: не удалось открыть папку ({})", StrHelper::UTF16ToUTF8(folder)));
		return playlist;
	}
	std::ranges::sort(playlist);
	return playlist;
}

// Контекстное меню и «Открыть с помощью»: HKCU\Software\Classes, права администратора не нужны.
// Динамический пункт на каждом поддерживаемом расширении (SystemFileAssociations не трогает
// программу по умолчанию), плюс Applications\GMagpie.exe для списка «Открыть с помощью».
bool GalleryService::RegisterShell(bool enable) {
	static constexpr const wchar_t* VERB = L"GMagpie.Gallery";
	static constexpr const wchar_t* APP_KEY = L"Software\\Classes\\Applications\\GMagpie.exe";

	const std::wstring exe = Win32Helper::GetExePath().native();
	const std::wstring command = fmt::format(L"\"{}\" \"%1\"", exe);
	const std::wstring icon = fmt::format(L"\"{}\",0", exe);
	bool ok = true;
	const auto setValue = [&](const std::wstring& key, const wchar_t* name, const std::wstring& value) {
		const LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ,
			value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
		if (status != ERROR_SUCCESS) {
			Logger::Get().Win32Error(fmt::format("Запись в реестр не удалась ({})", StrHelper::UTF16ToUTF8(key)));
			ok = false;
		}
	};
	const auto deleteTree = [&](const std::wstring& key) {
		const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
		if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
			Logger::Get().Win32Error(fmt::format("Удаление из реестра не удалось ({})", StrHelper::UTF16ToUTF8(key)));
			ok = false;
		}
	};

	if (enable) {
		const std::wstring label = _ResourceString(L"Home_Gallery_ShellMenu");
		setValue(APP_KEY, L"FriendlyAppName", L"GMagpie");
		setValue(std::wstring(APP_KEY) + L"\\shell\\open", L"MUIVerb", label);
		setValue(std::wstring(APP_KEY) + L"\\shell\\open\\command", nullptr, command);
	} else {
		deleteTree(APP_KEY);
	}
	for (const std::wstring& ext : SUPPORTED_EXTENSIONS) {
		const std::wstring verbKey = fmt::format(L"Software\\Classes\\SystemFileAssociations\\{}\\shell\\{}", ext, VERB);
		if (enable) {
			setValue(std::wstring(APP_KEY) + L"\\SupportedTypes", ext.c_str(), L"");
			setValue(verbKey, nullptr, _ResourceString(L"Home_Gallery_ShellMenu"));
			setValue(verbKey, L"Icon", icon);
			setValue(verbKey + L"\\command", nullptr, command);
		} else {
			deleteTree(verbKey);
		}
	}
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
	return ok;
}

bool GalleryService::IsShellRegistered() {
	const std::wstring key = fmt::format(
		L"Software\\Classes\\SystemFileAssociations\\{}\\shell\\GMagpie.Gallery\\command", *SUPPORTED_EXTENSIONS.begin());
	return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

// Приём файлов от второго запуска: message-only окно, WM_COPYDATA с путём. Обработка уходит в очередь
// диспетчера - внутри SendMessage отправителя нельзя запускать показ.
static constexpr const wchar_t* IPC_WINDOW_CLASS = L"GMagpie_GalleryIpc";
static constexpr ULONG_PTR IPC_OPEN_FILE = 0x47414C31;

static LRESULT CALLBACK GalleryIpcProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_COPYDATA) {
		const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
		if (data && data->dwData == IPC_OPEN_FILE && data->lpData && data->cbData >= sizeof(wchar_t)) {
			std::wstring file(static_cast<const wchar_t*>(data->lpData), data->cbData / sizeof(wchar_t));
			file.resize(wcsnlen(file.c_str(), file.size()));
			App::Get().Dispatcher().TryEnqueue([file = std::move(file)]() mutable {
				GalleryService::Get().OpenFile(std::move(file));
			});
			return TRUE;
		}
		return FALSE;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void GalleryService::CreateIpcWindow() {
	const WNDCLASSEXW wc{
		.cbSize = sizeof(WNDCLASSEXW),
		.lpfnWndProc = GalleryIpcProc,
		.hInstance = GetModuleHandle(nullptr),
		.lpszClassName = IPC_WINDOW_CLASS
	};
	RegisterClassExW(&wc);
	if (!CreateWindowExW(0, IPC_WINDOW_CLASS, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr)) {
		Logger::Get().Win32Error("Окно приёма файлов галереи не создано");
	}
}

bool GalleryService::ForwardToRunning(std::wstring_view file) {
	const std::wstring path(file);
	const COPYDATASTRUCT data{
		.dwData = IPC_OPEN_FILE,
		.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)),
		.lpData = const_cast<wchar_t*>(path.c_str())
	};
	// Первый экземпляр мог только что стартовать и ещё не создать окно приёма
	for (int attempt = 0; attempt < 30; ++attempt) {
		if (const HWND target = FindWindowExW(HWND_MESSAGE, nullptr, IPC_WINDOW_CLASS, nullptr)) {
			DWORD_PTR result = 0;
			if (SendMessageTimeoutW(target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
				SMTO_ABORTIFHUNG, 3000, &result) && result) {
				return true;
			}
			return false;
		}
		Sleep(100);
	}
	return false;
}

void GalleryService::OpenFile(std::wstring file) {
	std::error_code ec;
	if (!IsSupportedPhoto(file) || !fs::is_regular_file(file, ec)) {
		Logger::Get().Warn(fmt::format("Галерея: переданный файл не открыт ({})", StrHelper::UTF16ToUTF8(file)));
		return;
	}
	std::wstring folder = fs::path(file).parent_path().wstring();
	if (!_running) {
		Start(std::move(folder), 0, -1, 0, std::move(file));
		return;
	}
	// Идущий показ: тот же плейлист-«соседи по папке», что и при обычном открытии файла
	const auto playlist = BuildPhotoPlaylist(folder, false);
	const auto it = std::ranges::find_if(playlist, [&](const std::wstring& path) {
		return _wcsicmp(path.c_str(), file.c_str()) == 0;
	});
	if (it == playlist.end()) return;
	_playlist = playlist;
	_folder = std::move(folder);
	_index = static_cast<size_t>(it - playlist.begin());
	_stepDirection = 1;
	_ResetAutoTimer();
	_ShowCurrent();
}

// Сохранённая страница может оказаться за пределами текущего альбома (файлы
// удалили, добавили подпапку) — восстанавливаем ближайшую существующую.
static size_t PhotoResumeIndex(size_t saved, size_t count) noexcept {
	return count == 0 ? 0 : std::min(saved, count - 1);
}

bool GalleryService::Start(std::wstring folder, uint32_t intervalSeconds, int scalingModeIdx, size_t startIndex,
	std::wstring openFile) {
	_lastStartFailure = GalleryStartFailure::Other;
	if (_running) {
		Logger::Get().Warn("Галерея уже запущена; сначала остановите текущую сессию");
		return false;
	}

	_BuildPlaylist(folder, openFile.empty());
	if (!openFile.empty()) {
		// Открыть одно фото: страница — сам файл (в плейлисте тот же префикс папки, регистр имени не важен)
		const auto it = std::ranges::find_if(_playlist, [&](const std::wstring& path) {
			return _wcsicmp(path.c_str(), openFile.c_str()) == 0;
		});
		if (it == _playlist.end()) {
			Logger::Get().Warn(fmt::format("Галерея: открываемый файл не найден в папке ({}), начинаем с первого",
				StrHelper::UTF16ToUTF8(openFile)));
		}
		startIndex = it == _playlist.end() ? 0 : static_cast<size_t>(it - _playlist.begin());
	}
	if (_playlist.empty()) {
		Logger::Get().Error(fmt::format("Галерея: в папке нет поддерживаемых изображений ({})",
			StrHelper::UTF16ToUTF8(folder)));
		_lastStartFailure = GalleryStartFailure::NoImages;
		if (AppSettings::Get().GalleryLastFolder().native() == folder) {
			// продолжать больше некуда: сохранённая сессия ведёт в пустую папку
			AppSettings::Get().GalleryLastSession({}, 0, 0);
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
		// Достаточно, что эффект есть: CanScale значит «размер выхода свободный», а ACNet увеличивает
		// ровно вдвое - с этой проверкой он молча пропадал из списка
		const std::string_view effect = name == VSR_DEBLUR_ID ? NVIDIA_VFX_VSR_ID : std::string_view(name);
		if (EffectsService::Get().GetEffect(StrHelper::UTF8ToUTF16(effect))) {
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

void GalleryService::_BuildPlaylist(const std::wstring& folder, bool recursive) {
	_playlist = BuildPhotoPlaylist(folder, recursive);
}

void GalleryService::_ShowCurrent() {
	if (_index >= _playlist.size()) return;
	// Запрос страницы - первым делом: это единственное, что ждёт пользователь. Остальное (лог, строка состояния
	// с чтением размера файла через WIC, сохранение) может подождать: _ShowCurrent зовётся прямо из LL-хука клавиш
	ScalingService::Get().RequestPhoto(_playlist[_index]);
	_PrefetchNext();
	Logger::Get().Info(fmt::format("Галерея: {}/{} — {}", _index + 1, _playlist.size(),
		StrHelper::UTF16ToUTF8(_playlist[_index])));
	// Строку состояния обновляем после возврата из хука и один раз на серию нажатий; чтение файла в хуке на
	// загруженной системе съедало бы LowLevelHooksTimeout и тормозило следующее нажатие
	if (!std::exchange(_statusUpdateQueued, true)) {
		App::Get().Dispatcher().TryEnqueue(winrt::DispatcherQueuePriority::Low, [this] {
			_statusUpdateQueued = false;
			if (_running) {
				_UpdateStatusOverlay();
			}
		});
	}
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

	AppSettings::Get().GalleryLastSession(_folder, static_cast<uint32_t>(_index),
		static_cast<uint32_t>(_playlist.size()));
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
// Где на экране фото: окно вывода, а пока его нет (старт сессии, перезапуск при смене апскейлера) -
// сцена фото, она уже стоит на мониторе показа. Основной монитор - только если нет ни того, ни другого,
// иначе на втором мониторе строка и меню уезжали на основной.
static RECT PhotoFrameRect() noexcept {
	RECT rect{};
	for (const wchar_t* cls : { CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, ScalingService::PHOTO_STAGE_CLASS_NAME }) {
		if (HWND hwnd = FindWindowW(cls, nullptr); hwnd && IsWindowVisible(hwnd) && GetWindowRect(hwnd, &rect)) return rect;
	}
	Logger::Get().Info("Галерея: окно фото не найдено - строка состояния на основном мониторе");
	return { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
}

void GalleryService::_UpdateStatusOverlay() {
	std::wstring percent = L"?";

	// Пока фото не на экране (первая страница, перезапуск при смене апскейлера), окна вывода нет: строка встала бы
	// по окну-сцене не туда и с неверным процентом. Ждём окно вывода и показываем строку вместе с фото.
	HWND output = FindWindowW(CommonSharedConstants::SCALING_WINDOW_CLASS_NAME, nullptr);
	if (!output || !IsWindowVisible(output)) {
		if (!_statusRetryTimer) {
			_statusRetryTimer = App::Get().Dispatcher().CreateTimer();
			_statusRetryTimer.Interval(std::chrono::milliseconds(100));
			_statusRetryTimer.IsRepeating(false);
			_statusRetryTimer.Tick([this](auto const&, auto const&) {
				if (_running) _UpdateStatusOverlay();
			});
		}
		_statusRetryTimer.Start();
		return;
	}
	const RECT frameRect = PhotoFrameRect();

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

bool GalleryService::_EnsureUpscalerSnapshot() {
	// Пустой список других апскейлеров - не повод прятать раздел: текущий режим меню показывает всегда
	if (_cyclePos != -2) return true;

	_originalFirst = ScalingService::Get().PhotoEffectsSnapshot();
	if (_originalFirst.empty()) return false;
	// Эффект профиля и строка цикла с тем же id — одна и та же строка списка:
	// для профиля «RTX Video VSR Ultra» список показывал два «RTX VSR», а выбор
	// из цикловой копии терял параметры профиля. Оригинал остаётся первой строкой
	// (_cyclePos = -1), из цикла такой id убирается.
	std::erase(_upscalerCycle, _originalFirst[0].name);
	_cyclePos = -1;
	return true;
}

void GalleryService::_ApplyUpscaler(int next, bool silent) {
	if (!_EnsureUpscalerSnapshot() || next == _cyclePos ||
		next < -1 || next >= static_cast<int>(_upscalerCycle.size())) {
		return;
	}

	std::vector<EffectOption> effects = _originalFirst;
	std::wstring displayName;
	if (next == -1) {
		displayName = UpscalerDisplayName(_originalFirst[0].name);
	} else {
		const bool deblur = _upscalerCycle[next] == VSR_DEBLUR_ID;
		EffectOption& first = effects[0];
		first.name = deblur ? std::string(NVIDIA_VFX_VSR_ID) : _upscalerCycle[next];
		first.parameters.clear();
		const auto info = EffectsService::Get().GetEffect(StrHelper::UTF8ToUTF16(first.name));
		if (info && info->CanScale()) {
			// единственный ресайл делает первый эффект — как при запуске сессии
			first.scalingType = ScalingType::Fit;
			first.scale = { 1.f, 1.f };
		} else {
			// Кратность зашита в эффекте (ACNet - ровно 2x): масштаб от профиля ему не задаём, до кадра доводит
			// нормализующая стадия
			first.scalingType = ScalingType::Normal;
			first.scale = { 1.f, 1.f };
		}
		if (deblur) {
			// Увеличение - с качеством VSR из профиля, если профиль и так на VSR
			if (_originalFirst[0].name == NVIDIA_VFX_VSR_ID) {
				first.parameters = _originalFirst[0].parameters;
			}
			// Сначала Deblur в исходном размере (category 4 - Deblur, strength 3 - самый сильный), потом увеличение VSR.
			// Между ними копия Nearest 1:1: выход одного VSR нельзя второй раз привязать к CUDA входом другого
			// (NvCVImage_InitFromD3D11Texture: invalid resource handle)
			EffectOption deblurStage{ .name = std::string(NVIDIA_VFX_VSR_ID) };
			deblurStage.parameters["category"] = 4.f;
			deblurStage.parameters["strength"] = 3.f;
			effects.insert(effects.begin(), { std::move(deblurStage), EffectOption{ .name = "Nearest" } });
		}
		displayName = UpscalerDisplayName(_upscalerCycle[next]);
	}

	if (!ScalingService::Get().RestartPhotoEffects(std::move(effects))) {
		Logger::Get().Error("Галерея: перегрузка цепи эффектов не удалась");
		return;
	}
	_cyclePos = next;

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

// Папка избранного для фото: из настроек, а по умолчанию - favorite рядом с папкой фото (..\favorite)
static std::filesystem::path FavoriteFolderFor(const std::filesystem::path& photo) {
	if (const auto& custom = AppSettings::Get().GalleryFavoriteFolder(); !custom.empty()) {
		return custom;
	}
	const std::filesystem::path folder = photo.parent_path();
	const std::filesystem::path parent = folder.parent_path();
	// У папки в корне диска родителя нет - тогда favorite внутри неё
	return (parent.empty() || parent == folder ? folder : parent) / L"favorite";
}

// Переместить фото в избранное; занятое имя получает суффикс « (2)», « (3)»...
static bool MoveToFavorites(const std::filesystem::path& photo) {
	const std::filesystem::path folder = FavoriteFolderFor(photo);
	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	if (ec) {
		Logger::Get().Error(fmt::format("Галерея: папка избранного не создана: {} ({})",
			StrHelper::UTF16ToUTF8(folder.native()), ec.message()));
		return false;
	}
	std::filesystem::path target = folder / photo.filename();
	for (int n = 2; std::filesystem::exists(target, ec); ++n) {
		target = folder / (photo.stem().native() + L" (" + std::to_wstring(n) + L")" + photo.extension().native());
	}
	if (!MoveFileExW(photo.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED)) {
		Logger::Get().Win32Error(fmt::format("Галерея: фото не перемещено в избранное: {}",
			StrHelper::UTF16ToUTF8(target.native())));
		return false;
	}
	Logger::Get().Info(fmt::format("Галерея: фото перемещено в избранное — {}", StrHelper::UTF16ToUTF8(target.native())));
	return true;
}

// Delete: файл текущей страницы уходит в корзину (FOF_ALLOWUNDO — случайное удаление
// отменяется проводником); Insert (toFavorites): файл перемещается в папку избранного. Дальше одинаково:
// плейлист сжимается на него, показ продолжается следующим фото; на последней странице — сдвиг назад;
// альбом опустел — показ останавливаем.
void GalleryService::_DeleteCurrentPhoto(bool toFavorites) {
	if (_deleting) return;
	_deleting = true;
	// SHFileOperationW разворачивает модальный насос сообщений: LL-хуки и таймер
	// живы и реентерили бы навигацию/удаление, сдвигая _index под внешним erase
	// (стирание не того файла, а при опустевшем плейлисте — erase за end()).
	const auto clearDeleting = wil::scope_exit([this] { _deleting = false; });
	if (_playlist.empty()) return;
	const std::wstring path = _playlist[_index];

	if (toFavorites) {
		if (!MoveToFavorites(path)) return;
	} else {
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
	if (!toFavorites) {
		Logger::Get().Info(fmt::format("Галерея: файл удалён в корзину — {}", StrHelper::UTF16ToUTF8(path)));
	}
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
			_ResourceString(toFavorites ? L"Gallery_FavoriteToast" : L"Gallery_DeletedToast"), scalingWindow);
	}
}

// Контекстное меню: апскейлеры (оригинал профиля сверху - то, с чем запускали сессию, ниже доступные
// способы), затем пауза, удаление и выход. Системное меню даёт мышь и клавиатуру само. Выбор применяется
// после закрытия меню.
void GalleryService::_SaveCurrentAs(HWND owner) {
	if (_playlist.empty()) return;
	const std::wstring source = _playlist[_index];
	com_ptr<IFileSaveDialog> dialog = try_create_instance<IFileSaveDialog>(CLSID_FileSaveDialog);
	if (!dialog) {
		Logger::Get().Error("Галерея: диалог сохранения не создан");
		return;
	}
	const std::wstring filterName = _ResourceString(L"Gallery_SaveAs_Filter");
	const COMDLG_FILTERSPEC filter{ filterName.c_str(), L"*.png" };
	dialog->SetFileTypes(1, &filter);
	dialog->SetDefaultExtension(L"png");
	dialog->SetOptions(FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
	const std::filesystem::path sourcePath(source);
	dialog->SetFileName((sourcePath.stem().native() + L"_upscaled.png").c_str());
	com_ptr<IShellItem> folder;
	if (SUCCEEDED(SHCreateItemFromParsingName(sourcePath.parent_path().c_str(), nullptr, IID_PPV_ARGS(folder.put())))) {
		dialog->SetFolder(folder.get());
	}

	// Пока диалог открыт, хуки галереи пропускают ввод (как при открытом меню)
	_menuOpen = true;
	const HRESULT shown = dialog->Show(owner);
	_menuOpen = false;
	if (FAILED(shown)) return;  // отмена - тоже FAILED (ERROR_CANCELLED)

	com_ptr<IShellItem> item;
	wil::unique_cotaskmem_string path;
	if (FAILED(dialog->GetResult(item.put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, path.put()))) {
		Logger::Get().Error("Галерея: путь сохранения не получен");
		return;
	}
	Logger::Get().Info(fmt::format("Галерея: «Сохранить как» {}", StrHelper::UTF16ToUTF8(path.get())));
	ScalingService::Get().SavePhotoUpscaled(path.get(), source);
}

void GalleryService::_ShowContextMenu(POINT screenPoint, bool atCursor) {
	using namespace gallery_menu;
	if (!_running || _menuOpen) return;
	const HWND stage = FindWindowW(ScalingService::PHOTO_STAGE_CLASS_NAME, nullptr);
	if (!stage) return;

	wil::unique_hmenu menu(CreatePopupMenu());
	if (!menu) {
		Logger::Get().Win32Error("Меню галереи не создано");
		return;
	}
	if (_EnsureUpscalerSnapshot()) {
		AppendMenuW(menu.get(), MF_STRING | MF_DISABLED, 0, _ResourceString(L"Gallery_PickerHeader").c_str());
		AppendMenuW(menu.get(), MF_STRING, ID_UPSCALER_BASE, UpscalerDisplayName(_originalFirst[0].name).c_str());
		for (size_t i = 0; i < _upscalerCycle.size(); ++i) {
			AppendMenuW(menu.get(), MF_STRING, ID_UPSCALER_BASE + 1 + static_cast<UINT>(i),
				UpscalerDisplayName(_upscalerCycle[i]).c_str());
		}
		CheckMenuRadioItem(menu.get(), ID_UPSCALER_BASE, ID_UPSCALER_BASE + static_cast<UINT>(_upscalerCycle.size()),
			ID_UPSCALER_BASE + static_cast<UINT>(_cyclePos + 1), MF_BYCOMMAND);
		AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
	}
	AppendMenuW(menu.get(), MF_STRING, ID_PAUSE,
		_ResourceString(_paused ? L"Gallery_Menu_Resume" : L"Gallery_Menu_Pause").c_str());
	AppendMenuW(menu.get(), MF_STRING, ID_SAVE_AS, _ResourceString(L"Gallery_Menu_SaveAs").c_str());
	AppendMenuW(menu.get(), MF_STRING, ID_FAVORITE, _ResourceString(L"Gallery_Menu_Favorite").c_str());
	AppendMenuW(menu.get(), MF_STRING, ID_DELETE, _ResourceString(L"Gallery_Menu_Delete").c_str());
	AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
	AppendMenuW(menu.get(), MF_STRING, ID_STOP, _ResourceString(L"Gallery_Menu_Stop").c_str());

	// Окно-владелец обязано быть на переднем плане, иначе меню не закрывается кликом мимо
	_menuOpen = true;
	// Захват курсора Magpie держит реальный курсор в координатах источника - клик мышью не попадал в пункты меню.
	// На время меню курсор отпускается; поток окна масштабирования применяет это асинхронно, поэтому ждём его
	// и ставим меню у настоящего курсора (у Num6 точка - центр кадра)
	ScalingService::Get().SetPhotoMenuOpen(true);
	const auto releaseCursor = wil::scope_exit([] { ScalingService::Get().SetPhotoMenuOpen(false); });
	Sleep(50);
	if (atCursor) GetCursorPos(&screenPoint);
	SetForegroundWindow(stage);
	const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu.get(),
		TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
		screenPoint.x, screenPoint.y, stage, nullptr));
	PostMessageW(stage, WM_NULL, 0, 0);
	_menuOpen = false;

	if (!_running) return;  // показ могли остановить, пока меню было открыто
	if (command >= ID_UPSCALER_BASE && command < ID_PAUSE) {
		_ApplyUpscaler(static_cast<int>(command - ID_UPSCALER_BASE) - 1);
	} else if (command == ID_PAUSE) {
		_SetPaused(!_paused);
	} else if (command == ID_SAVE_AS) {
		_SaveCurrentAs(stage);
	} else if (command == ID_FAVORITE) {
		_DeleteCurrentPhoto(true);
	} else if (command == ID_DELETE) {
		_DeleteCurrentPhoto();
	} else if (command == ID_STOP) {
		Stop();
	}
}

void GalleryService::_Cleanup() noexcept {
	// состояние прошлой сессии сохраняем до того, как всё сброшено
	if (_running) {
		_PersistSession(true);
	}
	try {
		gallery_status::Hide();
		_keyboardHook.reset();
		_mouseHook.reset();
		if (_autoTimer) {
			_autoTimer.Stop();
			_autoTimer = nullptr;
		}
		if (_statusRetryTimer) {
			_statusRetryTimer.Stop();
			_statusRetryTimer = nullptr;
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

// Что делает клавиша галереи — вся карта здесь (тип — в GalleryService.h), проверяется
// тестами на CPU. Список намеренно короткий: всё остальное (включая Esc в чужих
// приложениях) должно доставаться обычным программам.
static GalleryKeyAction DecideGalleryKey(unsigned vkCode) noexcept {
	switch (vkCode) {
	case VK_LEFT: case VK_UP:
		return GalleryKeyAction::PagePrev;
	case VK_RIGHT: case VK_DOWN:
		return GalleryKeyAction::PageNext;
	case VK_HOME:
		return GalleryKeyAction::First;
	case VK_END:
		return GalleryKeyAction::Last;
	case VK_SPACE:
		return GalleryKeyAction::TogglePause;
	case VK_NUMPAD6:
		return GalleryKeyAction::OpenMenu;
	case VK_DELETE:
		return GalleryKeyAction::DeletePhoto;
	case VK_INSERT:
		return GalleryKeyAction::FavoritePhoto;
	case VK_ESCAPE:
	case VK_F11:
		// F11 как Esc: пользователь ждёт от F11 (полноэкранный) выхода из просмотра, а «чужой»
		// полноэкранный во время показа недоступен в принципе — перехват ничего не отнимает
		return GalleryKeyAction::Stop;
	default:
		return GalleryKeyAction::PassThrough;
	}
}

LRESULT CALLBACK GalleryService::_KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
	auto& self = Get();
	// Пока открыто контекстное меню, клавиши (стрелки, Enter, Esc) принадлежат ему
	if (nCode >= 0 && self._running && !self._menuOpen &&
		(wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
		const auto action = DecideGalleryKey(((KBDLLHOOKSTRUCT*)lParam)->vkCode);
		// хоткеи принадлежат галерее только пока она в фокусе — иначе клавиши
		// (включая Esc) достаются обычным приложениям
		if (action != GalleryKeyAction::PassThrough && self._IsSessionForeground()) {
			self._OnKeyAction(action);
			return 1;
		}
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK GalleryService::_MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
	auto& self = Get();
	if (nCode >= 0 && self._running && !self._menuOpen) {
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
			// Открытое меню лежит поверх фото: его клик не «возврат фокуса», иначе пункт мышью не выбрать
			if (!self._IsSessionForeground() &&
				self._TryFocusFromPhotoClick(((MSLLHOOKSTRUCT*)lParam)->pt)) {
				return 1;
			}
		} else if ((wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP) &&
			(self._IsSessionForeground() || self._IsClickOnPhoto(((MSLLHOOKSTRUCT*)lParam)->pt))) {
			// Правая кнопка открывает контекстное меню. Меню модальное, а LL-хуку нельзя блокироваться
			// (LowLevelHooksTimeout отключит хук), поэтому открываем его из очереди диспетчера.
			// Правый клик по фото вне фокуса тоже открывает меню: раньше он молча уходил окну под фото
			if (wParam == WM_RBUTTONUP) {
				const POINT pt = ((MSLLHOOKSTRUCT*)lParam)->pt;
				if (!self._IsSessionForeground()) self._TryFocusFromPhotoClick(pt);
				App::Get().Dispatcher().TryEnqueue([pt] { GalleryService::Get()._ShowContextMenu(pt, true); });
			}
			return 1;
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
	if (!_IsClickOnPhoto(pt)) return false;
	const HWND stage = FindWindowW(ScalingService::PHOTO_STAGE_CLASS_NAME, nullptr);

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

// Клик пришёлся на само фото, а не на окна нашего процесса (меню, тулбар) или topmost-окна поверх него
bool GalleryService::_IsClickOnPhoto(POINT pt) const noexcept {
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
	case GalleryKeyAction::OpenMenu:
	{
		// Num6: меню по центру кадра (у правой кнопки точка - курсор). Из хука нельзя блокироваться - через диспетчер
		const RECT frame = PhotoFrameRect();
		const POINT center{ (frame.left + frame.right) / 2, (frame.top + frame.bottom) / 3 };
		App::Get().Dispatcher().TryEnqueue([center] { GalleryService::Get()._ShowContextMenu(center, false); });
		break;
	}
	case GalleryKeyAction::DeletePhoto:
		_DeleteCurrentPhoto();
		break;
	case GalleryKeyAction::FavoritePhoto:
		_DeleteCurrentPhoto(true);
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
