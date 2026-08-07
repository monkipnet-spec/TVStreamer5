#pragma once

#include "ConfigManager.h"
#include <string>

namespace tvs::protocols::inputs {

bool isRtpInput(const StreamConfig& cfg);
std::string rtpInputUri(const StreamConfig& cfg);

} // namespace tvs::protocols::inputs
