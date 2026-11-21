#pragma once
#include "FrameSourceBase.h"
#include <mutex>
#include <thread>
#include <vector>

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
	void RequestPhotoPrefetch(std::wstring path) noexcept override;

	bool RequiresRebind() noexcept override;

	~PhotoFrameSource() override;

protected:
	const char* Name() const noexcept override { return "PhotoFrameSource"; }

	FrameSourceWaitType WaitType() const noexcept override { return FrameSourceWaitType::WaitForFrame; }

	// Выставляется в RequestLoad, сбрасывается после потребления в _Update
	HANDLE FrameArrivedEvent() const noexcept override { return _loadArrived.get(); }

	bool _Initialize() noexcept override;

	FrameSourceState _Update() noexcept override;

private:
	// Декодированный растр: BGRA straight (не premultiplied), alpha везде 255,
	// шаг строки = width*4, размер — собственный размер фотографии
	struct Canvas {
		std::wstring path;              // пусто = недействителен
		std::vector<uint8_t> pixels;
		uint32_t width = 0, height = 0;
	};

	// Чистый CPU: декодировать файл в нативный размер. Любой поток. Поля по краям
	// здесь не рисуются — ими занимается представление (целевой прямоугольник).
	bool _DecodeCanvas(const std::wstring& path, Canvas& out) noexcept;

	// Только поток бэкенда: _output — несбрасываемая текстура w×h (тот же размер —
	// переиспользуется, без перепривязки)
	bool _EnsureOutputTexture(uint32_t width, uint32_t height);

	void _PrefetchThreadProc();

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

	// Доступно только потоку захвата
	bool _rebindRequired = false;
	// Первая фотография декодируется в _Initialize: кадр уже в текстуре, но рендерер
	// узнаёт о смене источника только по NewFrame из _Update. Этот одноразовый
	// флаг догоняет первую страницу до обычного кадра.
	bool _firstFramePending = false;
	std::wstring _currentPath;
	uint32_t _canvasWidth = 0, _canvasHeight = 0;

	// «Кадр готов и стал новым источником» — один и тот же хвост для первой страницы
	// и для всех последующих
	void _MarkNewPresentation() noexcept;
};

}
