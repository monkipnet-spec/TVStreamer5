#pragma once

#include <string>

namespace tvs::stream_protocols::inputs {
bool isSrtInput(const std::string& input, const std::string& mode, bool testPattern);
}
