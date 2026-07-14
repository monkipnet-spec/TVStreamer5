#include "UdpVbrOutput.h"

#include "UdpTsOutput.h"

namespace {

constexpr uint64_t kVbrPaceHeadroomPercent = 110ULL;

} // namespace

namespace UdpVbrOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    std::string& error) {
    UdpTsOutput::PacingConfig pacing;
    pacing.headroomPercent = kVbrPaceHeadroomPercent;
    return UdpTsOutput::createSink(pipeline, config, pacing, error);
}

} // namespace UdpVbrOutput
