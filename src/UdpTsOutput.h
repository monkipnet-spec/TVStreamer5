#pragma once

#include <gst/gst.h>

#include <cstdint>
#include <string>

#include "ConfigManager.h"

namespace UdpTsOutput {

struct PacingConfig {
    bool enabled = true;
    uint64_t initialBitrate = 0;
    uint64_t configuredBitrate = 0;
    uint64_t headroomPercent = 100;
    bool holdConfiguredRateWhenSafe = false;
};

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const PacingConfig& pacing,
    std::string& error);

} // namespace UdpTsOutput
