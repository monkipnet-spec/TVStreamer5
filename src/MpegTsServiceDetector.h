#pragma once

#include "ConfigManager.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct MpegTsServiceDetectionResult {
    std::vector<uint32_t> serviceIds;
    std::string error;
};

class MpegTsServiceDetector {
public:
    static bool supports(const StreamConfig& config);
    static MpegTsServiceDetectionResult detect(
        const StreamConfig& config,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(4000));
};
