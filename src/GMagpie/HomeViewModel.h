#pragma once
#include "HomeViewModel.g.h"
#include "Event.h"

namespace GMagpie { struct Profile; }

namespace winrt::GMagpie::implementation {

struct HomeViewModel : HomeViewModelT<HomeViewModel>, wil::notify_property_changed_base<HomeViewModel> {
	HomeViewModel();
	bool ShowDlssNrStatus() const;
	hstring DlssNrStatusSummary() const;
	void OpenDlssNrEffects();
	bool ShowRecentIssue() const noexcept;
	void ShowRecentIssue(bool value);
	hstring RecentIssueSummary() const noexcept;
	MUXC::InfoBarSeverity RecentIssueSeverity() const noexcept;
	bool ShowIssueHistory() const noexcept;
	fire_and_forget ShowRecentIssueDetails() noexcept;

	hstring TimerDescription() const noexcept;

	bool IsTimerOn() const noexcept;

	double TimerProgressRingValue() const noexcept;

	hstring TimerLabelText() const noexcept;

	bool IsNotRunning() const noexcept;

	hstring TimerButtonText(bool windowedMode) const noexcept;

	void ToggleTimerFullscreen() const noexcept;

	void ToggleTimerWindowed() const noexcept;

	uint32_t Delay() const noexcept;
	void Delay(uint32_t value);

	hstring DelayText() const noexcept;

	bool ShowUpdateCard() const noexcept {
		return _showUpdateCard;
	}

	void ShowUpdateCard(bool value) noexcept;

	hstring UpdateCardTitle() const noexcept;

	bool IsAutoCheckForUpdates() const noexcept;
	void IsAutoCheckForUpdates(bool value) noexcept;

	void DownloadAndInstall();

	void ReleaseNotes();

	void RemindMeLater();

	hstring InitialToolbarStateDescription() const noexcept;

	int FullscreenInitialToolbarState() const noexcept;
	void FullscreenInitialToolbarState(int value);

	int WindowedInitialToolbarState() const noexcept;
	void WindowedInitialToolbarState(int value);

	hstring ScreenshotSaveDirectory() const noexcept;
	int ScreenshotContent() const noexcept;
	void ScreenshotContent(int value);

	void OpenScreenshotSaveDirectory() const noexcept;

	fire_and_forget ChangeScreenshotSaveDirectory() noexcept;

	// Фотогалерея: выбор папки и запуск GalleryService, интервал автолистания
	double GalleryInterval() const noexcept;
	void GalleryInterval(double value) noexcept;
	fire_and_forget PickGalleryFolder() noexcept;
	void ResumeGallery();
	void StopGallery() noexcept;
	// Есть ли что продолжать: папка прошлой сессии всё ещё доступна
	bool HasGalleryResume() const noexcept;
	hstring GalleryResumeInfo() const noexcept;
	// Прогресс идущего показа рядом с «Продолжить»: "N / M", как в строке состояния
	// показа; пусто, когда показа нет
	hstring GalleryProgressText() const noexcept;

	bool IsTouchSupportEnabled() const noexcept;
	fire_and_forget IsTouchSupportEnabled(bool value);

	Uri TouchSupportLearnMoreUrl() const noexcept;

	bool IsShowTouchSupportInfoBar() const noexcept;

	bool IsAllowScalingMaximized() const noexcept;
	void IsAllowScalingMaximized(bool value);

	bool IsSimulateExclusiveFullscreen() const noexcept;
	void IsSimulateExclusiveFullscreen(bool value);

	bool IsInlineParams() const noexcept;
	void IsInlineParams(bool value);

	bool IsStopEffectsOnTaskSwitchEnabled() const noexcept;
	void IsStopEffectsOnTaskSwitchEnabled(bool value);
	bool IsVRREnabled() const noexcept;
	void IsVRREnabled(bool value);

	hstring DefaultFrameRefreshSummary() const;
	void EditDefaultFrameRefresh();

	bool IsDeveloperMode() const noexcept;
	void IsDeveloperMode(bool value);

	void LocateMagpieLogs() noexcept;
	void LocateTouchHelperLogs() noexcept;
	void LocateUpdaterLogs() noexcept;

	bool IsDebugMode() const noexcept;
	void IsDebugMode(bool value);

	bool IsBenchmarkMode() const noexcept;
	void IsBenchmarkMode(bool value);

	bool IsTopmostDisabled() const noexcept;
	void IsTopmostDisabled(bool value);

	bool IsEffectCacheDisabled() const noexcept;
	void IsEffectCacheDisabled(bool value);

	bool IsFontCacheDisabled() const noexcept;
	void IsFontCacheDisabled(bool value);

	bool IsSaveEffectSources() const noexcept;
	void IsSaveEffectSources(bool value);

	bool IsWarningsAreErrors() const noexcept;
	void IsWarningsAreErrors(bool value);

	bool IsFP16Disabled() const noexcept;
	void IsFP16Disabled(bool value);

	int DuplicateFrameDetectionMode() const noexcept;
	void DuplicateFrameDetectionMode(int value);

	bool IsDynamicDection() const noexcept;

	bool IsStatisticsForDynamicDetectionEnabled() const noexcept;
	void IsStatisticsForDynamicDetectionEnabled(bool value);

private:
	::GMagpie::Event<>::EventRevoker _duplicateFrameDetectionModeChangedRevoker;
	::GMagpie::Event<::GMagpie::Profile&>::EventRevoker _frameRefreshChangedRevoker;
	::GMagpie::Event<>::EventRevoker _frameRefreshModeChangedRevoker;
	void _ScalingService_IsTimerOnChanged(bool value, bool windowedMode);

	void _RaiseGalleryResumeChanged();
	void _NotifyGalleryStartFailure();

	::GMagpie::Event<size_t, size_t>::EventRevoker _galleryProgressRevoker;

	void _ScalingService_TimerTick(double);

	void _ScalingService_IsScalingChanged(bool);

	void _ToggleTimer(bool windowedMode) const noexcept;

	::GMagpie::Event<bool, bool>::EventRevoker _isTimerOnRevoker;
	::GMagpie::Event<>::EventRevoker _issueChangedRevoker;
	::GMagpie::Event<double>::EventRevoker _timerTickRevoker;
	::GMagpie::Event<bool>::EventRevoker _isScalingChangedRevoker;
	::GMagpie::Event<bool>::EventRevoker _isShowOnHomePageChangedRevoker;

	bool _showUpdateCard = false;

	std::wstring _galleryFolder;
	// Автолистание по умолчанию выключено: альбом смотрят вручную, а интервал
	// намеренно не сохраняется — после перезапуска снова ручной режим
	double _galleryInterval = 0.0;
};

}
