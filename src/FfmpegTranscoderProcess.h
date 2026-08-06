#pragma once

#include "ConfigManager.h"

#include <atomic>
#include <sys/types.h>
#include <string>
#include <vector>

class FfmpegTranscoderProcess {
public:
    FfmpegTranscoderProcess() = default;
    ~FfmpegTranscoderProcess();

    FfmpegTranscoderProcess(const FfmpegTranscoderProcess&) = delete;
    FfmpegTranscoderProcess& operator=(const FfmpegTranscoderProcess&) = delete;

    static bool isAvailable(std::string* path = nullptr);

    bool start(const StreamConfig& config, std::string& error);
    void stop();
    bool isRunning();
    std::string description() const;

private:
    struct ChildProcess {
        pid_t pid = -1;
        std::string description;
    };

    std::vector<ChildProcess> children;
    std::atomic<bool> stopping{false};

    static std::vector<std::string> buildCommand(
        const StreamConfig& baseConfig,
        const StreamConfig& outputConfig,
        std::string& description,
        std::string& error);

    static bool spawnProcess(const std::vector<std::string>& args, const std::string& description, ChildProcess& child, std::string& error);
};
