#pragma once
#include "ConfigManager.h"
#include <string>
namespace tvs::protocols::inputs {
bool isUdpInput(const StreamConfig& cfg);
std::string udpInputUri(const StreamConfig& cfg);
}
