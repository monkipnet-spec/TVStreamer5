#include "StreamManager.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#include <glib.h>

namespace {

bool hasElementFactory(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

std::string missingElementStatus(const std::string& elementName) {
    return "missing element: " + elementName;
}

bool addElementOrFail(GstElement* pipeline, GstElement* element) {
    return element != nullptr && gst_bin_add(GST_BIN(pipeline), element);
}

bool isMulticastHost(const std::string& host) {
    std::regex re(R"(^((22[4-9])|(23[0-9]))\.)");
    return std::regex_search(host, re);
}

void configureUdpSink(GstElement* sink, const StreamConfig& cfg) {
    g_object_set(sink,
        "host", cfg.outputHost.c_str(),
        "port", cfg.outputPort,
        "async", FALSE,
        "sync", FALSE,
        nullptr);

    if (isMulticastHost(cfg.outputHost)) {
        g_object_set(sink, "auto-multicast", TRUE, nullptr);
        if (!cfg.interfaceAddress.empty()) {
            g_object_set(sink, "multicast-iface", cfg.interfaceAddress.c_str(), nullptr);
        }
    } else if (!cfg.interfaceAddress.empty()) {
        g_object_set(sink, "bind-address", cfg.interfaceAddress.c_str(), nullptr);
    }
}

} // namespace

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

    auto state = std::make_unique<StreamState>();
    state->config = streamConfig;
    if (streamConfig.remapEnabled) {
        state->remapContext = std::make_unique<RemapContext>();
        state->remapContext->config = streamConfig;
    }

    GstElement* pipeline = createPipeline(streamConfig);
    if (!pipeline) {
        state->statusMessage = "pipeline build failed";
        return false;
    }

    std::cerr << "Pipeline for stream '" << streamConfig.name
              << "': " << buildPipelineDescription(streamConfig) << std::endl;

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
            GstMessage* msg = gst_bus_timed_pop_filtered(
                state->bus,
                2 * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
            if (msg) {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    GError* err = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_error(msg, &err, &dbg);
                    std::cerr << "PLAYING error: " << (err ? err->message : "unknown")
                              << " debug=" << (dbg ? dbg : "") << std::endl;
                    if (err) g_error_free(err);
                    g_free(dbg);
                } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
                    GError* err = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_warning(msg, &err, &dbg);
                    std::cerr << "PLAYING warning: " << (err ? err->message : "unknown")
                              << " debug=" << (dbg ? dbg : "") << std::endl;
                    if (err) g_error_free(err);
                    g_free(dbg);
                }
                gst_message_unref(msg);
            }
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
    state.remapContext.reset();

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
        state.remapContext.reset();
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
            if (statePtr->outputBitrate.load() == 0 &&
                statePtr->config.targetBitrate > 0 &&
                statePtr->config.cbr) {
                statePtr->outputBitrate = statePtr->config.targetBitrate;
            }
        }
        result[id] = statePtr.get();
    }
    return result;
}

uint64_t StreamManager::queryPipelineBitrate(GstElement* pipeline) {
    (void)pipeline;
    return 0;
}
std::string StreamManager::buildPipelineDescription(const StreamConfig& cfg) {
    std::ostringstream desc;
    desc << "manual-pipeline"
         << " input=" << cfg.inputUri
         << " input_mode=" << cfg.inputMode
         << " remap=" << (cfg.remapEnabled ? "on" : "off")
         << " output=" << cfg.outputHost << ":" << cfg.outputPort
         << " iface=" << cfg.interfaceAddress
         << " service_id=" << cfg.serviceId
         << " vpid=" << cfg.videoPid
         << " apid=" << cfg.audioPid;
    return desc.str();
}

GstElement* StreamManager::createPipeline(const StreamConfig& cfg) {
    GstElement* pipeline = gst_pipeline_new(cfg.id.c_str());
    if (!pipeline) {
        return nullptr;
    }

    GstElement* sourceTail = nullptr;
    if (!createSourceChain(cfg, pipeline, sourceTail) || !sourceTail) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    bool ok = false;
    if (cfg.remapEnabled) {
        RemapContext remapContext;
        remapContext.config = cfg;
        StreamState tempState;
        tempState.config = cfg;
        tempState.remapContext = std::make_unique<RemapContext>(remapContext);
        ok = buildRemapPipeline(&tempState, pipeline, sourceTail);
        tempState.remapContext.release();
    } else {
        ok = buildPassthroughPipeline(cfg, pipeline, sourceTail);
    }

    if (!ok) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    return pipeline;
}

GstElement* StreamManager::createSourceChain(const StreamConfig& cfg, GstElement* pipeline, GstElement*& terminalElement) {
    terminalElement = nullptr;
    const std::string input = cfg.inputUri;
    const std::string inputLower = toLower(input);

    auto addQueue = [&](const char* name, guint64 maxSizeTime = 3000000000ULL) -> GstElement* {
        GstElement* queue = gst_element_factory_make("queue", name);
        if (!addElementOrFail(pipeline, queue)) {
            return nullptr;
        }
        g_object_set(queue,
            "max-size-buffers", 0,
            "max-size-bytes", 0,
            "max-size-time", maxSizeTime,
            nullptr);
        return queue;
    };

    if (inputLower == "test://bars" || inputLower == "testsrc://bars" || inputLower == "bars://hd") {
        return createTestPatternChain(cfg, pipeline, terminalElement);
    }

    if (inputLower.rfind("srt://", 0) == 0) {
        std::string mode = toLower(cfg.inputMode);
        const char* factory = (mode == "listener") ? "srtsrc" : "srtclientsrc";
        if (!hasElementFactory(factory)) {
            std::cerr << missingElementStatus(factory) << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make(factory, "input_src");
        GstElement* queue = addQueue("input_queue");
        if (!src || !queue || !addElementOrFail(pipeline, src)) {
            return nullptr;
        }

        g_object_set(src, "uri", input.c_str(), nullptr);
        if (std::string(factory) == "srtsrc") {
            g_object_set(src, "wait-for-connection", TRUE, nullptr);
        } else {
            g_object_set(src, "latency", 500, nullptr);
        }

        if (!gst_element_link(src, queue)) {
            return nullptr;
        }

        terminalElement = queue;
        return src;
    }

    if (inputLower.rfind("http://", 0) == 0 || inputLower.rfind("https://", 0) == 0) {
        if (!hasElementFactory("souphttpsrc")) {
            std::cerr << missingElementStatus("souphttpsrc") << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("souphttpsrc", "input_src");
        GstElement* queue = addQueue("input_queue");
        if (!src || !queue || !addElementOrFail(pipeline, src)) {
            return nullptr;
        }

        g_object_set(src, "location", input.c_str(), "is-live", TRUE, "do-timestamp", TRUE, nullptr);

        if (!gst_element_link(src, queue)) {
            return nullptr;
        }

        terminalElement = queue;
        return src;
    }

    if (inputLower.rfind("udp://", 0) == 0 || inputLower.rfind("rtp://", 0) == 0) {
        std::regex re("^(udp|rtp)://([^:/]+):(\\d+).*$", std::regex::icase);
        std::smatch match;
        if (!std::regex_match(input, match, re) || match.size() < 4) {
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("udpsrc", "input_src");
        GstElement* queue = addQueue("input_queue");
        if (!src || !queue || !addElementOrFail(pipeline, src)) {
            return nullptr;
        }

        int port = std::stoi(match[3].str());
        g_object_set(src, "port", port, nullptr);

        if (!cfg.interfaceAddress.empty()) {
            g_object_set(src, "multicast-iface", cfg.interfaceAddress.c_str(), nullptr);
        }

        if (toLower(match[1].str()) == "rtp") {
            GstElement* depay = gst_element_factory_make("rtpmp2tdepay", "rtp_depay");
            if (!depay || !addElementOrFail(pipeline, depay)) {
                return nullptr;
            }

            GstCaps* caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=MP2T,clock-rate=90000");
            g_object_set(src, "caps", caps, nullptr);
            gst_caps_unref(caps);

            if (!gst_element_link_many(src, depay, queue, nullptr)) {
                return nullptr;
            }
        } else {
            if (!gst_element_link(src, queue)) {
                return nullptr;
            }
        }

        terminalElement = queue;
        return src;
    }
        GstElement* src = gst_element_factory_make("filesrc", "input_src");
    GstElement* queue = addQueue("input_queue");
    if (!src || !queue || !addElementOrFail(pipeline, src)) {
        return nullptr;
    }

    g_object_set(src, "location", input.c_str(), nullptr);
    if (!gst_element_link(src, queue)) {
        return nullptr;
    }

    terminalElement = queue;
    return src;
}

GstElement* StreamManager::createTestPatternChain(const StreamConfig& cfg, GstElement* pipeline, GstElement*& terminalElement) {
    terminalElement = nullptr;
    const std::vector<const char*> required = {
        "videotestsrc", "capsfilter", "videoconvert", "x264enc", "h264parse", "mpegtsmux", "queue"
    };
    for (const char* element : required) {
        if (!hasElementFactory(element)) {
            std::cerr << missingElementStatus(element) << std::endl;
            return nullptr;
        }
    }

    GstElement* src = gst_element_factory_make("videotestsrc", "test_bars_src");
    GstElement* capsfilter = gst_element_factory_make("capsfilter", "test_bars_caps");
    GstElement* convert = gst_element_factory_make("videoconvert", "test_bars_convert");
    GstElement* encoder = gst_element_factory_make("x264enc", "test_bars_encoder");
    GstElement* parser = gst_element_factory_make("h264parse", "test_bars_h264parse");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "test_bars_mux");
    GstElement* queue = gst_element_factory_make("queue", "test_bars_queue");

    if (!src || !capsfilter || !convert || !encoder || !parser || !mux || !queue) {
        return nullptr;
    }

    if (!addElementOrFail(pipeline, src) ||
        !addElementOrFail(pipeline, capsfilter) ||
        !addElementOrFail(pipeline, convert) ||
        !addElementOrFail(pipeline, encoder) ||
        !addElementOrFail(pipeline, parser) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, queue)) {
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string("video/x-raw,width=1920,height=1080,framerate=25/1");
    g_object_set(capsfilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    guint bitrateKbps = static_cast<guint>(std::max<uint64_t>(cfg.targetBitrate / 1000, 1000));
    g_object_set(src,
        "is-live", TRUE,
        "pattern", 0,
        nullptr);
    g_object_set(encoder,
        "bitrate", bitrateKbps,
        "key-int-max", 50,
        "bframes", 0,
        nullptr);
    g_object_set(parser, "config-interval", 1, nullptr);
    g_object_set(mux, "alignment", 7, nullptr);
    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", 3000000000ULL,
        nullptr);

    if (!gst_element_link_many(src, capsfilter, convert, encoder, parser, mux, queue, nullptr)) {
        return nullptr;
    }

    terminalElement = queue;
    return src;
}

bool StreamManager::buildPassthroughPipeline(const StreamConfig& cfg, GstElement* pipeline, GstElement* sourceTail) {
    GstElement* tsparse = gst_element_factory_make("tsparse", "tsparse");
    GstElement* queue = gst_element_factory_make("queue", "output_queue");
    GstElement* sink = gst_element_factory_make("udpsink", "output_sink");

    if (!tsparse || !queue || !sink) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, queue) ||
        !addElementOrFail(pipeline, sink)) {
        return false;
    }

    configureUdpSink(sink, cfg);

    return gst_element_link_many(sourceTail, tsparse, queue, sink, nullptr);
}

bool StreamManager::buildRemapPipeline(StreamState* state, GstElement* pipeline, GstElement* sourceTail) {
    if (!state || !state->remapContext) {
        return false;
    }
    if (!hasElementFactory("tsparse") || !hasElementFactory("tsdemux") || !hasElementFactory("mpegtsmux")) {
        std::cerr << "missing remap elements: tsparse, tsdemux or mpegtsmux" << std::endl;
        return false;
    }

    GstElement* tsparse = gst_element_factory_make("tsparse", "remap_tsparse");
    GstElement* preDemuxQueue = gst_element_factory_make("queue", "remap_pre_demux_queue");
    GstElement* demux = gst_element_factory_make("tsdemux", "demux");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "mux");
    GstElement* sink = gst_element_factory_make("udpsink", "output_sink");
    if (!tsparse || !preDemuxQueue || !demux || !mux || !sink) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, preDemuxQueue) ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, sink)) {
        return false;
    }

    g_object_set(preDemuxQueue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", 3000000000ULL,
        nullptr);
    g_object_set(mux, "alignment", 7, nullptr);
    configureUdpSink(sink, state->config);

    if (!gst_element_link_many(sourceTail, tsparse, preDemuxQueue, demux, nullptr)) {
        return false;
    }
    if (!gst_element_link(mux, sink)) {
        return false;
    }

    state->remapContext->mux = mux;
    state->remapContext->sink = sink;
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), state->remapContext.get());
    return true;
}

void StreamManager::onDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data) {
    (void)demux;
    auto* ctx = static_cast<RemapContext*>(user_data);
    if (!ctx || !ctx->mux) {
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    std::string capsString = caps ? gst_caps_to_string(caps) : "unknown";
    bool isAudio = capsString.find("audio/") != std::string::npos;
    bool isVideo = capsString.find("video/") != std::string::npos;
    bool isPrivateTs = capsString.find("private") != std::string::npos || capsString.find("subpicture") != std::string::npos;
    if (caps) {
        gst_caps_unref(caps);
    }

    if ((!isAudio && !isVideo) || isPrivateTs) {
        return;
    }
    if ((isVideo && ctx->videoLinked) || (isAudio && ctx->audioLinked)) {
        return;
    }

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* parser = nullptr;
    if (capsString.find("video/x-h264") != std::string::npos) {
        parser = gst_element_factory_make("h264parse", nullptr);
    } else if (capsString.find("audio/mpeg") != std::string::npos && capsString.find("mpegversion=(int)4") != std::string::npos) {
        parser = gst_element_factory_make("aacparse", nullptr);
    }

    if (!queue || !parser) {
        if (queue) gst_object_unref(queue);
        if (parser) gst_object_unref(parser);
        return;
    }

    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(ctx->mux));
    if (!pipeline) {
        gst_object_unref(queue);
        return;
    }

    if (!gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), parser)) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (parser && !GST_OBJECT_PARENT(parser)) gst_object_unref(parser);
        gst_object_unref(pipeline);
        return;
    }

    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", 3000000000ULL,
        nullptr);
    if (G_OBJECT_TYPE_NAME(parser) && g_strcmp0(G_OBJECT_TYPE_NAME(parser), "GstH264Parse") == 0) {
        g_object_set(parser, "config-interval", 1, nullptr);
    }
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(parser);

    if (!gst_element_link(queue, parser)) {
        gst_object_unref(pipeline);
        return;
    }

    GstPad* queueSinkPad = gst_element_get_static_pad(queue, "sink");
    if (!queueSinkPad) {
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(pad, queueSinkPad) != GST_PAD_LINK_OK) {
        gst_object_unref(queueSinkPad);
        gst_object_unref(pipeline);
        return;
    }
    gst_object_unref(queueSinkPad);

    GstPad* parserSrcPad = gst_element_get_static_pad(parser, "src");
    GstPad* muxSinkPad = gst_element_get_request_pad(ctx->mux, "sink_%u");
    if (!parserSrcPad || !muxSinkPad) {
        if (parserSrcPad) gst_object_unref(parserSrcPad);
        if (muxSinkPad) gst_object_unref(muxSinkPad);
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(parserSrcPad, muxSinkPad) == GST_PAD_LINK_OK) {
        if (isVideo) {
            ctx->videoLinked = true;
        }
        if (isAudio) {
            ctx->audioLinked = true;
        }
    }

    gst_object_unref(parserSrcPad);
    gst_object_unref(muxSinkPad);
    gst_object_unref(pipeline);
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
        const gchar* factoryName =
            gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(gst_element_get_factory(element)));

        if (!inputAttached && factoryName &&
            (g_strcmp0(factoryName, "srtsrc") == 0 ||
             g_strcmp0(factoryName, "srtclientsrc") == 0 ||
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
    auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state->lastBitrateSample).count();
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
        if (!msg) {
            continue;
        }

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                std::string message = err ? err->message : "unknown";
                if (err) {
                    g_error_free(err);
                }
                g_free(dbg);

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
