#include "protocols/GstInputProtocols.h"

#include "utils.h"

namespace tvs::protocols {

std::string inputUriForGstreamer(const StreamConfig& cfg) {
    std::string input = cfg.inputUri;
    const std::string lower = toLower(input);
    if (lower.rfind("hls://", 0) == 0) {
        input = "http://" + input.substr(6);
    }
    return input;
}

void appendDecodeInput(std::vector<std::string>& args, const StreamConfig& cfg) {
    args.insert(args.end(), {
        "uridecodebin",
        "name=dec",
        "uri=" + inputUriForGstreamer(cfg),
        "use-buffering=true"
    });
}

std::vector<std::string> requiredInputElements() {
    return {"uridecodebin"};
}

} // namespace tvs::protocols
