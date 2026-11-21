#include "pch.h"
#include "PhotoFrameSource.h"
#include "BackendDescriptorStore.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include <robuffer.h>
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

PhotoFrameSource::~PhotoFrameSource() {
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

		Canvas canvas;
		if (_DecodeCanvas(path, canvas)) {
			std::scoped_lock lock(_prefetchMutex);
			if (!_prefetchStop) {
				_prefetched = std::move(canvas);
			}
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
			return FrameSourceState::Waiting;
		}
		path.swap(_pendingPath);
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
	_MarkNewPresentation();
	return FrameSourceState::NewFrame;
}

// 32bpp BMP (BGRA straight, снизу вверх), только для сравнения при отладке.
static bool _DumpToBmp(const std::wstring& path, const uint8_t* bgra, uint32_t width, uint32_t height) {
	const uint32_t rowBytes = width * 4;
	const uint32_t pixelBytes = rowBytes * height;

	BITMAPFILEHEADER bf{};
	bf.bfType = 0x4D42; // 'BM'
	bf.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + pixelBytes;
	bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

	BITMAPINFOHEADER bi{};
	bi.biSize = sizeof(BITMAPINFOHEADER);
	bi.biWidth = static_cast<LONG>(width);
	bi.biHeight = static_cast<LONG>(height);
	bi.biPlanes = 1;
	bi.biBitCount = 32;
	bi.biCompression = BI_RGB;

	wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!file) {
		return false;
	}

	const auto write = [&](const void* data, size_t size) {
		const char* ptr = static_cast<const char*>(data);
		while (size > 0) {
			DWORD written = 0;
			if (!WriteFile(file.get(), ptr, static_cast<DWORD>(size), &written, nullptr) || written == 0) {
				return false;
			}
			ptr += written;
			size -= written;
		}
		return true;
	};

	if (!write(&bf, sizeof(bf)) || !write(&bi, sizeof(bi))) {
		return false;
	}
	for (uint32_t row = height; row > 0; --row) {
		if (!write(bgra + static_cast<size_t>(row - 1) * rowBytes, rowBytes)) {
			return false;
		}
	}
	return true;
}

bool PhotoFrameSource::_DecodeCanvas(const std::wstring& path, Canvas& out) noexcept {
	std::string pathUtf8 = StrHelper::UTF16ToUTF8(path);
	try {
		auto file = wstorage::StorageFile::GetFileFromPathAsync(path.c_str()).get();
		auto stream = file.OpenAsync(wstorage::FileAccessMode::Read).get();
		auto decoder = wgi::BitmapDecoder::CreateAsync(stream).get();

		// Ориентация по EXIF и единый BGRA8 (он же DXGI B8G8R8A8) — тот формат,
		// который цепь эффектов получает от всех источников захвата; профиль — sRGB.
		// AlphaMode::Ignore: фотография непрозрачна, а цепь и представление считают
		// B8G8R8A8 straight (не premultiplied). От premultiplied или webp с альфой
		// полупрозрачные области подмешают предыдущий кадр и исказят цвет. Ниже
		// alpha принудительно выставляется в 255.
		wgi::SoftwareBitmap bitmap = decoder.GetSoftwareBitmapAsync(
			wgi::BitmapPixelFormat::Bgra8, wgi::BitmapAlphaMode::Ignore,
			wgi::BitmapTransform{}, wgi::ExifOrientationMode::RespectExifOrientation,
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

		// Временная диагностика: при MAGPIE_PHOTO_DEBUG_DUMP=<папка> сохранять результат
		// декода в BMP, чтобы сверять пиксели соседних страниц без экрана пользователя.
		static const std::wstring dumpDir = [] {
			wchar_t buf[4096]{};
			const DWORD len = GetEnvironmentVariableW(L"MAGPIE_PHOTO_DEBUG_DUMP", buf, (DWORD)std::size(buf));
			return len > 0 && len < std::size(buf) ? std::wstring(buf) : std::wstring{};
		}();
		if (!dumpDir.empty()) {
			static std::atomic<unsigned> dumpIndex{ 0 };
			const std::wstring dumpPath = dumpDir + L"\\photo-"
				+ std::to_wstring(dumpIndex.fetch_add(1)) + L".bmp";
			if (!_DumpToBmp(dumpPath, out.pixels.data(), width, height)) {
				Logger::Get().Warn(fmt::format("Запись отладочного BMP не удалась: {}", StrHelper::UTF16ToUTF8(dumpPath)));
			}
		}

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
	// Новая текстура — рендерер обязан перепривязать цепь; на странице того же размера сюда не заходим
	++_resourceGeneration;
	_rebindRequired = true;
	return true;
}

}
