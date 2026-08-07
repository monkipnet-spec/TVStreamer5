#include "protocols/stream/inputs/StreamTestInputProtocol.h"

namespace tvs::stream_protocols::inputs {
bool isTestInput(const std::string& input, const std::string& mode, bool testPattern) {
    return testPattern || input == "test://bars" || input == "testsrc://bars" || input == "bars://hd";
}
}
