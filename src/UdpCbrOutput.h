#pragma once

#include <gst/gst.h>

#include <string>

#include "ConfigManager.h"

namespace UdpCbrOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const std::string& sinkName,
    std::string& error);

} // namespace UdpCbrOutput
