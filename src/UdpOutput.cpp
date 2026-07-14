#include "UdpOutput.h"

#include "UdpCbrOutput.h"
#include "UdpVbrOutput.h"

namespace UdpOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    std::string& error) {
    return config.cbr && config.targetBitrate > 0
        ? UdpCbrOutput::createSink(pipeline, config, error)
        : UdpVbrOutput::createSink(pipeline, config, error);
}

} // namespace UdpOutput
