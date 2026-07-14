#include "UdpVbrOutput.h"

#include "UdpTsOutput.h"

namespace UdpVbrOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    std::string& error) {
    UdpTsOutput::PacingConfig pacing;
    pacing.enabled = false;
    return UdpTsOutput::createSink(pipeline, config, pacing, error);
}

} // namespace UdpVbrOutput
