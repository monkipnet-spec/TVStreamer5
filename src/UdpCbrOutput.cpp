#include "UdpCbrOutput.h"

#include "UdpTsOutput.h"

#include <algorithm>
#include <iostream>

namespace {

constexpr uint64_t kCbrSafetyHeadroomPercent = 105ULL;

} // namespace

namespace UdpCbrOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const std::string& sinkName,
    std::string& error) {
    if (config.targetBitrate == 0) {
        error = "UDP CBR target_bitrate must be greater than zero";
        return nullptr;
    }

    UdpTsOutput::PacingConfig pacing;

    // WISI compatibility is deliberately opt-in. The legacy UDP-CBR path below
    // is left byte-for-byte equivalent when the switch is disabled.
    //
    // On passthrough streams, forcing an arbitrary configured CBR rate can make
    // video accumulate behind the sender when the real TS/PCR rate is higher.
    // Audio often remains acceptable because it needs much less bandwidth, while
    // video appears to freeze. In WISI mode, pace the original transport stream
    // from its PCR clock instead of inserting synthetic NULL datagrams or forcing
    // the UI target bitrate. Packetization remains 7 x 188 bytes per UDP datagram.
    if (config.wisiCompatibility && !config.transcodeEnabled) {
        pacing.updateFromPcr = true;
        pacing.updateFromArrivalRate = false;
        pacing.initialBitrate = std::max<uint64_t>(config.targetBitrate, 8000000ULL);
        pacing.configuredBitrate = 0;
        pacing.headroomPercent = 100;
        pacing.holdConfiguredRateWhenSafe = false;
        std::cerr << "UDP CBR WISI compatibility: enabled, PCR-paced passthrough"
                  << " host=" << config.outputHost << ":" << config.outputPort
                  << " initial-bitrate=" << pacing.initialBitrate
                  << " target-bitrate-override=off" << std::endl;
        return UdpTsOutput::createSink(pipeline, config, sinkName, pacing, error);
    }

    pacing.updateFromPcr = false;
    pacing.updateFromArrivalRate = false;
    pacing.initialBitrate = config.targetBitrate;
    pacing.configuredBitrate = config.targetBitrate;
    pacing.headroomPercent = kCbrSafetyHeadroomPercent;
    pacing.holdConfiguredRateWhenSafe = true;
    return UdpTsOutput::createSink(pipeline, config, sinkName, pacing, error);
}

} // namespace UdpCbrOutput
