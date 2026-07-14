#pragma once

#include <gst/gst.h>

#include <string>

#include "ConfigManager.h"

namespace UdpVbrOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    std::string& error);

} // namespace UdpVbrOutput
