# Design: add-photo-gallery

## Подтверждённые факты из кодовой базы (важно при правках)
- Выбор источника кадров — единственная точка: `Renderer::_InitFrameSource()`
  (switch по `Options().captureMethod`, Renderer.cpp ~2089).
- Цепь эффектов биндит текстуру источника напрямую (`GetPipelineTexture` в NewFrame-ветке
  рендер-цикла ~3768–3890) → при подмене `_output` обязан быть `_ResizeEffects()`.
  Он идемпотентен и уже используется (OnResize).
- `case FrameSourceState::NewFrame` в `_BackendRender`-ветке — единственное место, куда
  вставляется ребинд (после `_forceNextRender = false;`).
- `FrameSourceBase::Initialize` сам создаёт `_outputSrv` из `_output`; после ручного
  свапа текстуры SRV надо обновить через `_descriptorStore->GetShaderResourceView`.
- `WaitType::WaitForFrame` + `FrameArrivedEvent()` → `StepTimer::WaitForNextFrame`
  ждёт event (idle без CPU). `duplicateFrameDetectionMode` для фото = Never (обход дедуп-логики).
- Fullscreen renderer rect = весь монитор; пропорции соблюдает цепь через
  `ScalingType::Fit` (макс. вписывание в экран) + `DestAlignment::Center` = letterbox.
- Окно-источник обязателен для `ScalingWindow::_StartImpl`/`SrcTracker::Set`
  (WS_VISIBLE, пересечение с монитором, не WS_EX_TRANSPARENT, DWM bounds, IL) →
  stage-окно: класс `MagpiePhotoStage`, `WS_POPUP|WS_VISIBLE` + `WS_EX_TOOLWINDOW|WS_EX_LAYERED`,
  (0,0,512,288), `SetLayeredWindowAttributes(alpha=0)`, DefWindowProcW, UI-поток.
  После старта `_InitialMoveSrcWindowInFullscreen` уведёт его за экран — ожидаемо.
- `_CheckForegroundTimer_Tick` только ЗАПУСКАЕТ auto-scale чужих окон, не роняет сессию.
- `ScalingWindow::RestartWithEffectParameters(effects, frameRefresh)` — полный рестарт
  цепи без записи в настройки; `ScalingOptions` (включая `photoSession`) сохраняется.
  Это механизм Num6.
- Имена эффектов: `Lanczos` (корневой hlsl), `NIS\NIS` (вложенный). Флаг «масштабирующий»:
  `EffectInfo::CanScale()` (`EffectsService.h`). Конверсия имён `EffectItem`(UTF-16) ↔
  `EffectOption.name`(UTF-8) — существующие операторы/`StrHelper`.
- `EffectItem → (EffectOption)` оператор существует; `AppSettings::Get().DefaultProfile()`,
  `ScalingModesService::Get().GetScalingMode[idx/Count]` — готовые точки.
- Приложение unpackaged WinUI3, классический GetMessage-цикл (`App::Run`) → LL-хуки и
  stage-окно на UI-потоке качаются. CLI-точка — `App::Initialize(arguments)`.
  Single-instance: обычный запуск по-прежнему единственный (SHOWME/QUIT). `--gallery` - отдельная
  группа (мьютекс `...-Gallery`, «лёгкий режим»: без хоткеев, обновлений, elevation-рестарта,
  старт в трей): живёт рядом с обычным GMagpie, повторный `--gallery` блокируется только в своей группе.
- Декод: `Windows.Graphics.Imaging.BitmapDecoder.GetSoftwareBitmapAsync(Bgra8, Premultiplied,
  {}, RespectExifOrientation, ColorManageToSRgb)` → EXIF-поворот и sRGB бесплатно →
  `B8G8R8A8_UNORM` (тот же формат, что у всех захватов). Ограничение 16384px.

## Решения
1. **Фото-сессия = обычный scaling-сеанс** с `ScalingOptions::photoSession` и stage-окном
   вместо реального источника. Минимум веток в оконно-центричном коде; весь оверлей,
   панель параметров и выбор профилей работают штатно.
2. **Фото-оверрайды в `_StartScaleImpl`** (после построения options): fullscreen, SDR
   (включая `legacyHdrCaptureIntent = nullopt`/пустой `hdrComponents`), FG-эффекты
   вырезаются из цепи, captureMethod=GDI (заглушка), touch/cursor-магии/RTSS/dup-detect
   выкл; первому эффекту с `ScalingType::Normal` ставится `Fit` (реализация — через
   масштабирование первого эффекта; если у пользователя там не апскейлер — это осознанно
   «выкрутит» его под экран, цикл Num6 даёт замену).
3. **Плейлист/таймер/ввод — `GalleryService` (app)**: рекурсивный обход (см. список
   расширений в spec), `DispatcherQueueTimer`, `WH_KEYBOARD_LL`/`WH_MOUSE_LL` (только пока
   сессия активна; целевые клавиши съедаются). Часы/колесо/пауза — все идут через
   `ScalingService::RequestPhoto` → `ScalingRuntime::RequestPhotoFile` (dispatcher).
4. **Num6** — список `PhotoEffectsSnapshot()`, замена `.name/.parameters` у эффекта №0 на
   следующий из безопасного HLSL-цикла (валидируется `GetEffect && CanScale`), оригинал
   хранится для возврата назад, `ScalingService::RestartPhotoEffects` (обёртка над
   `ScalingRuntime::RestartWithEffectParameters` c stage+scaling HWND+RunId).
   Рабочий список живёт в `ScalingService::_photoWorkingEffects` (снимок после оверрайдов).
5. **Таймер автолистания**: repeating; ручной ход → Stop+Start (сброс отсчёта); Space — пауза.
6. **Ошибочные файлы**: decode-fail → остаётся прошлый кадр + Warning-лог; сессия не роняется.

## Открытые вопросы (решить при тесте)
- Роняет ли foreground-потеря stage-окна сессию на практике → при необходимости
  сторожевой `SetForegroundWindow` по таймеру.
- Декодирование на capture-потоке блокирующим `.get()` (MTA; WGC делает WinRT-activate на
  capture-потоке — прецедент есть). При проблемах — пре-декod на worker с последующей загрузкой.
