#pragma once

#include <gst/gst.h>

#include <string>

#include "ConfigManager.h"

namespace UdpOutput {

bool build(
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& config,
    std::string& error);

} // namespace UdpOutput
