#pragma once

#include "ConfigManager.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tvs::protocols {

enum class OutputKind {
    UdpCbr,
    UdpVbr,
    Srt,
    Http,
    Hls,
    Rtmp,
    Youtube,
    Unknown
};

enum class ContainerKind {
    MpegTs,
    Flv
};

std::string normalizedOutputType(const StreamConfig& cfg);
OutputKind outputKind(const StreamConfig& cfg);
bool isUdpOutput(OutputKind kind);
bool isTsOutput(OutputKind kind);
bool isFlvOutput(OutputKind kind);

StreamOutputConfig primaryOutputConfig(const StreamConfig& cfg);
StreamConfig configForOutput(const StreamConfig& base, const StreamOutputConfig& output);
std::vector<StreamConfig> outputConfigs(const StreamConfig& cfg);

uint64_t safeVideoBitrate(const StreamConfig& cfg);
uint64_t safeAudioBitrate(const StreamConfig& cfg);
uint64_t muxBitrate(const StreamConfig& cfg);

std::string srtOutputMode(const StreamConfig& cfg);
std::string hlsDirectory(const StreamConfig& cfg);
std::string rtmpOutputLocation(const StreamConfig& cfg);

} // namespace tvs::protocols
