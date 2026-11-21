#pragma once
#include <winrt/GMagpie.h>
#include "Shortcut.h"

namespace GMagpie {

enum class ShortcutError {
	NoError,
	Invalid,
	InUse
};

struct ShortcutHelper {
	static std::string ToString(winrt::GMagpie::ShortcutAction action) noexcept;

	static bool IsValidKeyCode(uint8_t code) noexcept;

	static ShortcutError CheckShortcut(Shortcut shortcut) noexcept;
};

}

namespace winrt {

// 将 ShortcutAction 映射为字符串
hstring to_hstring(GMagpie::ShortcutAction action);

}
