#include "StreamManager.h"

#include <iostream>

StreamManager::StreamManager(ConfigManager& cfg, TelegramNotifier& notifier)
    : configManager(cfg), telegramNotifier(notifier), gstreamerInitialized(false) {
    std::cerr << "StreamManager constructed" << std::endl;
}

StreamManager::~StreamManager() {
    stopAll();
}

bool StreamManager::startStream(const StreamConfig& streamConfig) {
    std::lock_guard<std::mutex> lock(managerMutex);
    if (streams.count(streamConfig.id)) {
        return false;
    }

    if (!gstreamerInitialized) {
        gst_init(nullptr, nullptr);
        gstreamerInitialized = true;
    }

    std::string pipelineDescription = buildPipelineDescription(streamConfig);
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(pipelineDescription.c_str(), &error);
    if (!pipeline) {
        std::cerr << "Failed to create pipeline: " << (error ? error->message : "unknown") << std::endl;
        if (error) g_error_free(error);
        return false;
    }

    auto state = std::make_unique<StreamState>();
    state->config = streamConfig;
    state->pipeline = pipeline;
    state->bus = gst_element_get_bus(pipeline);
    state->running = true;
    state->active = true;
    state->statusMessage = "starting";
    state->outputBitrate = streamConfig.targetBitrate;
    state->busThread = std::thread(&StreamManager::monitorBus, this, streamConfig.id);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    streams[streamConfig.id] = std::move(state);
    telegramNotifier.sendMessage("Stream started: " + streamConfig.name);
    return true;
}

bool StreamManager::stopStream(const std::string& id) {
    std::lock_guard<std::mutex> lock(managerMutex);
    if (!streams.count(id)) {
        return false;
    }
    auto& state = *streams[id];
    state.running = false;
    if (state.pipeline) {
        gst_element_set_state(state.pipeline, GST_STATE_NULL);
        state.active = false;
        state.statusMessage = "stopped";
    }
    if (state.busThread.joinable()) {
        state.busThread.join();
    }
    if (state.bus) {
        gst_object_unref(state.bus);
        state.bus = nullptr;
    }
    if (state.pipeline) {
        gst_object_unref(state.pipeline);
        state.pipeline = nullptr;
    }
    streams.erase(id);
    telegramNotifier.sendMessage("Stream stopped: " + id);
    return true;
}

void StreamManager::stopAll() {
    std::lock_guard<std::mutex> lock(managerMutex);
    for (auto& [id, statePtr] : streams) {
        auto& state = *statePtr;
        state.running = false;
        if (state.pipeline) {
            gst_element_set_state(state.pipeline, GST_STATE_NULL);
        }
        if (state.busThread.joinable()) {
            state.busThread.join();
        }
        if (state.bus) {
            gst_object_unref(state.bus);
        }
        if (state.pipeline) {
            gst_object_unref(state.pipeline);
        }
    }
    streams.clear();
}

std::vector<std::string> StreamManager::activeStreams() {
    std::lock_guard<std::mutex> lock(managerMutex);
    std::vector<std::string> result;
    for (auto& [id, statePtr] : streams) {
        if (statePtr->active.load()) {
            result.push_back(id);
        }
    }
    return result;
}

std::map<std::string, StreamState*> StreamManager::snapshot() {
    std::lock_guard<std::mutex> lock(managerMutex);
    std::map<std::string, StreamState*> result;
    for (auto& [id, statePtr] : streams) {
        if (statePtr->pipeline) {
            statePtr->inputBitrate = queryPipelineBitrate(statePtr->pipeline);
            statePtr->outputBitrate = statePtr->config.cbr ? statePtr->config.targetBitrate : statePtr->inputBitrate.load();
        }
        result[id] = statePtr.get();
    }
    return result;
}

uint64_t StreamManager::queryPipelineBitrate(GstElement* pipeline) {
    if (!pipeline) {
        return 0;
    }
    GstQuery* query = gst_query_new_bitrate();
    if (!gst_element_query(pipeline, query)) {
        gst_query_unref(query);
        return 0;
    }
    guint nominal_bitrate = 0;
    gst_query_parse_bitrate(query, &nominal_bitrate);
    gst_query_unref(query);
    return static_cast<uint64_t>(nominal_bitrate);
}

std::string StreamManager::buildPipelineDescription(const StreamConfig& cfg) {
    std::ostringstream desc;
    std::string absIntf = cfg.interfaceAddress.empty() ? "0.0.0.0" : cfg.interfaceAddress;

    if (toLower(cfg.inputUri).rfind("http://", 0) == 0 ||
        toLower(cfg.inputUri).rfind("https://", 0) == 0 ||
        toLower(cfg.inputUri).rfind("srt://", 0) == 0 ||
        toLower(cfg.inputUri).rfind("rtp://", 0) == 0 ||
        toLower(cfg.inputUri).rfind("udp://", 0) == 0) {
        desc << "urisourcebin uri=" << gstQuote(cfg.inputUri) << " name=src ! ";
    }

    desc << "tsdemux name=demux demux. ! queue ! h264parse ! mpegtsmux name=mux "
         << "alignment=7 bitrate=" << (cfg.cbr ? cfg.targetBitrate : 0) << " ";

    if (!cfg.serviceName.empty()) {
        desc << "service-name=" << gstQuote(cfg.serviceName) << " ";
    }
    if (!cfg.serviceProvider.empty()) {
        desc << "service-provider=" << gstQuote(cfg.serviceProvider) << " ";
    }

    desc << "! udpsink host=" << gstQuote(cfg.outputHost) << " port=" << cfg.outputPort
         << " bind-address=" << gstQuote(absIntf) << " async=false sync=false";

    return desc.str();
}

void StreamManager::monitorBus(const std::string& id) {
    auto found = streams.find(id);
    if (found == streams.end()) {
        return;
    }
    StreamState* state = found->second.get();
    GstBus* bus = state->bus;
    while (state->running.load()) {
        GstMessage* msg = gst_bus_timed_pop(bus, 1000000000LL);
        if (!msg) continue;
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                std::string message = err ? err->message : "unknown";
                g_error_free(err);
                g_free(dbg);
                if (!state->usingBackup && !state->backupAttempted && !state->config.backupInputUri.empty() && state->config.backupInputUri != state->config.inputUri) {
                    state->backupAttempted = true;
                    state->statusMessage = "switching to backup";
                    telegramNotifier.sendMessage("Stream error: " + found->first + " -> " + message + ", switching to backup");
                    gst_message_unref(msg);
                    if (state->pipeline) {
                        gst_element_set_state(state->pipeline, GST_STATE_NULL);
                        gst_object_unref(state->pipeline);
                        state->pipeline = nullptr;
                    }
                    if (state->bus) {
                        gst_object_unref(state->bus);
                        state->bus = nullptr;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    state->config.inputUri = state->config.backupInputUri;
                    state->usingBackup = true;
                    std::string backupPipeline = buildPipelineDescription(state->config);
                    GError* backupError = nullptr;
                    GstElement* backupPipelineElement = gst_parse_launch(backupPipeline.c_str(), &backupError);
                    if (!backupPipelineElement) {
                        std::string errorText = backupError ? backupError->message : "unknown";
                        if (backupError) g_error_free(backupError);
                        state->statusMessage = "backup failed: " + errorText;
                        state->active = false;
                        telegramNotifier.sendMessage("Backup stream failed: " + found->first + " -> " + errorText);
                        return;
                    }
                    state->pipeline = backupPipelineElement;
                    state->bus = gst_element_get_bus(backupPipelineElement);
                    bus = state->bus;
                    state->statusMessage = "backup starting";
                    state->active = true;
                    gst_element_set_state(backupPipelineElement, GST_STATE_PLAYING);
                    continue;
                }
                state->statusMessage = "error: " + message;
                state->active = false;
                telegramNotifier.sendMessage("Stream error: " + found->first + " -> " + message);
                gst_message_unref(msg);
                return;
            }
            case GST_MESSAGE_EOS:
                state->statusMessage = "ended";
                state->active = false;
                telegramNotifier.sendMessage("Stream ended: " + found->first);
                gst_message_unref(msg);
                return;
            default:
                gst_message_unref(msg);
                break;
        }
    }
}
