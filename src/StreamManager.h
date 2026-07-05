#pragma once

#include <gst/gst.h>
#include <jsoncpp/json/json.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ConfigManager.h"
#include "TelegramNotifier.h"
#include "utils.h"

struct StreamState {
    std::atomic<bool> active{false};
    std::atomic<bool> running{false};
    bool usingBackup = false;
    bool backupAttempted = false;
    std::string statusMessage = "stopped";
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::thread busThread;
    StreamConfig config;
    std::atomic<uint64_t> inputBitrate{0};
    std::atomic<uint64_t> outputBitrate{0};
};

class StreamManager {
public:
    explicit StreamManager(ConfigManager& cfg, TelegramNotifier& notifier);
    ~StreamManager();

    bool startStream(const StreamConfig& streamConfig);
    bool stopStream(const std::string& id);
    void stopAll();
    std::vector<std::string> activeStreams();
    std::map<std::string, StreamState*> snapshot();

private:
    bool gstreamerInitialized;
    std::string buildPipelineDescription(const StreamConfig& cfg);
    void monitorBus(const std::string& id);
    uint64_t queryPipelineBitrate(GstElement* pipeline);

    ConfigManager& configManager;
    TelegramNotifier& telegramNotifier;
    std::map<std::string, std::unique_ptr<StreamState>> streams;
    std::mutex managerMutex;
};
