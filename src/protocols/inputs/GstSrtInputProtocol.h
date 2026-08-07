#pragma once
#include "ConfigManager.h"
#include <string>
namespace tvs::protocols::inputs {
bool isSrtInput(const StreamConfig& cfg);
std::string srtInputUri(const StreamConfig& cfg);
}
