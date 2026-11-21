#include "pch.h"
#include "PhotoFrameSource.h"
#include "BackendDescriptorStore.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <robuffer.h>
#include <fstream>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

namespace GMagpie {

namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wstorage = winrt::Windows::Storage;

void PhotoFrameSource::RequestLoad(std::wstring path) {
	{
		std::scoped_lock lock(_requestMutex);
		_pendingPath = std::move(path);
	}
	SetEvent(_loadArrived.get());
}

bool PhotoFrameSource::HasPendingLoad() const noexcept {
	std::scoped_lock lock(_requestMutex);
	return !_pendingPath.empty();
}

bool PhotoFrameSource::RequiresRebind() noexcept {
	// Только поток захвата; чтение со сбросом
	const bool required = _rebindRequired;
	_rebindRequired = false;
	return required;
}

bool PhotoFrameSource::CopyAnimationFrame(uint64_t page, uint64_t& serial, std::vector<uint8_t>& pixels,
	uint32_t width, uint32_t height) noexcept {
	std::scoped_lock lock(_animationFrameMutex);
	if (_animationFramePage != page || _animationFrameSerial == serial ||
		_animationFrame.size() != size_t(width) * height * 4) {
		return false;
	}
	pixels = _animationFrame;
	serial = _animationFrameSerial;
	return true;
}

PhotoFrameSource::~PhotoFrameSource() {
	if (_animationTimer) {
		SetThreadpoolTimer(_animationTimer, nullptr, 0, 0);
		WaitForThreadpoolTimerCallbacks(_animationTimer, TRUE);
		CloseThreadpoolTimer(_animationTimer);
	}
	if (_prefetchThread.joinable()) {
		{
			std::scoped_lock lock(_prefetchMutex);
			_prefetchStop = true;
			_prefetchRequest.clear();
		}
		if (_prefetchArrived) {
			SetEvent(_prefetchArrived.get());
		}
		_prefetchThread.join();
	}
}

void PhotoFrameSource::RequestPhotoPrefetch(std::wstring path) noexcept {
	try {
		if (!_prefetchArrived) {
			if (FAILED(_prefetchArrived.create(wil::EventOptions::ManualReset))) {
				return;
			}
			_prefetchThread = std::thread([this] { _PrefetchThreadProc(); });
		}
		bool notify = false;
		{
			std::scoped_lock lock(_prefetchMutex);
			if (_prefetchStop) {
				return;
			}
			// Уже предзагружено или тот же запрос в работе — нечего делать
			if (_prefetched.path == path || _prefetchRequest == path) {
				return;
			}
			_prefetchRequest = std::move(path);
			notify = true;
		}
		if (notify) {
			// ManualReset + сброс потребителем: лишние установки дадут потоку пустую проверку
			SetEvent(_prefetchArrived.get());
		}
	} catch (...) {
		// Предзагрузка — оптимизация «по возможности»: любой её сбой не влияет на показ
	}
}

void PhotoFrameSource::_PrefetchThreadProc() {
	try {
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	} catch (...) {
	}

	while (true) {
		_prefetchArrived.wait();
		{
			std::scoped_lock lock(_prefetchMutex);
			if (_prefetchStop) {
				break;
			}
		}

		std::wstring path;
		{
			std::scoped_lock lock(_prefetchMutex);
			// Сначала сброс, потом забор задачи — иначе SetEvent между этими
			// действиями потеряется, и задача простоит до следующего запроса
			ResetEvent(_prefetchArrived.get());
			path.swap(_prefetchRequest);
		}
		if (path.empty()) {
			continue;
		}

		// Поток бэкенда уже грузит ту же страницу — второй декод не нужен
		if (HasPendingLoad() && [&] { std::scoped_lock lock(_requestMutex); return _pendingPath == path; }()) {
			continue;
		}

		// Декодированное фото впрок - до сотни МБ; при нехватке оперативной памяти страница декодируется при показе
		if (Win32Helper::IsSystemMemoryTight()) {
			Logger::Get().Info("Фото: оперативной памяти мало - предзагрузка следующей страницы пропущена");
			continue;
		}
		Canvas canvas;
		if (_DecodeCanvas(path, canvas)) {
			const uint32_t width = canvas.width, height = canvas.height;
			std::function<void(uint32_t, uint32_t)> listener;
			{
				std::scoped_lock lock(_prefetchMutex);
				if (_prefetchStop) continue;
				_prefetched = std::move(canvas);
				listener = _prefetchListener;
			}
			// Вне блокировки: подготовка модели под эту страницу занимает секунды
			if (listener) listener(width, height);
		}
	}
}

bool PhotoFrameSource::_Initialize() noexcept {
	const auto& photo = ScalingWindow::Get().Options().photoSession;
	if (!photo || photo->firstFile.empty()) {
		Logger::Get().Error("В фото-сессии не задан начальный файл");
		return false;
	}

	if (!_loadArrived) {
		// ManualReset: после потребления _Update сбрасывает его ResetEvent (с auto-reset
		// этот вызов падал бы). create() возвращает HRESULT (успех — S_OK), а не bool.
		if (FAILED(_loadArrived.create(wil::EventOptions::ManualReset))) {
			Logger::Get().Win32Error("Не удалось создать событие загрузки фотографии");
			return false;
		}
	}

	// Первый кадр декодируем синхронно (переиспользовать нечего), дальше — предзагрузка
	std::wstring path = photo->firstFile;
	Canvas canvas;
	if (!_DecodeCanvas(path, canvas)) {
		return false;
	}
	if (!_EnsureOutputTexture(canvas.width, canvas.height)) {
		return false;
	}
	_deviceResources->GetD3DDC()->UpdateSubresource(
		_output.get(), 0, nullptr, canvas.pixels.data(), canvas.width * 4, 0);
	_currentPath = std::move(path);
	_StartAnimation(_currentPath);
	// Кадр уже в текстуре, но для рендерера новый источник наступает только вместе с
	// NewFrame из _Update: там же пересчитываются цепь эффектов под размер фотографии
	// и целевой прямоугольник. Без этого шага первая страница не показывалась до
	// первого перелистывания (в логе — ни одной «Capture sequence accepted» до второй
	// фотографии), а назад к ней приходилось уже по обычному пути.
	_firstFramePending = true;
	return true;
}

// «Кадр готов и стал новым источником». Одна и та же последовательность для первой
// фотографии (декод в _Initialize) и для всех последующих (_Update): общие слоты,
// история кадров и эффект-цепь судят о неизменности источника по resourceGeneration и
// rebind, поэтому пропускать её нельзя ни для одной из страниц.
void PhotoFrameSource::_MarkNewPresentation() noexcept {
	++_resourceGeneration;
	_rebindRequired = true;
	_captureInterrupted = false;
	++_captureSequence;
	_captureTimestamp100ns = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

FrameSourceState PhotoFrameSource::_Update() noexcept {
	std::wstring path;
	{
		std::scoped_lock lock(_requestMutex);
		if (_pendingPath.empty()) {
			if (_firstFramePending) {
				// Первая фотография: картинка уже в _output (декод в _Initialize),
				// заново её не декодируем — только сообщаем рендереру о новом источнике,
				// чтобы цепь и целевой прямоугольник были посчитаны по ней, как для
				// любой другой страницы.
				_firstFramePending = false;
				_MarkNewPresentation();
				return FrameSourceState::NewFrame;
			}
			if (!_animationDue.exchange(false) || !_gif.frameCount) {
				return FrameSourceState::Waiting;
			}
			// Под блокировкой: SetEvent нового запроса страницы придёт только после неё и не потеряется
			ResetEvent(_loadArrived.get());
		} else {
			path.swap(_pendingPath);
		}
	}
	if (path.empty()) {
		// Следующий кадр GIF той же страницы: цепь того же размера переиспользуется без перестройки
		_BeginCaptureRender();
		uint32_t delayMs = 100;
		try {
			delayMs = _ComposeGifFrame(_gif);
		} catch (const winrt::hresult_error& e) {
			Logger::Get().ComError("Фото: кадр GIF не собран - анимация остановлена", e.code());
			_StopAnimation();
			return FrameSourceState::Waiting;
		}
		_deviceResources->GetD3DDC()->UpdateSubresource(
			_output.get(), 0, nullptr, _gif.canvas.data(), _gif.width * 4, 0);
		{
			std::scoped_lock lock(_animationFrameMutex);
			_animationFrame = _gif.canvas;
			_animationFramePage = PhotoPageId();
			++_animationFrameSerial;
		}
		_ScheduleAnimation(delayMs);
		_MarkNewPresentation();
		return FrameSourceState::NewFrame;
	}
	// Запрос потреблён, снова ожидание (reset() у unique handle закрыл бы дескриптор)
	ResetEvent(_loadArrived.get());

	_BeginCaptureRender();

	Canvas canvas;
	{
		std::scoped_lock lock(_prefetchMutex);
		if (_prefetched.path == path) {
			canvas = std::move(_prefetched);
			_prefetched.path.clear();
		}
	}
	const bool fromPrefetch = !canvas.path.empty();
	if (!fromPrefetch) {
		// Предзагрузка не попала (ручной переход): декод на месте; при неудаче — прежняя страница
		if (!_DecodeCanvas(path, canvas)) {
			return FrameSourceState::Waiting;
		}
	}

	if (!_EnsureOutputTexture(canvas.width, canvas.height)) {
		return FrameSourceState::Waiting;
	}

	_deviceResources->GetD3DDC()->UpdateSubresource(
		_output.get(), 0, nullptr, canvas.pixels.data(), canvas.width * 4, 0);

	// Новая фотография — новое содержимое источника, надо сообщить вниз: и общие
	// слоты текстур, и история кадров цепи судят о неизменности источника по
	// resourceGeneration / rebind; без флагов изображение застыло бы на прошлой
	// странице (наблюдаемый симптом: счётчик листает, кадры в логе принимаются, а
	// на экране одна фотография). При том же размере _EnsureOutputTexture текстуру
	// не пересоздаёт — здесь только повторный забор pipeline-текстуры и сброс
	// истории цепи, без декодирования.
	_currentPath = std::move(path);
	_StartAnimation(_currentPath);
	_MarkNewPresentation();
	return FrameSourceState::NewFrame;
}

bool PhotoFrameSource::DecodeCanvas(const std::wstring& path, Canvas& out, SIZE fitTo) noexcept {
	std::string pathUtf8 = StrHelper::UTF16ToUTF8(path);
	try {
		auto file = wstorage::StorageFile::GetFileFromPathAsync(path.c_str()).get();
		auto stream = file.OpenAsync(wstorage::FileAccessMode::Read).get();
		auto decoder = wgi::BitmapDecoder::CreateAsync(stream).get();

		// Анимированный GIF: первый кадр собирается так же, как дальше при проигрывании (кадры GIF бывают
		// частичными), в размере логического экрана GIF. fitTo не применяется - анимации обычно небольшие
		if (GifPlayer gif; _OpenGif(decoder, gif)) {
			_ComposeGifFrame(gif);
			out.pixels = std::move(gif.canvas);
			out.width = gif.width;
			out.height = gif.height;
			out.path = path;
			Logger::Get().Info(fmt::format("Анимированный GIF декодирован: {} ({}x{}, кадров {})",
				pathUtf8, gif.width, gif.height, gif.frameCount));
			return true;
		}

		// Ориентация по EXIF и единый BGRA8 (он же DXGI B8G8R8A8) — тот формат,
		// который цепь эффектов получает от всех источников захвата; профиль — sRGB.
		// AlphaMode::Ignore: фотография непрозрачна, а цепь и представление считают
		// B8G8R8A8 straight (не premultiplied). От premultiplied или webp с альфой
		// полупрозрачные области подмешают предыдущий кадр и исказят цвет. Ниже
		// alpha принудительно выставляется в 255.
		wgi::BitmapTransform transform;
		if (fitTo.cx > 0 && fitTo.cy > 0) {
			// Масштаб один на обе стороны, поэтому его можно считать по ориентированному размеру, а применять
			// к неповёрнутому - порядок поворота и масштабирования в декодере не важен
			const double scale = std::min(double(fitTo.cx) / decoder.OrientedPixelWidth(),
				double(fitTo.cy) / decoder.OrientedPixelHeight());
			if (scale < 1.0) {
				transform.ScaledWidth(std::max(1u, uint32_t(std::lround(decoder.PixelWidth() * scale))));
				transform.ScaledHeight(std::max(1u, uint32_t(std::lround(decoder.PixelHeight() * scale))));
				transform.InterpolationMode(wgi::BitmapInterpolationMode::Fant);
			}
		}
		wgi::SoftwareBitmap bitmap = decoder.GetSoftwareBitmapAsync(
			wgi::BitmapPixelFormat::Bgra8, wgi::BitmapAlphaMode::Ignore,
			transform, wgi::ExifOrientationMode::RespectExifOrientation,
			wgi::ColorManagementMode::ColorManageToSRgb).get();
		if (bitmap.BitmapPixelFormat() != wgi::BitmapPixelFormat::Bgra8) {
			bitmap = wgi::SoftwareBitmap::Convert(bitmap, wgi::BitmapPixelFormat::Bgra8);
		}

		const uint32_t width = static_cast<uint32_t>(bitmap.PixelWidth());
		const uint32_t height = static_cast<uint32_t>(bitmap.PixelHeight());
		if (width == 0 || height == 0 ||
			width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
			Logger::Get().Error(fmt::format("Размер фотографии вне поддерживаемого: {}x{} ({})", width, height, pathUtf8));
			return false;
		}

		// Пиксели читаются по реальному шагу строки: плоскость SoftwareBitmap может
		// быть выровнена, а «width*4» дало бы пошаговый сдвиг. WinRT-проекция этого
		// проекта не экспортирует BitmapStride/CopyPixels, поэтому читаем напрямую
		// через LockBuffer + описание плоскости, как в IconHelper/AppXReader.
		wgi::BitmapBuffer planeBuffer = bitmap.LockBuffer(wgi::BitmapBufferAccessMode::Read);
		const wgi::BitmapPlaneDescription plane = planeBuffer.GetPlaneDescription(0);
		if (plane.Width != static_cast<int32_t>(width) || plane.Height != static_cast<int32_t>(height)) {
			Logger::Get().Error(fmt::format("Неожиданный размер плоскости: {}x{} (ожидалось {}x{}) ({})",
				plane.Width, plane.Height, width, height, pathUtf8));
			return false;
		}
		if (plane.Stride <= 0 || static_cast<uint32_t>(plane.Stride) < width * 4) {
			Logger::Get().Error(fmt::format("Неожиданный шаг строки: {} ({}x{}) ({})", plane.Stride, width, height, pathUtf8));
			return false;
		}
		const uint32_t stride = static_cast<uint32_t>(plane.Stride);
		// Ссылку держим живой всё время чтения, иначе память под указателем разблокируют
		auto bufferReference = planeBuffer.CreateReference();
		uint8_t* pixels = bufferReference.data();
		if (!pixels) {
			Logger::Get().Error(fmt::format("Указатель пикселей пуст ({})", pathUtf8));
			return false;
		}

		// Фотография входит в цепь в своём размере: у эффектов с кратностью коэффициент
		// зашит в выборку UV, и если заранее привести кадр к другому размеру (или к
		// экрану), пропорция ломается — это сдвиг увеличения и обрезка. Вписыванием в
		// экран занимается финальный масштаб представления.
		out.pixels.resize((size_t)width * height * 4);
		for (uint32_t y = 0; y < height; ++y) {
			const uint8_t* src = pixels + (size_t)y * stride;
			uint8_t* dst = out.pixels.data() + (size_t)y * width * 4;
			memcpy(dst, src, (size_t)width * 4);
			// Непрозрачно: в webp альфа-канал может быть заполнен, ниже ожидается straight
			for (uint32_t x = 3; x < width * 4; x += 4) {
				dst[x] = 0xFF;
			}
		}

		out.width = width;
		out.height = height;
		out.path = path;

		Logger::Get().Info(fmt::format("Фотография декодирована: {} ({}x{})", pathUtf8, width, height));
		return true;
	} catch (const winrt::hresult_error& e) {
		Logger::Get().ComError(fmt::format("Не удалось декодировать фотографию ({})", pathUtf8), e.code());
		return false;
	} catch (...) {
		Logger::Get().Error(fmt::format("Не удалось декодировать фотографию ({}), неизвестное исключение", pathUtf8));
		return false;
	}
}

// Значение свойства из метаданных GIF; ключа нет - fallback
template <typename T>
static T _GifProperty(const wgi::BitmapPropertySet& props, const wchar_t* key, T fallback) {
	return props.HasKey(key) ? winrt::unbox_value_or<T>(props.Lookup(key).Value(), fallback) : fallback;
}

bool PhotoFrameSource::_OpenGif(const wgi::BitmapDecoder& decoder, GifPlayer& player) {
	if (decoder.DecoderInformation().CodecId() != wgi::BitmapDecoder::GifDecoderId() || decoder.FrameCount() < 2) {
		return false;
	}
	const auto props = decoder.BitmapContainerProperties().GetPropertiesAsync(
		std::vector<winrt::hstring>{ L"/logscrdesc/Width", L"/logscrdesc/Height" }).get();
	player.decoder = decoder;
	player.frameCount = decoder.FrameCount();
	player.width = _GifProperty<uint16_t>(props, L"/logscrdesc/Width", uint16_t(decoder.PixelWidth()));
	player.height = _GifProperty<uint16_t>(props, L"/logscrdesc/Height", uint16_t(decoder.PixelHeight()));
	player.index = 0;
	return player.width && player.height;
}

bool PhotoFrameSource::_OpenWebp(const wgi::BitmapDecoder& decoder, const std::wstring& path, GifPlayer& player) {
	if (decoder.DecoderInformation().CodecId() != wgi::BitmapDecoder::WebpDecoderId() || decoder.FrameCount() < 2) {
		return false;
	}
	// RIFF: "RIFF" size "WEBP", затем чанки (FourCC, размер LE, данные с выравниванием до чётного).
	// ANMF: X(3) Y(3) W-1(3) H-1(3) Duration(3, мс) ...
	std::ifstream file(path, std::ios::binary);
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	const auto u24 = [&](size_t at) { return uint32_t(data[at]) | uint32_t(data[at + 1]) << 8 | uint32_t(data[at + 2]) << 16; };
	for (size_t at = 12; at + 8 <= data.size();) {
		const uint32_t size = u24(at + 4) | uint32_t(data[at + 7]) << 24;
		if (memcmp(&data[at], "ANMF", 4) == 0 && at + 8 + 15 <= data.size()) {
			const uint32_t duration = u24(at + 8 + 12);
			// Как в браузерах: слишком короткая длительность означает «по умолчанию», 100 мс
			player.webpDelays.push_back(duration < 20 ? 100 : duration);
		}
		at += 8 + size + (size & 1);
	}
	player.decoder = decoder;
	player.frameCount = decoder.FrameCount();
	player.width = decoder.PixelWidth();
	player.height = decoder.PixelHeight();
	player.index = 0;
	player.webpDelays.resize(player.frameCount, 100);
	return true;
}

uint32_t PhotoFrameSource::_ComposeGifFrame(GifPlayer& player) {
	const uint32_t rowBytes = player.width * 4;
	if (!player.webpDelays.empty()) {
		// WebP: кадр уже полный, просто копия непрозрачного BGRA
		const wgi::BitmapFrame frame = player.decoder.GetFrameAsync(player.index).get();
		const wgi::SoftwareBitmap bitmap = frame.GetSoftwareBitmapAsync(wgi::BitmapPixelFormat::Bgra8,
			wgi::BitmapAlphaMode::Ignore, wgi::BitmapTransform(), wgi::ExifOrientationMode::IgnoreExifOrientation,
			wgi::ColorManagementMode::ColorManageToSRgb).get();
		const wgi::BitmapBuffer buffer = bitmap.LockBuffer(wgi::BitmapBufferAccessMode::Read);
		const wgi::BitmapPlaneDescription plane = buffer.GetPlaneDescription(0);
		auto reference = buffer.CreateReference();
		player.canvas.resize(size_t(rowBytes) * player.height);
		const uint32_t rows = std::min<uint32_t>(player.height, plane.Height);
		const size_t bytes = std::min<size_t>(rowBytes, size_t(plane.Width) * 4);
		for (uint32_t y = 0; y < rows; ++y) {
			uint8_t* dst = player.canvas.data() + size_t(y) * rowBytes;
			memcpy(dst, reference.data() + plane.StartIndex + size_t(y) * plane.Stride, bytes);
			for (size_t x = 3; x < bytes; x += 4) {
				dst[x] = 0xFF;
			}
		}
		const uint32_t delay = player.webpDelays[player.index];
		player.index = (player.index + 1) % player.frameCount;
		return delay;
	}
	const auto fillRect = [&](const RECT& r, const uint8_t* from) {
		for (LONG y = r.top; y < r.bottom; ++y) {
			uint8_t* row = player.canvas.data() + size_t(y) * rowBytes + size_t(r.left) * 4;
			const size_t bytes = size_t(r.right - r.left) * 4;
			if (from) {
				memcpy(row, from, bytes);
				from += bytes;
			} else {
				for (size_t x = 0; x < bytes; x += 4) {
					row[x] = row[x + 1] = row[x + 2] = 0;
					row[x + 3] = 0xFF;
				}
			}
		}
	};

	// Начало цикла - чистый холст; иначе убрать предыдущий кадр так, как он просил
	if (player.index == 0) {
		player.canvas.resize(size_t(rowBytes) * player.height);
		fillRect({ 0, 0, LONG(player.width), LONG(player.height) }, nullptr);
	} else if (player.disposal == 2) {
		fillRect(player.rect, nullptr);
	} else if (player.disposal == 3 && !player.saved.empty()) {
		fillRect(player.rect, player.saved.data());
	}

	const wgi::BitmapFrame frame = player.decoder.GetFrameAsync(player.index).get();
	const auto props = frame.BitmapProperties().GetPropertiesAsync(std::vector<winrt::hstring>{
		L"/imgdesc/Left", L"/imgdesc/Top", L"/grctlext/Delay", L"/grctlext/Disposal" }).get();
	const LONG left = _GifProperty<uint16_t>(props, L"/imgdesc/Left", 0);
	const LONG top = _GifProperty<uint16_t>(props, L"/imgdesc/Top", 0);
	const uint32_t delay = _GifProperty<uint16_t>(props, L"/grctlext/Delay", 0) * 10u;
	player.disposal = _GifProperty<uint8_t>(props, L"/grctlext/Disposal", 0);

	// Straight-альфа: прозрачные пиксели кадра GIF оставляют под собой прошлое содержимое холста
	const wgi::SoftwareBitmap bitmap = frame.GetSoftwareBitmapAsync(wgi::BitmapPixelFormat::Bgra8,
		wgi::BitmapAlphaMode::Straight, wgi::BitmapTransform(), wgi::ExifOrientationMode::IgnoreExifOrientation,
		wgi::ColorManagementMode::DoNotColorManage).get();
	const LONG frameWidth = bitmap.PixelWidth(), frameHeight = bitmap.PixelHeight();
	player.rect = {
		std::clamp(left, 0L, LONG(player.width)), std::clamp(top, 0L, LONG(player.height)),
		std::clamp(left + frameWidth, 0L, LONG(player.width)), std::clamp(top + frameHeight, 0L, LONG(player.height))
	};
	const RECT& r = player.rect;
	if (player.disposal == 3) {
		player.saved.resize(size_t(r.right - r.left) * (r.bottom - r.top) * 4);
		uint8_t* dst = player.saved.data();
		for (LONG y = r.top; y < r.bottom; ++y, dst += size_t(r.right - r.left) * 4) {
			memcpy(dst, player.canvas.data() + size_t(y) * rowBytes + size_t(r.left) * 4, size_t(r.right - r.left) * 4);
		}
	}

	const wgi::BitmapBuffer buffer = bitmap.LockBuffer(wgi::BitmapBufferAccessMode::Read);
	const wgi::BitmapPlaneDescription plane = buffer.GetPlaneDescription(0);
	auto reference = buffer.CreateReference();
	const uint8_t* pixels = reference.data() + plane.StartIndex;
	for (LONG y = r.top; y < r.bottom; ++y) {
		const uint8_t* src = pixels + size_t(y - top) * plane.Stride + size_t(r.left - left) * 4;
		uint8_t* dst = player.canvas.data() + size_t(y) * rowBytes + size_t(r.left) * 4;
		for (LONG x = r.left; x < r.right; ++x, src += 4, dst += 4) {
			// В GIF прозрачность двоичная
			if (src[3] >= 128) {
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
			}
		}
	}

	player.index = (player.index + 1) % player.frameCount;
	// Как в браузерах: задержка меньше 20 мс в файлах означает «по умолчанию», 100 мс
	return delay < 20 ? 100 : delay;
}

void PhotoFrameSource::_StartAnimation(const std::wstring& path) noexcept {
	_StopAnimation();
	try {
		// Декодер анимации живёт, пока страница на экране: файл держим в памяти, а не открытым на диске,
		// иначе его нельзя переместить в избранное или удалить в корзину во время проигрывания
		auto file = wstorage::StorageFile::GetFileFromPathAsync(path.c_str()).get();
		winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
		{
			auto input = file.OpenAsync(wstorage::FileAccessMode::Read).get();
			winrt::Windows::Storage::Streams::RandomAccessStream::CopyAsync(input, stream).get();
			input.Close();
		}
		stream.Seek(0);
		auto decoder = wgi::BitmapDecoder::CreateAsync(stream).get();
		if (!_OpenGif(decoder, _gif) && !_OpenWebp(decoder, path, _gif)) {
			_gif = {};
			return;
		}
		if (_gif.width != _canvasWidth || _gif.height != _canvasHeight) {
			Logger::Get().Error(fmt::format("Фото: размер GIF {}x{} не совпал с текстурой {}x{} - без анимации",
				_gif.width, _gif.height, _canvasWidth, _canvasHeight));
			_gif = {};
			return;
		}
		// Кадр 0 уже на экране (DecodeCanvas): собираем его ещё раз ради состояния холста и его задержки
		const uint32_t delayMs = _ComposeGifFrame(_gif);
		Logger::Get().Info(fmt::format("Фото: проигрывание анимации ({}), кадров {}",
			_gif.webpDelays.empty() ? "GIF" : "WebP", _gif.frameCount));
		_ScheduleAnimation(delayMs);
	} catch (const winrt::hresult_error& e) {
		Logger::Get().ComError("Фото: GIF не открыт для анимации", e.code());
		_gif = {};
	}
}

void PhotoFrameSource::_StopAnimation() noexcept {
	if (_animationTimer) {
		SetThreadpoolTimer(_animationTimer, nullptr, 0, 0);
		WaitForThreadpoolTimerCallbacks(_animationTimer, TRUE);
	}
	_animationDue = false;
	_gif = {};
}

void PhotoFrameSource::_ScheduleAnimation(uint32_t delayMs) noexcept {
	if (!_animationTimer) {
		_animationTimer = CreateThreadpoolTimer([](PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
			auto* self = static_cast<PhotoFrameSource*>(context);
			self->_animationDue = true;
			SetEvent(self->_loadArrived.get());
		}, this, nullptr);
		if (!_animationTimer) {
			Logger::Get().Win32Error("Фото: таймер анимации GIF не создан");
			return;
		}
	}
	// Отрицательное время - относительное, в 100 нс
	ULARGE_INTEGER due;
	due.QuadPart = ULONGLONG(-int64_t(delayMs) * 10000);
	FILETIME ft{ due.LowPart, due.HighPart };
	SetThreadpoolTimer(_animationTimer, &ft, 0, 0);
}

bool PhotoFrameSource::_EnsureOutputTexture(uint32_t width, uint32_t height) {
	if (_output && _outputSrv && _canvasWidth == width && _canvasHeight == height) {
		return true;
	}

	winrt::com_ptr<ID3D11Texture2D> texture = DirectXHelper::CreateTexture2D(
		_deviceResources->GetD3DDevice(),
		DXGI_FORMAT_B8G8R8A8_UNORM,
		width, height,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
	if (!texture) {
		Logger::Get().Error("Не удалось создать текстуру фотографии");
		return false;
	}

	ID3D11Texture2D* oldTexture = _output.get();
	_output = std::move(texture);
	_outputSrv = _descriptorStore->GetShaderResourceView(_output.get());
	if (!_outputSrv) {
		Logger::Get().Error("Не удалось получить SRV текстуры фотографии");
		// Текстуру без SRV не оставляем: тот же размер всё равно не попал бы в переиспользование
		_output = {};
		return false;
	}
	if (oldTexture && oldTexture != _output.get()) {
		_descriptorStore->RemoveCache(oldTexture);
	}

	_canvasWidth = width;
	_canvasHeight = height;
	++_textureSerial;
	// Новая текстура — рендерер обязан перепривязать цепь; на странице того же размера сюда не заходим
	++_resourceGeneration;
	_rebindRequired = true;
	return true;
}

}
