#pragma once

#include <gst/gst.h>
#include <jsoncpp/json/json.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

#include "ConfigManager.h"
#include "TelegramNotifier.h"
#include "utils.h"

struct RemapContext {
    GstElement* mux = nullptr;
    GstElement* sink = nullptr;
    StreamConfig config;
    bool videoLinked = false;
    bool audioLinked = false;
    bool flvMux = false;
    std::string videoPadName;
    std::string audioPadName;
};

struct StreamState {
    std::atomic<bool> active{false};
    std::atomic<bool> running{false};
    bool usingBackup = false;
    bool backupAttempted = false;
    bool primaryRetryPending = false;
    bool inputLossNotified = false;
    std::string statusMessage = "stopped";
    std::string primaryInputUri;
    std::string activeInputUri;
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::thread busThread;
    StreamConfig config;
    std::atomic<uint64_t> inputBitrate{0};
    std::atomic<uint64_t> outputBitrate{0};
    std::atomic<uint64_t> inputBytes{0};
    std::atomic<uint64_t> outputBytes{0};
    std::chrono::steady_clock::time_point lastBitrateSample = std::chrono::steady_clock::now();
    uint64_t lastInputBytesSample = 0;
    uint64_t lastOutputBytesSample = 0;
    uint64_t lastInputBytesSeen = 0;
    std::chrono::steady_clock::time_point lastInputActivity = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastPrimaryRetry = std::chrono::steady_clock::now();
    std::unique_ptr<RemapContext> sourceContext;
    std::unique_ptr<RemapContext> remapContext;
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
    bool addHttpClient(const std::string& id, int fd);

private:
    bool gstreamerInitialized;
    std::string buildPipelineDescription(const StreamConfig& cfg);
    GstElement* createPipeline(StreamState* state);
    GstElement* createSourceChain(StreamState* state, GstElement* pipeline, GstElement*& terminalElement);
    GstElement* createTestPatternChain(const StreamConfig& cfg, GstElement* pipeline, GstElement*& terminalElement);
    bool buildPassthroughPipeline(StreamState* state, GstElement* pipeline, GstElement* sourceTail);
    bool buildRemapPipeline(StreamState* state, GstElement* pipeline, GstElement* sourceTail);
    bool buildRtmpOutputPipeline(StreamState* state, GstElement* pipeline, GstElement* sourceTail);
    GstElement* createOutputSink(const StreamConfig& cfg, GstElement* pipeline);
    bool restartPipelineWithInput(StreamState* state, const std::string& inputUri, bool useBackup);
    void notifyStreamState(const StreamConfig& cfg, const std::string& color, const std::string& title, const std::string& details);
    static void onDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data);
    static void onFlvDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data);
    static void onRtspPadAdded(GstElement* src, GstPad* pad, gpointer user_data);
    void monitorBus(const std::string& id);
    uint64_t queryPipelineBitrate(GstElement* pipeline);
    void attachBitrateProbes(StreamState* state);
    void updateBitrateEstimates(StreamState* state);
    static GstPadProbeReturn inputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static GstPadProbeReturn outputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);

    ConfigManager& configManager;
    TelegramNotifier& telegramNotifier;
    std::map<std::string, std::unique_ptr<StreamState>> streams;
    std::mutex managerMutex;
};
