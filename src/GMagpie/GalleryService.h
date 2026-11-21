#pragma once
#include "Event.h"
#include "ScalingOptions.h"
#include <wil/resource.h>

namespace GMagpie {

// Почему показ не удалось начать. Причины разные и действия у них разные:
// «выбрать другую папку», «дождаться конца масштабирования» и т.д.
enum class GalleryStartFailure {
	None,
	NoImages,
	SessionBusy,
	Other
};

// Что означает клавиша галереи. Решение зависит от того, открыт ли список выбора
// апскейлера: со списком стрелки выбирают способ, а не листают страницы. Чистое
// сопоставление — реализация в DecideGalleryKey (GalleryService.cpp), проверяется
// тестами на CPU без показа.
enum class GalleryKeyAction {
	PassThrough,    // не наша — пусть достанется обычным приложениям
	PagePrev,       // ← / ↑
	PageNext,       // → / ↓
	First,          // Home
	Last,           // End
	TogglePause,    // Space
	TogglePicker,   // Num6 — показать/скрыть список апскейлеров
	ClosePicker,    // Esc / F11 при открытом списке
	PickerUp,       // ↑ в списке — выбрать предыдущий способ
	PickerDown,     // ↓ в списке — выбрать следующий способ
	DeletePhoto,    // Delete — удалить файл текущей фотографии в корзину
	Stop,           // Esc при закрытом списке
};

// Фотогалерея поверх фото-сессии ScalingService: плейлист по папке (включая
// подпапки), листание стрелками/колесом мыши, автолистание раз в N секунд со
// сбросом отсчёта при ручном ходе, пауза (Space), выход (Esc) и список
// апскейлеров по Num6 (в нём выбор стрелками, закрытие — повторным Num6,
// Esc или F11). Клавиши и колесо перехватываются
// глобальными низкоуровневыми хуками только пока окно галереи в фокусе — вне
// фокуса ввод уходит обычным приложениям, а клик по фото возвращает фокус
// галерее. Все методы — с UI-потока. Папка и страница последней сессии
// сохраняются в конфигурации, чтобы с них можно было продолжить просмотр.
// Поддерживаемый ли растровый формат (по расширению, регистр не важен)
bool IsSupportedPhoto(std::wstring_view path);

class GalleryService {
public:
	static GalleryService& Get() noexcept {
		static GalleryService instance;
		return instance;
	}

	GalleryService(const GalleryService&) = delete;
	GalleryService(GalleryService&&) = delete;

	// intervalSeconds = 0 — автолистание выключено; scalingModeIdx = -1 — режим из профиля;
	// startIndex — первая страница (0), нужен для восстановления прошлой сессии
	// openFile — открыть одно фото «как в обычных галереях»: плейлист из его папки (без подпапок),
	// старт со страницы этого файла; startIndex при этом игнорируется
	bool Start(std::wstring folder, uint32_t intervalSeconds, int scalingModeIdx, size_t startIndex = 0,
		std::wstring openFile = {});
	// Открыть файл в этом экземпляре: идущий показ переключается на его папку без перезапуска сессии,
	// остановленный - стартует. Вызывается из окна приёма файлов от второго запуска
	void OpenFile(std::wstring file);
	// Окно приёма (message-only): второй запуск «GMagpie.exe <файл>» шлёт сюда путь через WM_COPYDATA
	void CreateIpcWindow();
	// Передать файл уже запущенному экземпляру галереи; false - никто не ответил
	static bool ForwardToRunning(std::wstring_view file);
	// Пункт «Открыть в галерее» в контекстном меню Проводника и запись в «Открыть с помощью» для
	// всех поддерживаемых форматов (HKCU, без прав администратора); enable=false — убрать
	static bool RegisterShell(bool enable);
	// Зарегистрирован ли пункт (по ключу команды для первого формата)
	static bool IsShellRegistered();
	// Продолжить прошлый просмотр: папка и страница из сохранённого состояния
	bool StartLast(uint32_t intervalSeconds);
	void Stop();
	bool IsRunning() const noexcept { return _running; }
	// Позиция показа для главной страницы (соседствует с кнопкой «Продолжить»):
	// страница с единицы и всего файлов, как в строке состояния показа; 0 — показа нет.
	size_t ProgressIndex() const noexcept { return _running ? _index + 1 : 0; }
	size_t ProgressTotal() const noexcept { return _running ? _playlist.size() : 0; }
	// (страница, всего) после каждой смены страницы; (0, 0) при остановке. UI-поток.
	Event<size_t, size_t> ProgressChanged;
	// Причина последнего неудачного Start/StartLast
	GalleryStartFailure LastStartFailure() const noexcept { return _lastStartFailure; }

private:
	GalleryService() = default;

	void _BuildPlaylist(const std::wstring& folder, bool recursive);
	void _ShowCurrent();
	void _PersistSession(bool force) noexcept;
	void _PrefetchNext();
	void _UpdateStatusOverlay();
	void _ResetAutoTimer();
	void _SetPaused(bool paused);
	bool _IsSessionForeground() const noexcept;
	bool _TryFocusFromPhotoClick(POINT pt) noexcept;
	void _CycleUpscaler(int direction, bool silent = false);
	// Delete: текущий файл в корзину, плейлист и страница — следом за ним
	void _DeleteCurrentPhoto();
	// На время модального SHFileOperationW блокирует реентер навигации/удаления из
	// LL-хуков и таймера (они живут на том же UI-потоке во время насоса сообщений)
	bool _deleting = false;
	// Список апскейлеров (Num6): показать и скрыть (скрытие — Num6 повторно,
	// Esc/F11 или остановка показа; выбор стрелкой список НЕ закрывает)
	void _ShowUpscalerPicker();
	void _HideUpscalerPicker() noexcept;
	void _Navigate(long long delta);
	void _Cleanup() noexcept;

	static LRESULT CALLBACK _KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK _MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);
	void _OnKeyAction(GalleryKeyAction action);
	void _OnMouseWheel(short wheelDelta);

	bool _running = false;
	// Обновление строки состояния уже стоит в очереди диспетчера (одно на серию нажатий)
	bool _statusUpdateQueued = false;
	bool _paused = false;
	size_t _index = 0;
	// Направление последнего перелистывания (+1/−1): по нему предзагружаем следующий кадр
	int _stepDirection = 1;
	uint32_t _intervalSeconds = 0;
	std::wstring _folder;
	std::vector<std::wstring> _playlist;
	// Последняя сохранённая позиция: быстрое листание не должно писать конфиг на каждом кадре
	uint32_t _lastPersistTick = 0;
	GalleryStartFailure _lastStartFailure = GalleryStartFailure::None;
	// Накопитель хода колеса: плавные колёса присылают доли щелчка, листаем по щелчку
	int _wheelAccum = 0;

	// Цикл апскейлеров: _originalFirst — копия рабочей цепи до первой замены,
	// _cyclePos = -1 — активен оригинал профиля, иначе индекс в _upscalerCycle.
	std::vector<EffectOption> _originalFirst;
	std::vector<std::string> _upscalerCycle;
	int _cyclePos = -2;
	// Список апскейлеров открыт (Num6). Он не прячется сам — только выбор,
	// повторный Num6, Esc или F11
	bool _pickerVisible = false;
	// Удерживаемый Num6 шлёт автоповтор: список должен переключаться один раз
	// на нажатие, поэтому флаг живёт до key-up
	bool _num6Held = false;

	wil::unique_hhook _keyboardHook;
	wil::unique_hhook _mouseHook;
	winrt::DispatcherQueueTimer _autoTimer{ nullptr };
	Event<bool>::EventRevoker _scalingChangedRevoker;
};

}
