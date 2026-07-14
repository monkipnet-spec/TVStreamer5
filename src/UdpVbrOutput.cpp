#include "UdpVbrOutput.h"

#include "UdpTsOutput.h"

namespace {

constexpr uint64_t kVbrInitialBitrate = 20000000ULL;
constexpr uint64_t kVbrPaceHeadroomPercent = 108ULL;

} // namespace

namespace UdpVbrOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    std::string& error) {
    UdpTsOutput::PacingConfig pacing;
    pacing.updateFromPcr = false;
    pacing.updateFromArrivalRate = true;
    pacing.initialBitrate = config.targetBitrate > kVbrInitialBitrate
        ? config.targetBitrate
        : kVbrInitialBitrate;
    pacing.headroomPercent = kVbrPaceHeadroomPercent;
    return UdpTsOutput::createSink(pipeline, config, pacing, error);
}

} // namespace UdpVbrOutput
