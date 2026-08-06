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
    static TranscoderCapabilities inspectCapabilities();

    // Creates a completely isolated GstBin with one MPEG-TS sink ghost pad and one
    // MPEG-TS source ghost pad. The bin owns demuxing, decoding, scaling, encoding,
    // audio conversion, remuxing and ignored-pad draining.
    static GstElement* createBin(const StreamConfig& config, std::string& error);

    static bool resolutionSize(const std::string& resolution, int& width, int& height);
    static uint64_t recommendedVideoBitrate(const std::string& resolution);
};
