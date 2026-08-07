#pragma once

#include "protocols/GstProtocolTypes.h"

#include <string>
#include <vector>

namespace tvs::protocols {

struct GstOutputSpec {
    OutputKind kind = OutputKind::Unknown;
    ContainerKind container = ContainerKind::MpegTs;
    std::string videoPad;
    std::string audioPad;
    std::string description;
};

std::vector<std::string> requiredOutputElements();
std::vector<std::string> requiredElementsForOutput(OutputKind kind);

bool appendOutputMuxAndSink(
    std::vector<std::string>& args,
    const StreamConfig& cfg,
    GstOutputSpec& spec,
    std::string& error);

} // namespace tvs::protocols
