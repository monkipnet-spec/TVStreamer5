#include "StreamManager.h"

#include <iostream>
#include <sstream>
#include <regex>
#include <chrono>
#include <thread>

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
    std::cerr << "Pipeline for stream '" << streamConfig.name << "': " << pipelineDescription << std::endl;
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

    GstStateChangeReturn stateChange = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateChange == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to set pipeline to PLAYING for stream: " << streamConfig.name << std::endl;
        if (state->bus) {
            gst_object_unref(state->bus);
            state->bus = nullptr;
        }
        gst_object_unref(pipeline);
        return false;
    }

    state->statusMessage = (stateChange == GST_STATE_CHANGE_ASYNC) ? "starting" : "running";
    streams[streamConfig.id] = std::move(state);
    attachBitrateProbes(streams[streamConfig.id].get());
    streams[streamConfig.id]->busThread = std::thread(&StreamManager::monitorBus, this, streamConfig.id);
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
            updateBitrateEstimates(statePtr.get());
            uint64_t measured = queryPipelineBitrate(statePtr->pipeline);
            if (statePtr->inputBitrate.load() == 0 && measured > 0) {
                statePtr->inputBitrate = measured;
            }
            if (statePtr->outputBitrate.load() == 0) {
                statePtr->outputBitrate = statePtr->config.cbr ? statePtr->config.targetBitrate : statePtr->inputBitrate.load();
            }
            if (statePtr->outputBitrate.load() == 0 && statePtr->config.targetBitrate > 0) {
                statePtr->outputBitrate = statePtr->config.targetBitrate;
            }
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
    const std::string input = cfg.inputUri;
    const std::string inputLower = toLower(input);
    const std::string bindAddress = cfg.interfaceAddress.empty() ? "0.0.0.0" : cfg.interfaceAddress;

    auto extractHostPort = [](const std::string& uri, const std::string& scheme, std::string& host, int& port) -> bool {
        std::regex re("^" + scheme + R"(://([^:/]+):(\d+).*$)", std::regex::icase);
        std::smatch match;
        if (!std::regex_match(uri, match, re) || match.size() < 3) {
            return false;
        }
        host = match[1].str();
        port = std::stoi(match[2].str());
        return true;
    };

    if (inputLower.rfind("srt://", 0) == 0) {
        desc << "srtsrc uri=" << gstQuote(input)
             << " latency=120 wait-for-connection=true ! queue ! tsparse ! queue ";
    } else if (inputLower.rfind("http://", 0) == 0 || inputLower.rfind("https://", 0) == 0) {
        desc << "souphttpsrc location=" << gstQuote(input)
             << " is-live=true do-timestamp=true ! queue ! tsparse ! queue ";
    } else if (inputLower.rfind("udp://", 0) == 0) {
        std::string host;
        int port = 0;
        if (!extractHostPort(input, "udp", host, port)) {
            return "";
        }
        desc << "udpsrc port=" << port
             << " address=" << gstQuote(host)
             << " multicast-iface=" << gstQuote(bindAddress)
             << " auto-multicast=true buffer-size=2097152 ! queue ! tsparse ! queue ";
    } else if (inputLower.rfind("rtp://", 0) == 0) {
        std::string host;
        int port = 0;
        if (!extractHostPort(input, "rtp", host, port)) {
            return "";
        }
        desc << "udpsrc port=" << port
             << " address=" << gstQuote(host)
             << " multicast-iface=" << gstQuote(bindAddress)
             << " caps=\"application/x-rtp,media=video,encoding-name=MP2T,clock-rate=90000\" ! "
             << "rtpmp2tdepay ! queue ! tsparse ! queue ";
    } else {
        desc << "filesrc location=" << gstQuote(input) << " ! queue ! tsparse ! queue ";
    }

    desc << "! udpsink host=" << gstQuote(cfg.outputHost)
         << " port=" << cfg.outputPort
         << " bind-address=" << gstQuote(bindAddress)
         << " async=false sync=false";

    return desc.str();
}

void StreamManager::attachBitrateProbes(StreamState* state) {
    if (!state || !state->pipeline) {
        return;
    }

    GstIterator* iterator = gst_bin_iterate_elements(GST_BIN(state->pipeline));
    GValue item = G_VALUE_INIT;
    gboolean inputAttached = FALSE;
    gboolean outputAttached = FALSE;

    while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
        GstElement* element = GST_ELEMENT(g_value_get_object(&item));
        const gchar* factoryName = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(gst_element_get_factory(element)));

        if (!inputAttached && factoryName &&
            (g_strcmp0(factoryName, "srtsrc") == 0 ||
             g_strcmp0(factoryName, "souphttpsrc") == 0 ||
             g_strcmp0(factoryName, "udpsrc") == 0 ||
             g_strcmp0(factoryName, "filesrc") == 0)) {
            GstPad* srcPad = gst_element_get_static_pad(element, "src");
            if (srcPad) {
                gst_pad_add_probe(srcPad, GST_PAD_PROBE_TYPE_BUFFER, inputPadProbe, state, nullptr);
                gst_object_unref(srcPad);
                inputAttached = TRUE;
            }
        }

        if (!outputAttached && factoryName && g_strcmp0(factoryName, "udpsink") == 0) {
            GstPad* sinkPad = gst_element_get_static_pad(element, "sink");
            if (sinkPad) {
                gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER, outputPadProbe, state, nullptr);
                gst_object_unref(sinkPad);
                outputAttached = TRUE;
            }
        }

        g_value_unset(&item);
        if (inputAttached && outputAttached) {
            break;
        }
    }

    gst_iterator_free(iterator);
}

void StreamManager::updateBitrateEstimates(StreamState* state) {
    if (!state) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - state->lastBitrateSample).count();
    if (elapsedMs < 1000) {
        return;
    }

    uint64_t currentInputBytes = state->inputBytes.load();
    uint64_t currentOutputBytes = state->outputBytes.load();
    uint64_t inputDelta = currentInputBytes - state->lastInputBytesSample;
    uint64_t outputDelta = currentOutputBytes - state->lastOutputBytesSample;
    double seconds = static_cast<double>(elapsedMs) / 1000.0;

    state->inputBitrate = static_cast<uint64_t>((inputDelta * 8) / seconds);
    state->outputBitrate = static_cast<uint64_t>((outputDelta * 8) / seconds);

    if (state->config.cbr && state->outputBitrate.load() == 0 && state->config.targetBitrate > 0) {
        state->outputBitrate = state->config.targetBitrate;
    }

    state->lastInputBytesSample = currentInputBytes;
    state->lastOutputBytesSample = currentOutputBytes;
    state->lastBitrateSample = now;
}

GstPadProbeReturn StreamManager::inputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    (void)pad;
    auto* state = static_cast<StreamState*>(user_data);
    if (!state || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
    if (!buffer) {
        return GST_PAD_PROBE_OK;
    }
    state->inputBytes += gst_buffer_get_size(buffer);
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn StreamManager::outputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    (void)pad;
    auto* state = static_cast<StreamState*>(user_data);
    if (!state || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
    if (!buffer) {
        return GST_PAD_PROBE_OK;
    }
    state->outputBytes += gst_buffer_get_size(buffer);
    return GST_PAD_PROBE_OK;
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
