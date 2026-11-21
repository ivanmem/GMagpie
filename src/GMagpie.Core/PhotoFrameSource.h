#pragma once
#include "FrameSourceBase.h"
#include <mutex>
#include <thread>
#include <vector>
#include <winrt/Windows.Graphics.Imaging.h>

namespace GMagpie {

// Источник кадров показа фотографий: ничего не захватывает, а декодирует файл
// изображения в выходную текстуру в его собственном размере. Увеличение и
// повышение резкости делает цепь эффектов, вписывание в экран — представление.
// Поэтому каждая стадия цепи обязана сохранять кратность, объявленную эффектом:
// заранее масштабировать кадр нельзя — у шейдеров коэффициент зашит в UV.
class PhotoFrameSource final : public FrameSourceBase {
public:
	// Любой поток; сам переход произойдёт в следующем _Update на потоке захвата.
	void RequestLoad(std::wstring path);

	// Есть ли запрос на загрузку; любой поток.
	bool HasPendingLoad() const noexcept;

	// Для рендерера в фото-сессии (реализация виртуальных методов FrameSourceBase)
	void RequestPhotoFile(std::wstring path) noexcept override { RequestLoad(std::move(path)); }
	bool HasPendingPhotoRequest() const noexcept override { return HasPendingLoad(); }
	uint64_t PhotoPageId() const noexcept override { return PhotoPageIdOf(_currentPath); }
	uint64_t PipelineTextureSerial() const noexcept override { return _textureSerial; }
	void RequestPhotoPrefetch(std::wstring path) noexcept override;
	void SetPrefetchListener(std::function<void(uint32_t, uint32_t)> listener) noexcept override {
		std::scoped_lock lock(_prefetchMutex);
		_prefetchListener = std::move(listener);
	}

	bool CopyAnimationFrame(uint64_t page, uint64_t& serial, std::vector<uint8_t>& pixels,
		uint32_t width, uint32_t height) noexcept override;

	bool RequiresRebind() noexcept override;

	~PhotoFrameSource() override;

	// Декодированный растр: BGRA straight (не premultiplied), alpha везде 255,
	// шаг строки = width*4, размер — собственный размер фотографии
	struct Canvas {
		std::wstring path;              // пусто = недействителен
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
	};

	// Чистый CPU: декодировать файл. Любой поток. fitTo задан - крупное фото сразу уменьшается декодером
	// так, чтобы вписаться в него (быстрый показ оригинала); пусто - нативный размер (вход цепи эффектов).
	// Поля по краям здесь не рисуются — ими занимается представление (целевой прямоугольник).
	static bool DecodeCanvas(const std::wstring& path, Canvas& out, SIZE fitTo = {}) noexcept;

protected:
	const char* Name() const noexcept override { return "PhotoFrameSource"; }

	FrameSourceWaitType WaitType() const noexcept override { return FrameSourceWaitType::WaitForFrame; }

	// Выставляется в RequestLoad, сбрасывается после потребления в _Update
	HANDLE FrameArrivedEvent() const noexcept override { return _loadArrived.get(); }

	bool _Initialize() noexcept override;

	FrameSourceState _Update() noexcept override;

private:
	static bool _DecodeCanvas(const std::wstring& path, Canvas& out) noexcept { return DecodeCanvas(path, out); }

	// Только поток бэкенда: _output — несбрасываемая текстура w×h (тот же размер —
	// переиспользуется, без перепривязки)
	bool _EnsureOutputTexture(uint32_t width, uint32_t height);

	void _PrefetchThreadProc();

	// Анимированный GIF: кадры собираются по одному на холсте логического экрана GIF
	struct GifPlayer {
		winrt::Windows::Graphics::Imaging::BitmapDecoder decoder{ nullptr };
		uint32_t frameCount = 0, index = 0, width = 0, height = 0;
		std::vector<uint8_t> canvas;  // BGRA, непрозрачный
		// Как убрать предыдущий кадр перед следующим (Disposal из GIF) и его прямоугольник
		uint32_t disposal = 0;
		RECT rect{};
		std::vector<uint8_t> saved;   // холст под rect до кадра с Disposal=3
		// Анимированный WebP: декодер отдаёт кадры уже собранными, длительности - из чанков ANMF файла
		std::vector<uint32_t> webpDelays;
	};
	// Открыть анимированный GIF (кадров больше одного); иначе false
	static bool _OpenGif(const winrt::Windows::Graphics::Imaging::BitmapDecoder& decoder, GifPlayer& player);
	// Открыть анимированный WebP; иначе false
	static bool _OpenWebp(const winrt::Windows::Graphics::Imaging::BitmapDecoder& decoder, const std::wstring& path, GifPlayer& player);
	// Наложить кадр player.index на холст и перейти к следующему; возвращает задержку кадра в мс
	static uint32_t _ComposeGifFrame(GifPlayer& player);

	// Только поток бэкенда: начать проигрывание текущей страницы, если это анимированный GIF
	void _StartAnimation(const std::wstring& path) noexcept;
	void _StopAnimation() noexcept;
	void _ScheduleAnimation(uint32_t delayMs) noexcept;

	GifPlayer _gif;                   // поток бэкенда; frameCount == 0 - анимации нет
	PTP_TIMER _animationTimer = nullptr;
	std::atomic<bool> _animationDue = false;
	// Копия последнего кадра анимации для Compare на фронтенде
	std::mutex _animationFrameMutex;
	std::vector<uint8_t> _animationFrame;
	uint64_t _animationFramePage = 0, _animationFrameSerial = 0;

	mutable std::mutex _requestMutex;
	std::wstring _pendingPath;  // под _requestMutex
	wil::unique_event_nothrow _loadArrived;

	// Предзагрузка (фоновый поток декодирования)
	std::thread _prefetchThread;
	wil::unique_event_nothrow _prefetchArrived;
	std::mutex _prefetchMutex;      // защищает следующие три поля
	std::wstring _prefetchRequest;  // задача на декод (пусто = нет)
	Canvas _prefetched;             // уже декодированное
	bool _prefetchStop = false;
	std::function<void(uint32_t, uint32_t)> _prefetchListener;

	// Доступно только потоку захвата
	bool _rebindRequired = false;
	// Первая фотография декодируется в _Initialize: кадр уже в текстуре, но рендерер
	// узнаёт о смене источника только по NewFrame из _Update. Этот одноразовый
	// флаг догоняет первую страницу до обычного кадра.
	bool _firstFramePending = false;
	std::wstring _currentPath;
	uint32_t _canvasWidth = 0, _canvasHeight = 0;
	uint64_t _textureSerial = 0;  // поток бэкенда: +1 на каждое пересоздание _output

	// «Кадр готов и стал новым источником» — один и тот же хвост для первой страницы
	// и для всех последующих
	void _MarkNewPresentation() noexcept;
};

}
