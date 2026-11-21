#pragma once

namespace GMagpie {

struct ExclModeHelper {
	static wil::unique_mutex_nothrow EnterExclMode() noexcept;
};

}
