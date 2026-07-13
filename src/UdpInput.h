#pragma once

#include <gst/gst.h>

#include <string>

#include "ConfigManager.h"

namespace UdpInput {

bool handles(const std::string& uri);
GstElement* build(
    GstElement* pipeline,
    const StreamConfig& config,
    GstElement*& terminalElement,
    std::string& error);

} // namespace UdpInput
