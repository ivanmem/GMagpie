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

// Что означает клавиша галереи. Чистое сопоставление — реализация в DecideGalleryKey
// (GalleryService.cpp), проверяется тестами на CPU без показа. Пока открыто контекстное
// меню, клавиши принадлежат ему, а не этому сопоставлению.
enum class GalleryKeyAction {
	PassThrough,    // не наша — пусть достанется обычным приложениям
	PagePrev,       // ← / ↑
	PageNext,       // → / ↓
	First,          // Home
	Last,           // End
	TogglePause,    // Space
	OpenMenu,       // Num6 — контекстное меню (как правая кнопка мыши)
	DeletePhoto,    // Delete — удалить файл текущей фотографии в корзину
	FavoritePhoto,  // Insert — переместить фото в папку избранного
	Stop,           // Esc / F11
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
	bool _IsClickOnPhoto(POINT pt) const noexcept;
	// Выбрать апскейлер по позиции списка (-1 - оригинал профиля, иначе индекс в _upscalerCycle)
	void _ApplyUpscaler(int position, bool silent = false);
	// Подготовить снимок цепи профиля и убрать его эффект из цикла; false - применять нечего
	bool _EnsureUpscalerSnapshot();
	// Delete: текущий файл в корзину, плейлист и страница — следом за ним
	void _DeleteCurrentPhoto(bool toFavorites = false);
	// «Сохранить как»: диалог выбора файла, затем апскейл текущей страницы в PNG
	void _SaveCurrentAs(HWND owner);
	// На время модального SHFileOperationW блокирует реентер навигации/удаления из
	// LL-хуков и таймера (они живут на том же UI-потоке во время насоса сообщений)
	bool _deleting = false;
	// Контекстное меню (правая кнопка, Num6): апскейлеры, пауза, удаление, выход. Модальное:
	// вызывать из очереди диспетчера, не из LL-хука
	void _ShowContextMenu(POINT screenPoint, bool atCursor);
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
	// Контекстное меню открыто: хуки пропускают ввод ему (иначе стрелки листали бы страницы)
	bool _menuOpen = false;

	wil::unique_hhook _keyboardHook;
	wil::unique_hhook _mouseHook;
	winrt::DispatcherQueueTimer _autoTimer{ nullptr };
	// Строка состояния ждёт окна вывода: до него место и процент считать не от чего
	winrt::DispatcherQueueTimer _statusRetryTimer{ nullptr };
	Event<bool>::EventRevoker _scalingChangedRevoker;
};

}
