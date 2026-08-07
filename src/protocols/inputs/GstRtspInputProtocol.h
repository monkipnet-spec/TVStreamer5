#pragma once
#include "ConfigManager.h"
#include <string>
namespace tvs::protocols::inputs {
bool isRtspInput(const StreamConfig& cfg);
std::string rtspInputUri(const StreamConfig& cfg);
}
