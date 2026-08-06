#pragma once

#include "ConfigManager.h"
#include <gst/gst.h>
#include <string>

class TranscoderModule {
public:
    // Builds a TS -> decode -> scale -> H.264/AAC -> MPEG-TS chain.
    // Returns the MPEG-TS mux element that must be used as the source for output branches.
    static GstElement* build(GstElement* pipeline, GstElement* sourceTail, const StreamConfig& config, std::string& error);

    static bool resolutionSize(const std::string& resolution, int& width, int& height);
    static uint64_t recommendedVideoBitrate(const std::string& resolution);
};
