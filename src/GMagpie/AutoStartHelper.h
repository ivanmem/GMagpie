#pragma once

namespace GMagpie {

struct AutoStartHelper {
	static bool EnableAutoStart(bool runElevated) noexcept;
	static bool DisableAutoStart() noexcept;
	static bool IsAutoStartEnabled() noexcept;
};

}
