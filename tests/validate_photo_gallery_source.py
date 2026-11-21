"""Check the photo-gallery source invariants that no CPU test can execute.

These are the fixes that need a window, a device or a second process to happen:
the always-appended normalizing pass, the empty effect chain, keys and the wheel
belonging to the slideshow only while it is in the foreground, not restarting the
app after a photo session. Reading the shipped source is what a review would do;
it runs offline and changes nothing.

--self-test re-introduces each old defect in memory and requires the check that
catches it to fail, so the script cannot become a rubber stamp.
"""
import re
import sys

_REPO = __import__("pathlib").Path(__file__).resolve().parents[1]
SRC = _REPO / "src"

failures = []


def text(relative):
    source = (SRC / relative).read_text(encoding="utf-8-sig")
    for old, new in _PATCHES.get(relative, ()):
        source = source.replace(old, new)
    return source


_PATCHES = {}


def body(source, signature):
    """The text of one function, from its signature to its closing brace."""
    start = source.index(signature)
    depth = 0
    for i in range(source.index("{", start), len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError(f"unbalanced braces in {signature}")


def guarded_by(code, call, guard, window=420):
    """Every call site must be preceded by the guard inside its own if-condition."""
    sites = [m.start() for m in re.finditer(re.escape(call), code)]
    return bool(sites) and all(guard in code[max(0, i - window):i] for i in sites)


def check(name, condition, detail):
    if not condition:
        failures.append(f"{name}: {detail}")


def run_checks():
    """All invariants. Kept free of side effects so --self-test can rerun them."""
    del failures[:]

    # --- Renderer: every photo session ends with a pass at renderer size -----
    renderer = text("GMagpie.Core/Renderer.cpp")
    should_append = body(renderer, "bool Renderer::_ShouldAppendBicubic(")
    check("always-normalize", "if (options.photoSession)" in should_append,
          "photo sessions must return true before anything can return false: the "
          "presentation textures exist only at the size the chain had at session start")
    if "if (options.photoSession)" in should_append:
        # Ничего, что может вернуть false, не должно стоять раньше фото-проверки:
        # иначе путь с upscaleMode=0 выходит раньше, чем мы что-то решили про фото
        after_options = should_append[
            should_append.index("ScalingOptions& options = ScalingWindow::Get().Options();"):]
        photo_at = after_options.index("if (options.photoSession) {")
        check("always-normalize-first",
              "return" not in after_options[:photo_at],
              "the photo-session check must come first: with Nvidia VFX upscaleMode=0 the "
              "Nvidia path returns false, the chain ends at an arbitrary size, and every "
              "CopyResource into the presentation texture fails")

    backend = body(renderer, "void Renderer::_BackendThreadProc() noexcept")
    check("stage-count-is-predicted",
          "CalcPhotoEffectStages(" in body(renderer, "ID3D11Texture2D* Renderer::_ResizePhotoEffects("),
          "the number of stages must come from the tested prediction function, not a copy of it")
    check("empty-chain-guard",
          re.search(r"_effectDrawers\.empty\(\)\s*\n\s*\?\s*_frameSource->GetPipelineTexture\(\)",
                    backend) is not None,
          "an empty effect chain (a photo not smaller than the screen) must publish the "
          "pipeline texture instead of back() on an empty vector")

    switch_block = backend[backend.index("RequiresRebind()"):][:1200]
    check("photo-switch-rebind",
          "_ResizeEffects()" in switch_block and "_UpdateDestRect()" in switch_block
          and switch_block.index("_ResizeEffects()") < switch_block.index("_UpdateDestRect()"),
          "on a photo switch the effect chain and the destination rectangle must both be "
          "rebuilt, in that order: the rectangle follows the page's own aspect ratio, so a "
          "stale one stretches or crops the picture")
    check("photo-chain-from-rebind",
          "_ResizePhotoEffects()" in body(renderer, "ID3D11Texture2D* Renderer::_ResizeEffects("),
          "the photo chain must be reached through the normal rebinding path")
    copy_body = body(renderer, "void Renderer::_CopySceneToTarget(")
    check("photo-scene-scaled-to-dest",
          "CalcPhotoSceneViewport" in copy_body and "_DrawPhotoSceneScaled" in copy_body
          and "Draw(3, 0)" in body(renderer, "void Renderer::_DrawPhotoSceneScaled("),
          "the photo chain always outputs a frame-sized (stretched) image and the "
          "presentation copies 1:1, so the photo session must draw the scene scaled into "
          "the fitted rectangle: otherwise the photo fills the whole screen with its "
          "aspect lost, whatever _destRect says")
    scaled_body = body(renderer, "void Renderer::_DrawPhotoSceneScaled(")
    check("photo-scene-copy-before-draw",
          "CopyResource(_photoSceneCopy.get(), scene)" in scaled_body
          and scaled_body.index("CopyResource(_photoSceneCopy.get(), scene)")
              < scaled_body.index("Draw(3, 0)")
          and "CreateShaderResourceView(scene" not in scaled_body,
          "a keyed-mutex shared texture allows nothing but copying out of itself: "
          "the SRV on it failed every frame and photos that need the scaled draw "
          "(everything not exactly screen-shaped) stayed black after the background "
          "clear; the scene must first be copied into a private texture and only "
          "then drawn from it")
    passthrough_body = body(renderer, "bool Renderer::SetPassThroughActive(")
    eye_guard = text("GMagpie.Core/OverlayDrawer.cpp")
    eye_at = eye_guard.index("Overlay_Toolbar_PassThrough")
    check("compare-off-in-photo-session",
          "Options().photoSession" in passthrough_body
          and passthrough_body.index("Options().photoSession")
              < passthrough_body.index("_isPassThroughActive = value;")
          and "Options().photoSession" in eye_guard[max(0, eye_at - 1500):eye_at],
          "Compare (pass-through) is built for continuous capture sources - the photo "
          "source sleeps between pages, so enabling it hangs the next photo on the "
          "reference-frame slot (reproduced: page requested, backend never published). "
          "The photo session must refuse pass-through before any state change, and the "
          "toolbar must not even offer the eye button there")

    # --- PhotoFrameSource: the first page is presented like every other page --
    photos = text("GMagpie.Core/PhotoFrameSource.cpp")
    initialize = body(photos, "bool PhotoFrameSource::_Initialize() noexcept")
    update = body(photos, "FrameSourceState PhotoFrameSource::_Update() noexcept")
    check("first-page-presented",
          "_firstFramePending = true" in initialize
          and "if (_firstFramePending)" in update
          and "_MarkNewPresentation();" in update
          and "++_captureSequence" not in initialize,
          "the first photo is decoded before the renderer exists, so its frame must be "
          "presented through the same tail as every page (_MarkNewPresentation): without "
          "it the chain and the destination rectangle are never computed for page one and "
          "the first page stays black until the user pages forward")

    # --- ScalingService: a photo session must not relaunch the app -----------
    restart = [m for m in re.findall(r"shouldRestartForSmoothMotion\s*=\s*([^;]+);",
                                     text("GMagpie/ScalingService.cpp"))
               if "IsSmoothMotionCompatibilityMode" in m]
    check("no-restart-for-photos",
          len(restart) == 1 and "!_photoSessionActive" in restart[0],
          "the app must not relaunch itself after a slideshow: it raced manual launches "
          "('already one instance running' / 'scaling session in progress')")

    scaling = text("GMagpie/ScalingService.cpp")
    start_photo = body(scaling, "bool ScalingService::StartPhotoSession(")
    check("photo-session-build-mode-fallback",
          "EffectDisabledAtBuildTime(effect.name)" in start_photo
          and "profile.scalingMode = static_cast<int>(*fallback);" in start_photo,
          "a default profile pointing at a mode whose native effect is compiled out of the "
          "build killed the slideshow at renderer init (\"opens and instantly closes\"); the "
          "session must fall back to the first build-supported mode instead of starting a doomed one")

    # --- GalleryService: input belongs to the slideshow only when focused ----
    gallery = text("GMagpie/GalleryService.cpp")
    keyboard = body(gallery, "LRESULT CALLBACK GalleryService::_KeyboardHookProc(")
    check("keys-come-from-tested-map",
          "DecideGalleryKey(vkCode, self._pickerVisible)" in keyboard
          and "case VK_" not in keyboard,
          "the meaning of a key must come from the tested map (it depends on the picker "
          "state), not from an inline switch in the hook")
    check("keys-need-focus", guarded_by(keyboard, "self._OnKeyAction(action)", "_IsSessionForeground()"),
          "hotkeys including Esc must reach ordinary applications while the slideshow is "
          "not in the foreground")
    check("num6-single-toggle",
          "_num6Held = false;" in keyboard and "WM_KEYUP" in keyboard
          and "if (self._num6Held)" in keyboard,
          "a held Num6 sends auto-repeat: without consuming it the picker flickers "
          "open/closed while the key is down")

    key_action = body(gallery, "void GalleryService::_OnKeyAction(GalleryKeyAction action)")
    picker_cases = key_action[key_action.index("PickerUp"):key_action.index("case GalleryKeyAction::Stop")]
    check("picker-stays-open-on-choice",
          picker_cases.count("_CycleUpscaler(") == 2
          and "_HideUpscalerPicker" not in picker_cases,
          "choosing with the arrows must apply the effect but KEEP the list open: the "
          "user compares modes in place; the list closes only on Num6/Esc/F11")
    check("picker-no-self-hide",
          gallery.count("gallery_picker::Hide()") == 2
          and "gallery_picker::Hide()" in body(gallery, "void GalleryService::_HideUpscalerPicker() noexcept")
          and "gallery_picker::Hide()" in body(gallery, "void GalleryService::_Cleanup() noexcept"),
          "the list must never disappear by itself (no timer): only the explicit hide on "
          "Num6/Esc/F11 and on cleanup")
    candidates = body(gallery, "static const std::vector<std::string> UPSCALER_CANDIDATES")
    check("vsr-in-upscaler-cycle",
          "#ifdef MP_ENABLE_RTX_VIDEO_DENOISE" in candidates
          and "NVIDIA_VFX_VSR_ID" in candidates
          and "info->CanScale()" in gallery,
          "the user asked for RTX VSR in the picker: the list must include it, but only in "
          "builds where the native RTX Video backend is compiled in (offering it in a build "
          "without the SDK killed the session on selection), gated again by the runtime "
          "availability check that skips machines without the VFX runtime")

    mouse = body(gallery, "LRESULT CALLBACK GalleryService::_MouseHookProc(")
    check("wheel-data-source",
          mouse.count("HIWORD(((MSLLHOOKSTRUCT*)lParam)->mouseData)") >= 2,
          "a low-level mouse hook carries the wheel delta in MSLLHOOKSTRUCT.mouseData; "
          "reading wParam made the delta always 0 and the wheel did nothing")
    check("wheel-need-focus", guarded_by(mouse, "self._OnMouseWheel(", "_IsSessionForeground()"),
          "the wheel must page only while the slideshow is in the foreground")

    check("cycle-uses-tested-index",
          "NextUpscalerCycle(_cyclePos," in body(gallery, "void GalleryService::_CycleUpscaler("),
          "the Num6 index must come from the tested cycle function")
    cycle_body = body(gallery, "void GalleryService::_CycleUpscaler(int direction, bool silent)")
    check("restart-keeps-page",
          "NextUpscalerCycle(_cyclePos," in cycle_body
          and "RestartPhotoEffects(std::move(effects))" in cycle_body
          and "RequestPhoto(_playlist[_index])" in cycle_body
          and cycle_body.index("RestartPhotoEffects(std::move(effects))")
              < cycle_body.index("RequestPhoto(_playlist[_index])"),
          "picking an upscaler rebuilds the session, which loads the album's first photo; "
          "the gallery must re-request the current page immediately after the restart")
    check("picker-highlight-follows-choice",
          "_pickerVisible" in cycle_body and "_ShowUpscalerPicker();" in cycle_body,
          "the arrows choose without closing the list, so the highlight must move there: "
          "the picker window only repaints on Update(), and without a refresh the "
          "highlight stayed on the old row until the list was hidden and shown again")
    check("cycle-dedup-profile-effect",
          "std::erase(_upscalerCycle, _originalFirst[0].name)" in
              body(gallery, "void GalleryService::_CycleUpscaler(int direction, bool silent)")
          and "std::erase(_upscalerCycle, _originalFirst[0].name)" in
              body(gallery, "void GalleryService::_ShowUpscalerPicker("),
          "when the profile mode's first effect is also a cycle entry (the shipped "
          "default profile is RTX VSR Ultra), the list showed the same upscaler twice "
          "and the cycle copy silently dropped the profile's parameters; the original "
          "row must stay the only instance and keep its parameters")
    delete_body = body(gallery, "void GalleryService::_DeleteCurrentPhoto(")
    check("delete-goes-to-recycle-bin",
          "FOF_ALLOWUNDO" in delete_body and "SHFileOperationW" in delete_body
          and "_DeleteCurrentPhoto();" in body(gallery, "void GalleryService::_OnKeyAction("),
          "Delete must never destroy the file (a wrong press has to stay recoverable "
          "from the Recycle Bin) and must be wired to the key map")
    check("delete-keeps-page-position",
          "PhotoIndexAfterDelete(_index" in delete_body and "_playlist.erase(" in delete_body,
          "after removing the file the show must continue with the photo that took its "
          "place (from the last page step back), via the tested pure function - not "
          "jump to page one")
    check("home-shows-gallery-progress",
          "ViewModel.GalleryProgressText" in text("GMagpie/HomePage.xaml")
          and "GalleryService::Get().ProgressIndex()" in text("GMagpie/HomeViewModel.cpp")
          and "ProgressChanged.Invoke(_index + 1" in body(gallery, "void GalleryService::_ShowCurrent(")
          and "ProgressChanged.Invoke(0, 0);" in body(gallery, "void GalleryService::_Cleanup("),
          "the home page shows the slideshow position next to the resume button (same "
          "'N / M' as the status line): the gallery must publish every page and the "
          "clear-on-stop, the view model must read the live position")
    app = text("GMagpie/App.cpp")
    check("gallery-lite-coexists",
          'L"-Gallery"' in body(app, "bool App::_CheckSingleInstance(")
          and "_galleryLiteMode = HasGalleryArgument(" in app
          and "if (!_galleryLiteMode) {\n\t\tShortcutService::Get().Initialize();" in app
          and "if (!_galleryLiteMode) {\n\t\tUpdateService::Get().Initialize();" in app,
          "the slideshow instance must live next to the user's GMagpie: its own "
          "single-instance group (no SHOWME/QUIT fights), no global hotkey hook, no "
          "update check - a plain launch keeps the original full behaviour")
    scaling = text("GMagpie/ScalingService.cpp")
    check("gallery-lite-no-foreground-watcher",
          "IsGalleryLiteMode()" in scaling
          and scaling.index("IsGalleryLiteMode()")
              < scaling.index("_checkForegroundTimer = dispatcher.CreateTimer()")
          and "if (_checkForegroundTimer)" in scaling,
          "two GMagpie instances must not both chase the foreground window - the lite "
          "gallery instance skips the watcher timer (and its immediate first tick), "
          "and Uninitialize must tolerate the timer never having been created")
    check("start-last-resets-failure-reason",
          "_lastStartFailure = GalleryStartFailure::Other;"
              in body(gallery, "bool GalleryService::StartLast(")[:400],
          "a resume that fails before Start (the folder vanished) must not leave the "
          "previous run's failure reason in place - the home toast would tell about "
          "the wrong problem")
    renderer_cpp = text("GMagpie.Core/Renderer.cpp")
    check("frontend-dest-rect-snapshot",
          "_frontendDestRect = _destRect;\n\t\t_frontendMotionAvailability" in renderer_cpp
          and "CalcPhotoSceneViewport(_frontendDestRect," in renderer_cpp
          and "CopySubresourceRegion(target, 0,\n\t\t\tdrawOffset.x + _frontendDestRect.left" in renderer_cpp,
          "the backend rewrites _destRect for the NEW photo before the frontend adopts its "
          "frame; a redraw of the old base frame with the live rectangle painted the old "
          "photo in the new bars (aspect flash) - the frontend must draw by the snapshot "
          "taken together with the frame")
    check("ngx-fault-keeps-frame-source",
          "while (GetMessage(&msg, NULL, 0, 0) > 0) {" in
              renderer_cpp[renderer_cpp.index("NgxRuntimeGuard::IsFaulted() &&"):][:1800],
          "a backend that returned on an NGX fault ran cleanupBackend (_frameSource.reset()) "
          "in parallel with the frontend photo lambdas - wait for WM_QUIT/join instead")
    check("restart-latest-request-wins",
          "serial != _restartSerial.load" in text("GMagpie.Core/ScalingRuntime.cpp"),
          "the first queued restart recreates the window and bumps RunId, so every later "
          "queued one died on the guards - only the newest request may run")
    check("stale-idle-tears-down-photo-session",
          "if (value == ScalingState::Idle && _photoSessionActive) {\n\t\t\t\t_photoStage.reset();" in scaling,
          "an Idle that no longer matches State() (photo session replaced by a forced "
          "game scaling) must still drop the photo flag/stage and notify the gallery")
    check("gallery-start-timer-failure-cleans-up",
          "catch (const winrt::hresult_error& e)" in body(gallery, "bool GalleryService::Start(")
          and "try {\n\t\tProgressChanged.Invoke(0, 0);" in gallery,
          "a throwing timer or progress subscriber must not leave hooks/session hanging "
          "or terminate the noexcept cleanup")
    show_body = body(gallery, "void GalleryService::_ShowCurrent(")
    check("page-request-first-in-hook",
          show_body.index("RequestPhoto(") < show_body.index("_UpdateStatusOverlay(")
          and "_UpdateStatusOverlay();\n\t_PersistSession" not in show_body
          and "_statusUpdateQueued" in show_body,
          "_ShowCurrent runs inside the LL keyboard hook: the page request must go first and the "
          "status line (WIC file read) must be deferred, or a loaded system stalls the hook and "
          "the next key press")
    check("stale-photo-skipped-with-limit",
          "HasPendingPhotoRequest()" in renderer_cpp and "PHOTO_SKIP_WINDOW" in renderer_cpp
          and "photoUpdateEnd - _lastPhotoRender < PHOTO_SKIP_WINDOW" in renderer_cpp,
          "a page the user already moved past must not cost GPU time, but never for longer than "
          "the window - a held arrow would otherwise show nothing")
    delete_body = body(gallery, "void GalleryService::_DeleteCurrentPhoto(")
    check("status-percent-respects-exif",
          "System.Photo.Orientation" in body(gallery, "bool TryGetImageSize(")
          and "PhotoAppliesExifRotation(orientation)" in body(gallery, "bool TryGetImageSize("),
          "the frame is presented EXIF-rotated (the source decodes with "
          "RespectExifOrientation) while WIC GetSize reports the unrotated encoded "
          "size - the status percent must swap sides for orientations 5..8 or it "
          "inverts on phone portraits")
    check("delete-survives-message-pump",
          "_deleting" in delete_body and "scope_exit" in delete_body
          and "std::find(_playlist.begin(), _playlist.end(), path)" in delete_body
          and "if (!_running)" in delete_body,
          "SHFileOperationW pumps messages on the UI thread while the gallery's "
          "LL hooks and the auto timer are alive: without the re-entry guard and "
          "erasing by identity (not by a possibly moved index), a held Delete key "
          "erased the wrong entry or ran erase past end() (UB)")
    overlay_bodies = [body(gallery, "void Update(const std::wstring& text"),
                      body(gallery, "void Update(std::wstring header")]
    check("overlay-owned-reanchors-below-photo",
          all("GW_OWNER" not in update_body and "0, 0, 0, 0, owner" not in update_body
              and "0, 0, 0, 0, nullptr" in update_body for update_body in overlay_bodies),
          "owned overlays follow their owner: the scaling output re-asserts its z-order "
          "on every page shown (WM_WINDOWPOSCHANGING forces HWND_TOPMOST) and the OS "
          "re-anchors owned windows right beneath it - the re-assert timer kept calling "
          "SetWindowPos(HWND_TOPMOST) while the window stayed one slot below the photo "
          "(measured: 'z-guard' fired every tick, probe and screenshot still saw the "
          "photo on top). The overlays must be ownerless topmost windows")
    check("overlay-insert-after-owner-sinks",
          all("SetWindowPos(g_hwnd.get(), HWND_TOPMOST, x" in update_body
              and "owner ? owner :" not in update_body for update_body in overlay_bodies),
          "SetWindowPos's hwndInsertAfter means insert AFTER (below) the given "
          "window: passing the owner handle placed the overlay exactly beneath the "
          "photo (the user saw 'the menu is right under the picture'); the overlays "
          "must sit at the top of the topmost band")
    gallery_text = text("GMagpie/GalleryService.cpp")
    check("overlay-reasserts-topmost",
          gallery_text.count("WM_TIMER:") == 2
          and gallery_text.count(
              "SetTimer(g_hwnd.get(), Z_REASSERT_TIMER, 200, nullptr)") == 2
          and "IsBelowOutputWindow" in gallery_text,
          "the scaling output re-asserts its z-order whenever a page is shown, so a "
          "one-shot placement loses (the probe screenshot showed the photo with "
          "neither the picker nor the counter): both overlay windows need the "
          "WM_TIMER guard that walks GW_HWNDPREV and returns the window to the top "
           "of the band")
    check("overlay-layered-sinks-below-photo",
          all("| WS_EX_LAYERED" not in update_body
              and "WS_EX_NOACTIVATE | WS_EX_TRANSPARENT," in update_body
              for update_body in overlay_bodies),
          "a live z-order probe (tests/probe_photo_gallery_zorder.ps1) showed that a "
          "WS_EX_LAYERED owned window still sinks below the owner when the owner "
          "presents through DComp, while the plain owned toast window stays on top: "
          "the overlays must be ordinary owned windows (opaque, like the toast)")
    check("resume-page-clamped",
          "PhotoResumeIndex(startIndex" in body(gallery, "bool GalleryService::Start(std::wstring folder"),
          "a saved page beyond the current album must clamp to an existing page")
    cleanup = body(gallery, "void GalleryService::_Cleanup() noexcept")
    check("state-saved-before-reset",
          "_PersistSession(true)" in cleanup and "_playlist.clear()" in cleanup
          and cleanup.index("_PersistSession(true)") < cleanup.index("_playlist.clear()"),
          "the resume state must be written before the session is torn down")
    check("prefetch-follows-direction",
          "PhotoPrefetchIndex(_index, _playlist.size(), _stepDirection)"
          in body(gallery, "void GalleryService::_PrefetchNext()"),
          "prefetching only forward left backward scrolling on a cold path (decode is most "
          "of the flip latency)")

    # --- Review follow-ups: the fixes that came out of the review ------------
    callback = body(gallery, "winrt::auto_revoke, [this](bool isScaling)")
    deferred = callback.find("TryEnqueue")
    check("cleanup-not-from-its-own-event",
          deferred != -1 and "_Cleanup()" not in callback[:deferred]
          and "_Cleanup();" in callback[deferred:],
          "unsubscribing from inside the event's own callback removes the element while "
          "IsScalingChanged.Invoke walks the delegate list (Event.h forbids it) and could "
          "drop the next subscriber; the cleanup must go to the next dispatcher turn")
    check("photo-src-rect-single-writer",
          "_photoSrcRect = " not in body(renderer, "const RECT& Renderer::SrcRect() const noexcept")
          and "_photoSrcRect = " in body(renderer, "void Renderer::_UpdatePhotoSrcRect() noexcept")
          and "_UpdatePhotoSrcRect();" in body(renderer, "void Renderer::_UpdateDestRect() noexcept"),
          "SrcRect() returned a reference to a member it wrote, and it is called from the "
          "backend thread and from the window thread: a holder of `const RECT&` could see "
          "the value change under it. One writer (backend, next to _destRect) instead.")
    check("stage-released-on-stop",
          "_photoStage.reset()" in body(text("GMagpie/ScalingService.cpp"),
              "void ScalingService::_ScalingRuntime_StateChanged(ScalingState value)"),
          "the spec requires the invisible photo stage window to be released when the "
          "session stops; keeping it alive until the process exits leaks a top-level window")
    scaling = text("GMagpie/ScalingService.cpp")
    ensure_stage = body(scaling, "HWND ScalingService::_EnsurePhotoStage() noexcept")
    place_stage = body(scaling, "void ScalingService::_PlacePhotoStageAtCursor(HWND stage) noexcept")
    check("stage-on-cursor-monitor",
          ensure_stage.count("_PlacePhotoStageAtCursor(") == 2
          and all(token in place_stage for token in
                  ("GetCursorPos", "MonitorFromPoint", "PhotoStageRect(")),
          "the photo stage is the anchor the display is chosen from: every start must "
          "place it on the monitor under the mouse - on a reused window as well as a new one")

    # --- AppSettings: the resume state round-trips, and only when it exists --
    settings = text("GMagpie/AppSettings.cpp")
    save = body(settings, "std::string AppSettings::_Serialize(")
    check("resume-omitted-when-empty",
          "if (!data._galleryLastFolder.empty())" in save
          and save.index("if (!data._galleryLastFolder.empty())") < save.index('writer.Key("galleryLastFolder")'),
          "the config must not grow keys for a session nobody has started")
    check("resume-round-trip",
          'JsonHelper::ReadString(root, "galleryLastFolder"' in settings
          and 'JsonHelper::ReadUInt(root, "galleryLastIndex"' in settings,
          "the saved folder and page must be read back")

    # --- defaults: no autoplay unless someone asked for it -------------------
    cli = body(text("GMagpie/App.cpp"), "static void _TryStartGalleryFromCommandLine()")
    check("cli-default-interval",
          "uint32_t interval = 0;" in cli and "value > 0 ? static_cast<uint32_t>(value) : 0" in cli,
          "the interval default is 0 seconds; a negative or missing value must not start an "
          "autoplay nobody asked for")
    check("ui-default-interval", "double _galleryInterval = 0.0;" in text("GMagpie/HomeViewModel.h"),
          "the UI interval default must stay 0 as the user asked")

    return failures


# Each entry recreates one defect that was actually fixed, in memory only.
OPEN_PHOTO_CHECK = "bool Renderer::_ShouldAppendBicubic(ID3D11Texture2D* outTexture) noexcept {\n\tconst ScalingOptions& options = ScalingWindow::Get().Options();\n\tif (options.photoSession) {"

REGRESSIONS = {
    "always-normalize": ("GMagpie.Core/Renderer.cpp",
        [("if (options.photoSession) {\n\t\t// ", "if (false) {\n\t\t// ")]),
    "always-normalize-first": ("GMagpie.Core/Renderer.cpp",
        [(OPEN_PHOTO_CHECK, OPEN_PHOTO_CHECK.replace(
            "\tif (options.photoSession) {",
            "\tif (options.photoSession) return false;\n\tif (options.photoSession) {"))]),
    "stage-count-is-predicted": ("GMagpie.Core/Renderer.cpp",
        [("CalcPhotoEffectStages(_photoEffectOptionsAll", "size_t(_photoEffectOptionsAll.size()) //")]),
    "empty-chain-guard": ("GMagpie.Core/Renderer.cpp",
        [("_effectDrawers.empty()\n\t\t\t\t? _frameSource->GetPipelineTexture()", "_effectDrawers.back()")]),
    "cleanup-not-from-its-own-event": ("GMagpie/GalleryService.cpp",
        [("App::Get().Dispatcher().TryEnqueue([this] {\n\t\t\t\tif (_running) {\n\t\t\t\t\t_Cleanup();\n\t\t\t\t}\n\t\t\t});",
          "_Cleanup();")]),
    "photo-src-rect-single-writer": ("GMagpie.Core/Renderer.cpp",
        [("\t_photoSrcRect = { 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };",
          "\tdesc.Width = desc.Height = 0;"),
         ("\t\treturn _photoSrcRect;", "\t\t_photoSrcRect = {};\n\t\treturn _photoSrcRect;")]),
    "stage-released-on-stop": ("GMagpie/ScalingService.cpp", [("\t\t\t\t_photoStage.reset();", "")]),
    "stage-on-cursor-monitor": ("GMagpie/ScalingService.cpp",
        [("\t\t_PlacePhotoStageAtCursor(_photoStage.get());", "")]),
    "no-restart-for-photos": ("GMagpie/ScalingService.cpp",
        [("!_photoSessionActive && AppSettings::Get().IsSmoothMotionCompatibilityMode()",
          "AppSettings::Get().IsSmoothMotionCompatibilityMode()")]),
    "keys-come-from-tested-map": ("GMagpie/GalleryService.cpp",
        [("const auto action = DecideGalleryKey(vkCode, self._pickerVisible);",
          "const auto action = vkCode == VK_LEFT || vkCode == VK_UP\n"
          "\t\t\t\t? GalleryKeyAction::PagePrev : GalleryKeyAction::PassThrough;")]),
    "keys-need-focus": ("GMagpie/GalleryService.cpp",
        [("if (action != GalleryKeyAction::PassThrough && self._IsSessionForeground()) {",
          "if (action != GalleryKeyAction::PassThrough) {")]),
    "num6-single-toggle": ("GMagpie/GalleryService.cpp",
        [("\t\t\tself._num6Held = false;", "")]),
    "picker-stays-open-on-choice": ("GMagpie/GalleryService.cpp",
        [("\t\t_CycleUpscaler(+1, true);\n\t\tbreak;",
          "\t\t_CycleUpscaler(+1, true);\n\t\t_HideUpscalerPicker();\n\t\tbreak;")]),
    "vsr-in-upscaler-cycle": ("GMagpie/GalleryService.cpp",
        [("\t, std::string(NVIDIA_VFX_VSR_ID)", "")]),
    "picker-highlight-follows-choice": ("GMagpie/GalleryService.cpp",
        [("\t\t_ShowUpscalerPicker();\n\t}", "\t}")]),
    "cycle-dedup-profile-effect": ("GMagpie/GalleryService.cpp",
        [("\t\tstd::erase(_upscalerCycle, _originalFirst[0].name);\n", "")]),
    "delete-goes-to-recycle-bin": ("GMagpie/GalleryService.cpp",
        [("FOF_ALLOWUNDO | ", "")]),
    "delete-keeps-page-position": ("GMagpie/GalleryService.cpp",
        [("if (const auto next = PhotoIndexAfterDelete(_index, _playlist.size() + 1)) {",
          "if (const auto next = std::optional<size_t>(_index)) {")]),
    "compare-off-in-photo-session": ("GMagpie.Core/Renderer.cpp",
        [("if (value && ScalingWindow::Get().Options().photoSession) {", "if (false) {")]),
    "home-shows-gallery-progress": ("GMagpie/GalleryService.cpp",
        [("\tProgressChanged.Invoke(_index + 1, _playlist.size());\n", "")]),
    "gallery-lite-coexists": ("GMagpie/App.cpp",
        [(" std::wstring(CommonSharedConstants::SINGLE_INSTANCE_MUTEX_NAME) + L\"-Gallery\"",
          " CommonSharedConstants::SINGLE_INSTANCE_MUTEX_NAME")]),
    "gallery-lite-no-foreground-watcher": ("GMagpie/ScalingService.cpp",
        [("if (!App::Get().IsGalleryLiteMode()) {", "if (true) {")]),
    "start-last-resets-failure-reason": ("GMagpie/GalleryService.cpp",
        [("\t// Причина отказа актуальна только для текущего запуска: иначе при недоступной\n"
          "\t// папке главной странице показался бы тост с причиной ПРОШЛОГО отказа\n"
          "\t_lastStartFailure = GalleryStartFailure::Other;\n", "")]),
    "status-percent-respects-exif": ("GMagpie/GalleryService.cpp",
        [("if (PhotoAppliesExifRotation(orientation)) {", "if (false) {")]),
    "delete-survives-message-pump": ("GMagpie/GalleryService.cpp",
        [("const auto it = std::find(_playlist.begin(), _playlist.end(), path);",
          "auto it = _playlist.begin() + (long long)_index;")]),
    "frontend-dest-rect-snapshot": ("GMagpie.Core/Renderer.cpp",
        [("CalcPhotoSceneViewport(_frontendDestRect,", "CalcPhotoSceneViewport(_destRect,")]),
    "ngx-fault-keeps-frame-source": ("GMagpie.Core/Renderer.cpp",
        [("\t\t\twhile (GetMessage(&msg, NULL, 0, 0) > 0) {\n\t\t\t\tDispatchMessage(&msg);\n\t\t\t}\n\t\t\treturn;\n\t\t}\n\t\tif (_sessionLifetime->IsStopping())",
          "\t\t\treturn;\n\t\t}\n\t\tif (_sessionLifetime->IsStopping())")]),
    "restart-latest-request-wins": ("GMagpie.Core/ScalingRuntime.cpp",
        [("serial != _restartSerial.load", "false && _restartSerial.load")]),
    "stale-idle-tears-down-photo-session": ("GMagpie/ScalingService.cpp",
        [("if (value == ScalingState::Idle && _photoSessionActive) {\n\t\t\t\t_photoStage.reset();",
          "if (false) {\n\t\t\t\t_photoStage.reset();")]),
    "gallery-start-timer-failure-cleans-up": ("GMagpie/GalleryService.cpp",
        [("} catch (const winrt::hresult_error& e) {", "} catch (...) {")]),
    "page-request-first-in-hook": ("GMagpie/GalleryService.cpp",
        [("\tScalingService::Get().RequestPhoto(_playlist[_index]);\n\t_PrefetchNext();",
          "\t_UpdateStatusOverlay();\n\tScalingService::Get().RequestPhoto(_playlist[_index]);\n\t_PrefetchNext();")]),
    "stale-photo-skipped-with-limit": ("GMagpie.Core/Renderer.cpp",
        [("photoUpdateEnd - _lastPhotoRender < PHOTO_SKIP_WINDOW", "true")]),
    "photo-scene-scaled-to-dest": ("GMagpie.Core/Renderer.cpp",
        [("\t\t\t\t_DrawPhotoSceneScaled(scene, rtv, view);", "")]),
    "photo-scene-copy-before-draw": ("GMagpie.Core/Renderer.cpp",
        [("\tdc->CopyResource(_photoSceneCopy.get(), scene);", "")]),
    "overlay-owned-reanchors-below-photo": ("GMagpie/GalleryService.cpp",
        [("0, 0, 0, 0, nullptr, nullptr", "0, 0, 0, 0, owner, nullptr")]),
    "overlay-layered-sinks-below-photo": ("GMagpie/GalleryService.cpp",
        [("WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,",
          "WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,")]),
    "overlay-insert-after-owner-sinks": ("GMagpie/GalleryService.cpp",
        [("SetWindowPos(g_hwnd.get(), HWND_TOPMOST, x, y, w, h",
          "SetWindowPos(g_hwnd.get(), owner ? owner : HWND_TOPMOST, x, y, w, h"),
         ("SetWindowPos(g_hwnd.get(), HWND_TOPMOST, x, py, w, h",
          "SetWindowPos(g_hwnd.get(), owner ? owner : HWND_TOPMOST, x, py, w, h")]),
    "overlay-reasserts-topmost": ("GMagpie/GalleryService.cpp",
        [("SetTimer(g_hwnd.get(), Z_REASSERT_TIMER, 200, nullptr); // см. IsBelowOutputWindow\n",
          "")]),
    "photo-session-build-mode-fallback": ("GMagpie/ScalingService.cpp",
        [("\t\t\tprofile.scalingMode = static_cast<int>(*fallback);", "")]),
    "restart-keeps-page": ("GMagpie/GalleryService.cpp",
        [("\t\tScalingService::Get().RequestPhoto(_playlist[_index]);", "")]),
    "picker-no-self-hide": ("GMagpie/GalleryService.cpp",
        [("\t\t\t_ResourceString(_paused ? L\"Gallery_PausedToast\" : L\"Gallery_AutoPlayToast\"), scalingWindow);",
          "\t\t\t_ResourceString(_paused ? L\"Gallery_PausedToast\" : L\"Gallery_AutoPlayToast\"), scalingWindow);\n"
          "\t\tgallery_picker::Hide();  // дефект: список прячется не по выбору пользователя")]),
    "wheel-data-source": ("GMagpie/GalleryService.cpp",
        [("HIWORD(((MSLLHOOKSTRUCT*)lParam)->mouseData)", "GET_WHEEL_DELTA_WPARAM(wParam)")]),
    "wheel-need-focus": ("GMagpie/GalleryService.cpp",
        [("if (delta != 0 && self._IsSessionForeground()) {\n\t\t\t\tself._OnMouseWheel",
          "if (delta != 0) {\n\t\t\t\tself._OnMouseWheel")]),
    "cycle-uses-tested-index": ("GMagpie/GalleryService.cpp",
        [("NextUpscalerCycle(_cyclePos, (int)_upscalerCycle.size(), direction)",
          "(((_cyclePos + 1) + direction) % (int)_upscalerCycle.size() + 1) - 1")]),
    "resume-page-clamped": ("GMagpie/GalleryService.cpp",
        [("PhotoResumeIndex(startIndex, _playlist.size())", "startIndex")]),
    "state-saved-before-reset": ("GMagpie/GalleryService.cpp",
        [("\tif (_running) {\n\t\t_PersistSession(true);\n\t}\n\ttry {", "try {")]),
    "prefetch-follows-direction": ("GMagpie/GalleryService.cpp",
        [("PhotoPrefetchIndex(_index, _playlist.size(), _stepDirection)", "(_index + 1) % _playlist.size()")]),
    "resume-omitted-when-empty": ("GMagpie/AppSettings.cpp",
        [("if (!data._galleryLastFolder.empty()) {", "if (true) {")]),
    "cli-default-interval": ("GMagpie/App.cpp", [("uint32_t interval = 0;", "uint32_t interval = 3;")]),
    "ui-default-interval": ("GMagpie/HomeViewModel.h",
        [("double _galleryInterval = 0.0;", "double _galleryInterval = 5.0;")]),
}

SUMMARY = ("source invariants checked: always-normalize and its order, predicted stage count, "
           "empty chain, photo-switch rebind and rectangle order, first page presented like "
           "any page, event callback not self-revoking, one writer for the photo source "
           "rectangle, stage window released on stop, the stage anchors on the cursor's "
           "monitor, no self-restart, a build-disabled mode falls back to a working one, keys come from the tested map and need focus, held Num6 "
           "toggles once, the picker stays open on choice and never hides itself, the profile effect is never listed twice, RTX VSR is "
           "in the cycled list, restart keeps the page, the photo scene is drawn scaled into the fitted rectangle and copied to a private texture before drawing, overlay windows are ownerless topmost windows (not owned, not layered) re-asserted by a z-order timer, the picker highlight follows the choice, wheel data source and focus gating, "
           "tested cycle index, Delete only ever goes to the Recycle Bin and keeps the position, "
            "Compare is off and hidden in photo sessions, the home page mirrors the "
            "slideshow position, "
           "clamped resume, state save order, direction-aware prefetch, config round-trip, "
           "CLI and UI interval defaults")

if __name__ == "__main__":
    if "--self-test" in sys.argv:
        missed = []
        for name, (relative, patches) in REGRESSIONS.items():
            for old, _ in patches:
                assert old in text(relative), f"regression patch for {name} no longer applies"
            _PATCHES[relative] = patches
            caught = any(failure.startswith(name + ":") for failure in run_checks())
            _PATCHES.clear()
            print(f"{'detected' if caught else 'MISSED  '} {name}")
            if not caught:
                missed.append(name)
        run_checks()
        if missed:
            print(f"{len(missed)} regressions not detected: {missed}")
            sys.exit(1)
        print(f"Self-test: all {len(REGRESSIONS)} old defects are caught by these checks.")
        sys.exit(0)

    found = run_checks()
    print(SUMMARY)
    for failure in found:
        print("FAIL " + failure)
    if found:
        sys.exit(1)
    print("Photo gallery source invariants: every defect fixed earlier is still fixed in "
          "the shipped source.")
