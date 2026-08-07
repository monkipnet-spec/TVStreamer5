#pragma once

#include <string>

namespace tvs::stream_protocols::inputs {
bool isRtpInput(const std::string& input, const std::string& mode, bool testPattern);
}
