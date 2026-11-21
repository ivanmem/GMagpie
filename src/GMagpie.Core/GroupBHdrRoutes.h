#pragma once

#include "HdrProtocol.h"

namespace GMagpie {

HdrFormatRoutes GetGroupBHdrRoutes(
	std::string_view effectGroup,
	bool experimentalDlssnr = false,
	float dlssnrScale = 1.0f
) noexcept;

}
