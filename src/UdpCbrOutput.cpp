#include "UdpCbrOutput.h"

#include "UdpTsOutput.h"

namespace {

constexpr uint64_t kCbrSafetyHeadroomPercent = 105ULL;

} // namespace

namespace UdpCbrOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    std::string& error) {
    if (config.targetBitrate == 0) {
        error = "UDP CBR target_bitrate must be greater than zero";
        return nullptr;
    }

    UdpTsOutput::PacingConfig pacing;
    pacing.initialBitrate = config.targetBitrate;
    pacing.configuredBitrate = config.targetBitrate;
    pacing.headroomPercent = kCbrSafetyHeadroomPercent;
    pacing.holdConfiguredRateWhenSafe = true;
    return UdpTsOutput::createSink(pipeline, config, pacing, error);
}

} // namespace UdpCbrOutput
