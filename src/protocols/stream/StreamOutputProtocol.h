#pragma once

#include "ConfigManager.h"

#include <string>
#include <vector>

namespace tvs::stream_protocols {

enum class OutputProtocolKind {
    Udp,
    UdpCbr,
    UdpVbr,
    Rtp,
    Http,
    Hls,
    Srt,
    Rtsp,
    Rtmp,
    Youtube,
    Unknown
};

OutputProtocolKind outputKind(const StreamConfig& cfg);
std::string outputKindName(OutputProtocolKind kind);
std::vector<const char*> requiredElementsForOutput(OutputProtocolKind kind);
bool isUdpLikeOutput(OutputProtocolKind kind);
bool isTsOutput(OutputProtocolKind kind);
bool isFlvOutput(OutputProtocolKind kind);

} // namespace tvs::stream_protocols
