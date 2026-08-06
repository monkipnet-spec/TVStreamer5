#pragma once

#include "ConfigManager.h"
#include <gst/gst.h>
#include <string>
#include <vector>

struct TranscoderCapabilities {
    bool available = false;
    std::string videoEncoder;
    std::string audioEncoder;
    std::vector<std::string> missingElements;
    std::string message;
};

class TranscoderModule {
public:
    // Inspects runtime GStreamer factories. Passthrough streaming remains available
    // even when transcoding dependencies are missing.
    static TranscoderCapabilities inspectCapabilities();

    // Builds a TS -> decode -> scale -> H.264/AAC -> MPEG-TS chain.
    // Returns the MPEG-TS mux element that must be used as the source for output branches.
    static GstElement* build(GstElement* pipeline, GstElement* sourceTail, const StreamConfig& config, std::string& error);

    static bool resolutionSize(const std::string& resolution, int& width, int& height);
    static uint64_t recommendedVideoBitrate(const std::string& resolution);
};
