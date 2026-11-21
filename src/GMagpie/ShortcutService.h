#pragma once
#include "Event.h"
#include <winrt/GMagpie.h>

namespace GMagpie {

class ShortcutService {
public:
	static ShortcutService& Get() noexcept {
		static ShortcutService instance;
		return instance;
	}

	ShortcutService(const ShortcutService&) = delete;
	ShortcutService(ShortcutService&&) = delete;

	void Initialize();

	void Uninitialize();

	bool IsError(winrt::GMagpie::ShortcutAction action) const noexcept {
		return _shortcutInfos[(size_t)action].isError;
	}

	void StopKeyboardHook() noexcept {
		_isKeyboardHookActive = false;
	}

	void StartKeyboardHook() noexcept {
		_isKeyboardHookActive = true;
	}

	Event<winrt::GMagpie::ShortcutAction> ShortcutActivated;

private:
	ShortcutService() = default;

	static LRESULT _WndProcStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		return Get()._WndProc(hWnd, msg, wParam, lParam);
	}
	LRESULT _WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

	void _AppSettings_OnShortcutChanged(winrt::GMagpie::ShortcutAction action);

	void _RegisterShortcut(winrt::GMagpie::ShortcutAction action);

	void _FireShortcut(winrt::GMagpie::ShortcutAction action);

	static LRESULT CALLBACK _LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

	struct _ShortcutInfo {
		std::chrono::steady_clock::time_point lastFireTime{};
		bool isError = true;
	};
	std::array<_ShortcutInfo, (size_t)winrt::GMagpie::ShortcutAction::COUNT_OR_NONE> _shortcutInfos{};
	wil::unique_hwnd _hwndHotkey;
	wil::unique_hhook _keyboardHook;

	Event<winrt::GMagpie::ShortcutAction>::EventRevoker _shortcutChangedRevoker;

	bool _isKeyboardHookActive = true;
	// 用于防止长按时重复触发热键
	bool _keyboardHookShortcutActivated = false;
};

}
