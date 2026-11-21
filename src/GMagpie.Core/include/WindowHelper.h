#pragma once

namespace GMagpie {

struct WindowHelper {
	static bool IsStartMenu(HWND hWnd) noexcept;

	static bool IsForbiddenSystemWindow(HWND hwndSrc) noexcept;
};

}
