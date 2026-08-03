#include "UdpOutput.h"

#include "UdpCbrOutput.h"
#include "UdpVbrOutput.h"

namespace UdpOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const std::string& sinkName,
    std::string& error) {
    return config.cbr && config.targetBitrate > 0
        ? UdpCbrOutput::createSink(pipeline, config, sinkName, error)
        : UdpVbrOutput::createSink(pipeline, config, sinkName, error);
}

} // namespace UdpOutput
