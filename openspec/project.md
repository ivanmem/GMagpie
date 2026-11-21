# Project: GMagpie Experimental (локальная копия)

Неофициальный форк Blinue/GMagpie: Windows-инструмент обработки содержимого окон.
Захватывает целевое окно, прогоняет через цепочку GPU-эффектов (апскейл, DLSS/FSR/XeSS,
шарпинг, HDR) и выводит fullscreen/оконно. GPL-3.0.

## Стек и слои
- `src/GMagpie` — WinUI 3 (unpackaged) фронтенд: профили (ScalingModes JSON), хоткеи, `ScalingService`.
- `src/GMagpie.Core` — движок: `FrameSource*` (захват) → `EffectDrawer` (цепь) → `Presenter`; HDR-конвейер, ImGui-оверлей.
- `src/Effects` — HLSL-эффекты; имена = путь относительно папки без расширения (`Lanczos`, `NIS\NIS`).
- `src/Shared` — общее (Win32Helper, Logger, Event…).

## Конвенции
- C++20, MSVC (VS2022), табы, `#include "pch.h"` первым, Chinese-комментарии в original-коде —
  новые файлы комментируем кратко на русском/английском, стиль как у соседних файлов.
- Любые новые файлы регистрировать в `*.vcxproj` + `*.vcxproj.filters`.
- Логи через `Logger::Get()` (текст можно на русском), ошибки юзеру — `ErrorService`/`ToastService`.
- Потоки: UI-поток (DispatcherQueue + классический GetMessage-цикл), scaling-поток (`ScalingRuntime`),
  backend/capture-поток (render-цикл `Renderer`). Каузальные переходы через dispatcher-пэттерны.

## Сборка
- VS2022 Community: `C:\Program Files\Microsoft\Visual\Studio...` → `MSBuild.exe` (x64-Release).
- Conan-зависимости (`F:\Python\Python310\Scripts\conan.exe`, `conan-locks/`); проприетарные бэкенды
  (DLSS/FSR/XeSS/RTX) по умолчанию выключены — `src/BuildOptions.props.user` не нужен для базовой сборки.
- Точные шаги: `docs/编译指南.md`.

## OpenSpec workflow
- Активные изменения: `openspec/changes/<id>/` (proposal → design → specs delta → tasks).
- Реализованные требования переносить в `openspec/specs/` архивацией изменения.
