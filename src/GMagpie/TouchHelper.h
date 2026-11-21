#pragma once

namespace GMagpie {

struct TouchHelper {
	static bool Register() noexcept;
	static bool Unregister() noexcept;

	static bool IsTouchSupportEnabled() noexcept;
	static void IsTouchSupportEnabled(bool value) noexcept;
	static bool TryLaunchTouchHelper(bool& isTouchSupportEnabled) noexcept;
};

}
