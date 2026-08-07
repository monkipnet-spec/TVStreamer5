#pragma once
#include "ConfigManager.h"
#include <string>
namespace tvs::protocols::inputs {
bool isHlsInput(const StreamConfig& cfg);
std::string hlsInputUri(const StreamConfig& cfg);
}
