#pragma once

#include <string>

namespace tvs::stream_protocols::inputs {
bool isHttpInput(const std::string& input, const std::string& mode, bool testPattern);
}
