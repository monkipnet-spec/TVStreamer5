#include "StreamManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#include <glib.h>
#include <unistd.h>
#define GST_USE_UNSTABLE_API
#include <gst/mpegts/mpegts.h>

namespace {

constexpr gint kUdpSocketBufferSize = 16 * 1024 * 1024;
constexpr guint kTsSmoothingLatencyUsec = 700000;
constexpr auto kInputFailoverDelay = std::chrono::seconds(5);
constexpr auto kPrimaryRetryInterval = std::chrono::seconds(10);

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

bool isHlsUri(const std::string& inputLower, const std::string& inputMode) {
    return inputLower.rfind("hls://", 0) == 0 ||
           toLower(inputMode) == "hls" ||
           inputLower.find(".m3u8") != std::string::npos;
}

bool addElementOrFail(GstElement* pipeline, GstElement* element) {
    return element != nullptr && gst_bin_add(GST_BIN(pipeline), element);
}

bool hasProperty(GstElement* element, const char* propertyName) {
    return element && g_object_class_find_property(G_OBJECT_GET_CLASS(element), propertyName) != nullptr;
}

void setBooleanPropertyIfPresent(GstElement* element, const char* propertyName, gboolean value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setIntPropertyIfPresent(GstElement* element, const char* propertyName, gint value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setUInt64PropertyIfPresent(GstElement* element, const char* propertyName, guint64 value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setInt64PropertyIfPresent(GstElement* element, const char* propertyName, gint64 value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setStringPropertyIfPresent(GstElement* element, const char* propertyName, const std::string& value) {
    if (hasProperty(element, propertyName) && !value.empty()) {
        g_object_set(element, propertyName, value.c_str(), nullptr);
    }
}

bool isMulticastHost(const std::string& host) {
    std::regex re(R"(^((22[4-9])|(23[0-9]))\.)");
    return std::regex_search(host, re);
}

std::string interfaceNameForAddress(const std::string& address) {
    if (address.empty()) {
        return "";
    }

    for (const auto& iface : enumerateNetworkInterfaces()) {
        if (iface.address == address) {
            return iface.name;
        }
    }

    return "";
}

std::string interfaceNameOrAddress(const std::string& address) {
    const std::string ifaceName = interfaceNameForAddress(address);
    return ifaceName.empty() ? address : ifaceName;
}

std::string outputType(const StreamConfig& cfg) {
    std::string type = toLower(cfg.outputType);
    if (type != "srt" && type != "http" && type != "hls") {
        type = "udp";
    }
    return type;
}

std::string hlsDirectory(const StreamConfig& cfg) {
    return "/tmp/tvstreamer5-hls/" + cfg.id;
}

std::string telegramEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

std::string displayName(const StreamConfig& cfg) {
    return cfg.name.empty() ? cfg.id : cfg.name;
}

uint64_t bufferListSize(GstBufferList* list) {
    if (!list) {
        return 0;
    }

    uint64_t total = 0;
    const guint length = gst_buffer_list_length(list);
    for (guint i = 0; i < length; ++i) {
        GstBuffer* buffer = gst_buffer_list_get(list, i);
        if (buffer) {
            total += gst_buffer_get_size(buffer);
        }
    }
    return total;
}

void configureUdpSink(GstElement* sink, const StreamConfig& cfg) {
    const bool multicastOutput = isMulticastHost(cfg.outputHost);

    g_object_set(sink,
        "host", cfg.outputHost.c_str(),
        "port", cfg.outputPort,
        "async", FALSE,
        "sync", cfg.cbr ? TRUE : FALSE,
        "buffer-size", kUdpSocketBufferSize,
        nullptr);

    if (cfg.cbr && cfg.targetBitrate > 0) {
        setBooleanPropertyIfPresent(sink, "qos", FALSE);
        setInt64PropertyIfPresent(sink, "max-lateness", -1);
        setUInt64PropertyIfPresent(sink, "max-bitrate", static_cast<guint64>(cfg.targetBitrate));
    }

    if (!cfg.interfaceAddress.empty() && !multicastOutput) {
        setStringPropertyIfPresent(sink, "bind-address", cfg.interfaceAddress);
    }

    if (multicastOutput) {
        g_object_set(sink, "auto-multicast", TRUE, nullptr);
        setStringPropertyIfPresent(sink, "multicast-iface", interfaceNameOrAddress(cfg.interfaceAddress));
    }
}

void configureSrtSink(GstElement* sink, const StreamConfig& cfg) {
    const std::string host = cfg.outputHost.empty() ? "0.0.0.0" : cfg.outputHost;
    const bool listener = host == "0.0.0.0" || host == "::";
    const std::string uri = "srt://" + host + ":" + std::to_string(cfg.outputPort);
    g_object_set(sink, "uri", uri.c_str(), "sync", FALSE, "async", FALSE, nullptr);
    setIntPropertyIfPresent(sink, "mode", listener ? 2 : 1);
    setBooleanPropertyIfPresent(sink, "wait-for-connection", listener ? TRUE : FALSE);
    setStringPropertyIfPresent(sink, "localaddress", cfg.interfaceAddress);
}

void configureHttpSink(GstElement* sink, const StreamConfig& cfg) {
    (void)cfg;
    g_object_set(sink,
        "sync", FALSE,
        "async", FALSE,
        nullptr);
}

void configureHlsSink(GstElement* sink, const StreamConfig& cfg) {
    std::filesystem::create_directories(hlsDirectory(cfg));
    const std::string playlist = hlsDirectory(cfg) + "/playlist.m3u8";
    const std::string location = hlsDirectory(cfg) + "/segment%05d.ts";
    g_object_set(sink,
        "playlist-location", playlist.c_str(),
        "location", location.c_str(),
        "target-duration", 4,
        "max-files", 8,
        nullptr);
}

void configureQueue(GstElement* queue, guint64 maxSizeTime = 3000000000ULL) {
    if (!queue) {
        return;
    }

    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", maxSizeTime,
        nullptr);
}

void configureTsParse(GstElement* tsparse, bool smoothTimestamps) {
    if (!tsparse || !smoothTimestamps) {
        return;
    }

    setBooleanPropertyIfPresent(tsparse, "set-timestamps", TRUE);
    setIntPropertyIfPresent(tsparse, "smoothing-latency", static_cast<gint>(kTsSmoothingLatencyUsec));
}

void linkDemuxPadToQueue(GstElement* demux, GstPad* pad, gpointer userData) {
    (void)demux;
    auto* queue = static_cast<GstElement*>(userData);
    if (!queue) {
        return;
    }

    GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
    if (!sinkPad) {
        return;
    }

    if (!gst_pad_is_linked(sinkPad)) {
        GstPadLinkReturn ret = gst_pad_link(pad, sinkPad);
        if (ret != GST_PAD_LINK_OK) {
            std::cerr << "HLS demux pad link failed: " << gst_pad_link_get_name(ret) << std::endl;
        }
    }

    gst_object_unref(sinkPad);
}

void configureTsMux(GstElement* mux, const StreamConfig& cfg) {
    g_object_set(mux,
        "alignment", 7,
        "pcr-interval", 1800U,
        "pat-interval", 9000U,
        "pmt-interval", 9000U,
        "si-interval", 9000U,
        nullptr);
    if (cfg.cbr && cfg.targetBitrate > 0) {
        setUInt64PropertyIfPresent(mux, "bitrate", static_cast<guint64>(cfg.targetBitrate));
    }
}

void sendServiceDescription(GstElement* mux, const StreamConfig& cfg) {
    if (!mux || (cfg.serviceName.empty() && cfg.serviceProvider.empty())) {
        return;
    }

    GstMpegtsSDT* sdt = gst_mpegts_sdt_new();
    GstMpegtsSDTService* service = gst_mpegts_sdt_service_new();
    if (!sdt || !service) {
        return;
    }

    sdt->actual_ts = TRUE;
    sdt->transport_stream_id = 1;
    sdt->original_network_id = 1;

    service->service_id = static_cast<guint16>(cfg.serviceId ? cfg.serviceId : 1);
    service->EIT_schedule_flag = FALSE;
    service->EIT_present_following_flag = FALSE;
    service->running_status = GST_MPEGTS_RUNNING_STATUS_RUNNING;
    service->free_CA_mode = FALSE;

    GstMpegtsDescriptor* descriptor = gst_mpegts_descriptor_from_dvb_service(
        GST_DVB_SERVICE_DIGITAL_TELEVISION,
        cfg.serviceName.empty() ? cfg.name.c_str() : cfg.serviceName.c_str(),
        cfg.serviceProvider.empty() ? "TVStreamer5" : cfg.serviceProvider.c_str());
    if (descriptor) {
        g_ptr_array_add(service->descriptors, descriptor);
    }
    g_ptr_array_add(sdt->services, service);

    GstMpegtsSection* section = gst_mpegts_section_from_sdt(sdt);
    if (section) {
        gst_mpegts_section_send_event(section, mux);
        gst_mpegts_section_unref(section);
    }
}

GstPad* requestMuxSinkPad(GstElement* mux, uint32_t requestedPid) {
    GstPad* pad = nullptr;
    if (requestedPid > 0) {
        std::string requestedName = "sink_" + std::to_string(requestedPid);
        pad = gst_element_request_pad_simple(mux, requestedName.c_str());
    }
    if (!pad) {
        pad = gst_element_request_pad_simple(mux, "sink_%d");
    }
    return pad;
}

void updateMuxProgramMap(RemapContext* ctx) {
    if (!ctx || !ctx->mux) {
        return;
    }

    GstStructure* programMap = gst_structure_new_empty("program_map");
    int serviceId = static_cast<int>(ctx->config.serviceId ? ctx->config.serviceId : 1);
    if (!ctx->videoPadName.empty()) {
        gst_structure_set(programMap, ctx->videoPadName.c_str(), G_TYPE_INT, serviceId, nullptr);
    }
    if (!ctx->audioPadName.empty()) {
        gst_structure_set(programMap, ctx->audioPadName.c_str(), G_TYPE_INT, serviceId, nullptr);
    }
    g_object_set(ctx->mux, "prog-map", programMap, nullptr);
    gst_structure_free(programMap);
}

std::string parserForCaps(const std::string& capsString) {
    if (capsString.find("video/x-h264") != std::string::npos) {
        return "h264parse";
    }
    if (capsString.find("video/x-h265") != std::string::npos) {
        return "h265parse";
    }
    if (capsString.find("video/mpeg") != std::string::npos) {
        return "mpegvideoparse";
    }
    if (capsString.find("audio/mpeg") != std::string::npos &&
        capsString.find("mpegversion=(int)4") != std::string::npos) {
        return "aacparse";
    }
    if (capsString.find("audio/mpeg") != std::string::npos &&
        capsString.find("mpegversion=(int)1") != std::string::npos) {
        return "mpegaudioparse";
    }
    if (capsString.find("audio/x-ac3") != std::string::npos) {
        return "ac3parse";
    }
    if (capsString.find("audio/x-dts") != std::string::npos) {
        return "dtsparse";
    }
    return "";
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
    state->primaryInputUri = streamConfig.inputUri;
    state->activeInputUri = streamConfig.inputUri;
    if (streamConfig.remapEnabled) {
        state->remapContext = std::make_unique<RemapContext>();
        state->remapContext->config = streamConfig;
    }

    GstElement* pipeline = createPipeline(state.get());
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
    state->outputBitrate = streamConfig.cbr ? streamConfig.targetBitrate : 0;
    state->lastInputActivity = std::chrono::steady_clock::now();
    state->lastPrimaryRetry = state->lastInputActivity;
    state->lastBitrateSample = state->lastInputActivity;
    attachBitrateProbes(state.get());

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
    streams[streamConfig.id]->busThread = std::thread(&StreamManager::monitorBus, this, streamConfig.id);
    notifyStreamState(streamConfig, "🟢", "Поток запущен", "Источник: основной\nURL: " + streamConfig.inputUri);
    return true;
}

bool StreamManager::stopStream(const std::string& id) {
    std::lock_guard<std::mutex> lock(managerMutex);
    if (!streams.count(id)) {
        return false;
    }

    auto& state = *streams[id];
    const StreamConfig stoppedConfig = state.config;
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
    notifyStreamState(stoppedConfig, "⚪", "Поток остановлен", "Остановлен вручную");
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
         << " output_type=" << outputType(cfg)
         << " output=" << cfg.outputHost << ":" << cfg.outputPort
         << " iface=" << cfg.interfaceAddress
         << " service_id=" << cfg.serviceId
         << " vpid=" << cfg.videoPid
         << " apid=" << cfg.audioPid;
    return desc.str();
}

bool StreamManager::addHttpClient(const std::string& id, int fd) {
    std::lock_guard<std::mutex> lock(managerMutex);
    auto found = streams.find(id);
    if (found == streams.end() || !found->second->pipeline) {
        close(fd);
        return false;
    }

    GstElement* sink = gst_bin_get_by_name(GST_BIN(found->second->pipeline), "output_sink");
    if (!sink) {
        close(fd);
        return false;
    }

    GstElementFactory* factory = gst_element_get_factory(sink);
    const gchar* factoryName = factory
        ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
        : nullptr;
    if (!factoryName || g_strcmp0(factoryName, "multifdsink") != 0) {
        gst_object_unref(sink);
        close(fd);
        return false;
    }

    g_signal_emit_by_name(sink, "add", fd);
    gst_object_unref(sink);
    return true;
}

void StreamManager::notifyStreamState(
    const StreamConfig& cfg,
    const std::string& color,
    const std::string& title,
    const std::string& details) {
    const std::string serverName = configManager.config.serverName.empty()
        ? "TVStreamer5"
        : configManager.config.serverName;
    std::ostringstream message;
    message << color << " <b>" << telegramEscape(title) << "</b>\n"
            << "Сервер: <b>" << telegramEscape(serverName) << "</b>\n"
            << "Канал: <b>" << telegramEscape(displayName(cfg)) << "</b>\n"
            << "ID: <code>" << telegramEscape(cfg.id) << "</code>";
    if (!details.empty()) {
        message << "\n" << telegramEscape(details);
    }
    telegramNotifier.sendMessage(message.str());
}

bool StreamManager::restartPipelineWithInput(StreamState* state, const std::string& inputUri, bool useBackup) {
    if (!state || inputUri.empty()) {
        return false;
    }

    GstElement* oldPipeline = state->pipeline;
    GstBus* oldBus = state->bus;

    if (oldPipeline) {
        gst_element_set_state(oldPipeline, GST_STATE_NULL);
    }

    state->config.inputUri = inputUri;
    state->remapContext.reset();
    if (state->config.remapEnabled) {
        state->remapContext = std::make_unique<RemapContext>();
        state->remapContext->config = state->config;
    }

    GstElement* newPipeline = createPipeline(state);
    if (!newPipeline) {
        state->pipeline = oldPipeline;
        state->bus = oldBus;
        state->statusMessage = "error: restart failed";
        if (oldPipeline) {
            gst_element_set_state(oldPipeline, GST_STATE_PLAYING);
        }
        return false;
    }

    GstBus* newBus = gst_element_get_bus(newPipeline);
    state->pipeline = newPipeline;
    state->bus = newBus;
    state->usingBackup = useBackup;
    state->backupAttempted = useBackup;
    state->primaryRetryPending = !useBackup;
    state->inputLossNotified = false;
    state->activeInputUri = inputUri;
    state->active = true;
    state->statusMessage = useBackup ? "running on backup" : "running on primary";
    state->inputBytes = 0;
    state->outputBytes = 0;
    state->inputBitrate = 0;
    state->outputBitrate = state->config.cbr ? state->config.targetBitrate : 0;
    state->lastInputBytesSample = 0;
    state->lastOutputBytesSample = 0;
    state->lastInputBytesSeen = 0;
    state->lastInputActivity = std::chrono::steady_clock::now();
    state->lastPrimaryRetry = state->lastInputActivity;
    state->lastBitrateSample = state->lastInputActivity;
    attachBitrateProbes(state);

    GstStateChangeReturn ret = gst_element_set_state(newPipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        state->statusMessage = "error: restart playback failed";
        state->active = false;
    }

    if (oldBus) {
        gst_object_unref(oldBus);
    }
    if (oldPipeline) {
        gst_object_unref(oldPipeline);
    }
    return ret != GST_STATE_CHANGE_FAILURE;
}

GstElement* StreamManager::createPipeline(StreamState* state) {
    if (!state) {
        return nullptr;
    }
    const StreamConfig& cfg = state->config;
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
        if (!state->remapContext) {
            state->remapContext = std::make_unique<RemapContext>();
        }
        state->remapContext->config = cfg;
        ok = buildRemapPipeline(state, pipeline, sourceTail);
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
        configureQueue(queue, maxSizeTime);
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
        setBooleanPropertyIfPresent(src, "do-timestamp", TRUE);
        setBooleanPropertyIfPresent(src, "auto-reconnect", TRUE);
        setIntPropertyIfPresent(src, "latency", 500);
        setStringPropertyIfPresent(src, "localaddress", cfg.interfaceAddress);
        if (mode == "listener") {
            setIntPropertyIfPresent(src, "mode", 2);
            setBooleanPropertyIfPresent(src, "wait-for-connection", TRUE);
            setBooleanPropertyIfPresent(src, "keep-listening", TRUE);
        } else {
            setIntPropertyIfPresent(src, "mode", 1);
            setBooleanPropertyIfPresent(src, "wait-for-connection", FALSE);
        }

        if (!gst_element_link(src, queue)) {
            return nullptr;
        }

        terminalElement = queue;
        return src;
    }

    if (isHlsUri(inputLower, cfg.inputMode)) {
        if (!hasElementFactory("souphttpsrc")) {
            std::cerr << missingElementStatus("souphttpsrc") << std::endl;
            return nullptr;
        }
        if (!hasElementFactory("hlsdemux")) {
            std::cerr << missingElementStatus("hlsdemux") << std::endl;
            return nullptr;
        }

        std::string location = input;
        if (inputLower.rfind("hls://", 0) == 0) {
            location = "http://" + input.substr(6);
        }

        GstElement* src = gst_element_factory_make("souphttpsrc", "input_src");
        GstElement* demux = gst_element_factory_make("hlsdemux", "hls_demux");
        GstElement* queue = addQueue("input_queue", 5000000000ULL);
        if (!src || !demux || !queue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, demux)) {
            return nullptr;
        }

        g_object_set(src, "location", location.c_str(), "is-live", TRUE, "do-timestamp", TRUE, nullptr);
        setIntPropertyIfPresent(src, "timeout", 15);
        setIntPropertyIfPresent(demux, "connection-speed", static_cast<gint>(std::max<uint64_t>(cfg.targetBitrate / 1000, 1)));

        if (!gst_element_link(src, demux)) {
            return nullptr;
        }

        g_signal_connect(demux, "pad-added", G_CALLBACK(linkDemuxPadToQueue), queue);
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
        setIntPropertyIfPresent(src, "timeout", 15);

        if (!gst_element_link(src, queue)) {
            return nullptr;
        }

        terminalElement = queue;
        return src;
    }

    if (inputLower.rfind("udp://", 0) == 0 || inputLower.rfind("rtp://", 0) == 0) {
        std::regex re(R"(^(udp|rtp)://@?([^:/]*):(\d+).*$)", std::regex::icase);
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
        std::string host = match[2].str();
        if (host.empty() || host == "@") {
            host = "0.0.0.0";
        }

        g_object_set(src,
            "address", host.c_str(),
            "port", port,
            "reuse", TRUE,
            "do-timestamp", TRUE,
            "buffer-size", kUdpSocketBufferSize,
            nullptr);

        if (isMulticastHost(host) && !cfg.interfaceAddress.empty()) {
            g_object_set(src, "multicast-iface", interfaceNameOrAddress(cfg.interfaceAddress).c_str(), nullptr);
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
            GstCaps* caps = gst_caps_from_string("video/mpegts,systemstream=(boolean)true,packetsize=(int)188");
            g_object_set(src, "caps", caps, nullptr);
            gst_caps_unref(caps);

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
    configureTsMux(mux, cfg);
    configureQueue(queue);

    if (!gst_element_link_many(src, capsfilter, convert, encoder, parser, mux, queue, nullptr)) {
        return nullptr;
    }

    terminalElement = queue;
    return src;
}

bool StreamManager::buildPassthroughPipeline(const StreamConfig& cfg, GstElement* pipeline, GstElement* sourceTail) {
    GstElement* tsparse = gst_element_factory_make("tsparse", "tsparse");
    GstElement* queue = gst_element_factory_make("queue", "output_queue");
    GstElement* sink = createOutputSink(cfg, pipeline);

    if (!tsparse || !queue || !sink) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, queue)) {
        return false;
    }

    configureTsParse(tsparse, cfg.cbr);
    configureQueue(queue);

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
    GstElement* outputQueue = gst_element_factory_make("queue", "output_queue");
    GstElement* sink = createOutputSink(state->config, pipeline);
    if (!tsparse || !preDemuxQueue || !demux || !mux || !outputQueue || !sink) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, preDemuxQueue) ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, outputQueue)) {
        return false;
    }

    configureQueue(preDemuxQueue);
    configureQueue(outputQueue);
    configureTsParse(tsparse, state->config.cbr);
    configureTsMux(mux, state->config);
    sendServiceDescription(mux, state->config);

    if (!gst_element_link_many(sourceTail, tsparse, preDemuxQueue, demux, nullptr)) {
        return false;
    }
    if (!gst_element_link_many(mux, outputQueue, sink, nullptr)) {
        return false;
    }

    state->remapContext->mux = mux;
    state->remapContext->sink = sink;
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), state->remapContext.get());
    return true;
}

GstElement* StreamManager::createOutputSink(const StreamConfig& cfg, GstElement* pipeline) {
    const std::string type = outputType(cfg);
    const char* factory = "udpsink";
    if (type == "srt") {
        factory = "srtsink";
    } else if (type == "http") {
        factory = "multifdsink";
    } else if (type == "hls") {
        factory = "hlssink";
    }

    if (!hasElementFactory(factory)) {
        std::cerr << missingElementStatus(factory) << std::endl;
        return nullptr;
    }

    GstElement* sink = gst_element_factory_make(factory, "output_sink");
    if (!sink || !addElementOrFail(pipeline, sink)) {
        return nullptr;
    }

    if (type == "srt") {
        configureSrtSink(sink, cfg);
    } else if (type == "http") {
        configureHttpSink(sink, cfg);
    } else if (type == "hls") {
        configureHlsSink(sink, cfg);
    } else {
        configureUdpSink(sink, cfg);
    }

    return sink;
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

    std::string parserFactory = parserForCaps(capsString);
    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* parser = parserFactory.empty() ? nullptr : gst_element_factory_make(parserFactory.c_str(), nullptr);

    if (!queue || !parser) {
        std::cerr << "remap skipped unsupported elementary stream caps: " << capsString << std::endl;
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

    configureQueue(queue);
    if (parserFactory == "h264parse" || parserFactory == "h265parse") {
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
    GstPad* muxSinkPad = requestMuxSinkPad(ctx->mux, isVideo ? ctx->config.videoPid : ctx->config.audioPid);
    if (!parserSrcPad || !muxSinkPad) {
        if (parserSrcPad) gst_object_unref(parserSrcPad);
        if (muxSinkPad) gst_object_unref(muxSinkPad);
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(parserSrcPad, muxSinkPad) == GST_PAD_LINK_OK) {
        const gchar* padName = GST_PAD_NAME(muxSinkPad);
        if (isVideo) {
            ctx->videoLinked = true;
            ctx->videoPadName = padName ? padName : "";
        }
        if (isAudio) {
            ctx->audioLinked = true;
            ctx->audioPadName = padName ? padName : "";
        }
        updateMuxProgramMap(ctx);
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

    GstElement* inputQueue = gst_bin_get_by_name(GST_BIN(state->pipeline), "input_queue");
    if (inputQueue) {
        GstPad* sinkPad = gst_element_get_static_pad(inputQueue, "sink");
        if (sinkPad) {
            gst_pad_add_probe(
                sinkPad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                inputPadProbe,
                state,
                nullptr);
            gst_object_unref(sinkPad);
            inputAttached = TRUE;
        }
        gst_object_unref(inputQueue);
    }

    GstElement* outputQueue = gst_bin_get_by_name(GST_BIN(state->pipeline), "output_queue");
    if (outputQueue) {
        GstPad* srcPad = gst_element_get_static_pad(outputQueue, "src");
        if (srcPad) {
            gst_pad_add_probe(
                srcPad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                outputPadProbe,
                state,
                nullptr);
            gst_object_unref(srcPad);
            outputAttached = TRUE;
        }
        gst_object_unref(outputQueue);
    }

    GstElement* outputSink = gst_bin_get_by_name(GST_BIN(state->pipeline), "output_sink");
    if (!outputAttached && outputSink) {
        GstPad* sinkPad = gst_element_get_static_pad(outputSink, "sink");
        if (sinkPad) {
            gst_pad_add_probe(
                sinkPad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                outputPadProbe,
                state,
                nullptr);
            gst_object_unref(sinkPad);
            outputAttached = TRUE;
        }
    }
    if (outputSink) {
        gst_object_unref(outputSink);
    }

    while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
        GstElement* element = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory* factory = gst_element_get_factory(element);
        const gchar* factoryName = factory
            ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
            : nullptr;

        if (!inputAttached && factoryName &&
            (g_strcmp0(factoryName, "srtsrc") == 0 ||
             g_strcmp0(factoryName, "srtclientsrc") == 0 ||
             g_strcmp0(factoryName, "souphttpsrc") == 0 ||
             g_strcmp0(factoryName, "udpsrc") == 0 ||
             g_strcmp0(factoryName, "filesrc") == 0)) {
            GstPad* srcPad = gst_element_get_static_pad(element, "src");
            if (srcPad) {
                gst_pad_add_probe(
                    srcPad,
                    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                    inputPadProbe,
                    state,
                    nullptr);
                gst_object_unref(srcPad);
                inputAttached = TRUE;
            }
        }

        if (!outputAttached && factoryName &&
            (g_strcmp0(factoryName, "udpsink") == 0 ||
             g_strcmp0(factoryName, "srtsink") == 0 ||
             g_strcmp0(factoryName, "multifdsink") == 0 ||
             g_strcmp0(factoryName, "hlssink") == 0)) {
            GstPad* sinkPad = gst_element_get_static_pad(element, "sink");
            if (sinkPad) {
                gst_pad_add_probe(
                    sinkPad,
                    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                    outputPadProbe,
                    state,
                    nullptr);
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

    state->lastInputBytesSample = currentInputBytes;
    state->lastOutputBytesSample = currentOutputBytes;
    state->lastBitrateSample = now;
}
GstPadProbeReturn StreamManager::inputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    (void)pad;
    auto* state = static_cast<StreamState*>(user_data);
    if (!state) {
        return GST_PAD_PROBE_OK;
    }

    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
        if (buffer) {
            state->inputBytes.fetch_add(gst_buffer_get_size(buffer), std::memory_order_relaxed);
        }
    } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        state->inputBytes.fetch_add(
            bufferListSize(gst_pad_probe_info_get_buffer_list(info)),
            std::memory_order_relaxed);
    }

    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn StreamManager::outputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    (void)pad;
    auto* state = static_cast<StreamState*>(user_data);
    if (!state) {
        return GST_PAD_PROBE_OK;
    }

    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
        if (buffer) {
            state->outputBytes.fetch_add(gst_buffer_get_size(buffer), std::memory_order_relaxed);
        }
    } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        state->outputBytes.fetch_add(
            bufferListSize(gst_pad_probe_info_get_buffer_list(info)),
            std::memory_order_relaxed);
    }

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
        const auto now = std::chrono::steady_clock::now();
        const uint64_t currentInputBytes = state->inputBytes.load();
        if (currentInputBytes != state->lastInputBytesSeen) {
            state->lastInputBytesSeen = currentInputBytes;
            state->lastInputActivity = now;
            if (state->inputLossNotified && !state->usingBackup && !state->primaryRetryPending) {
                state->statusMessage = "running";
                notifyStreamState(
                    state->config,
                    "🟢",
                    "Входной сигнал восстановлен",
                    "Активный источник: основной\nURL: " + state->activeInputUri);
            }
            state->inputLossNotified = false;
            if (state->primaryRetryPending && !state->usingBackup) {
                state->primaryRetryPending = false;
                state->backupAttempted = false;
                state->statusMessage = "running on primary";
                notifyStreamState(
                    state->config,
                    "🟢",
                    "Основной поток восстановлен",
                    "Активный источник: основной\nURL: " + state->activeInputUri);
            }
        }

        const bool inputTimedOut = now - state->lastInputActivity >= kInputFailoverDelay;
        if (inputTimedOut && !state->usingBackup && !state->config.backupInputUri.empty()) {
                notifyStreamState(
                    state->config,
                    "🟡",
                    "Основной поток пропал",
                    "Нет входных данных 5 секунд\nПереключаюсь на резерв\nBackup: " + state->config.backupInputUri);
            if (restartPipelineWithInput(state, state->config.backupInputUri, true)) {
                bus = state->bus;
                state->inputLossNotified = false;
                notifyStreamState(
                    state->config,
                    "🟠",
                    "Работаю с резервного источника",
                    "Активный источник: резерв\nURL: " + state->activeInputUri);
            } else {
                state->inputLossNotified = true;
                notifyStreamState(
                    state->config,
                    "🔴",
                    "Не удалось включить резерв",
                    "Backup pipeline не стартовал\nBackup: " + state->config.backupInputUri);
            }
        } else if (state->usingBackup && now - state->lastPrimaryRetry >= kPrimaryRetryInterval) {
            const std::string primaryUri = state->primaryInputUri;

            if (!primaryUri.empty()) {
                notifyStreamState(
                    state->config,
                    "🔵",
                    "Проверяю основной источник",
                    "Временно возвращаюсь на основной URL\nURL: " + primaryUri);
                if (restartPipelineWithInput(state, primaryUri, false)) {
                    bus = state->bus;
                    state->inputLossNotified = false;
                }
            }
        } else if (inputTimedOut && !state->usingBackup && state->primaryRetryPending && !state->config.backupInputUri.empty()) {
            notifyStreamState(
                state->config,
                "🟡",
                "Основной пока недоступен",
                "Возвращаюсь на резервный источник\nBackup: " + state->config.backupInputUri);
            if (restartPipelineWithInput(state, state->config.backupInputUri, true)) {
                bus = state->bus;
                state->inputLossNotified = false;
            }
        } else if (inputTimedOut && !state->usingBackup && state->config.backupInputUri.empty() && !state->inputLossNotified) {
            state->inputLossNotified = true;
            state->statusMessage = "no input signal";
            notifyStreamState(
                state->config,
                "🔴",
                "Нет входного сигнала",
                "Входных данных нет 5 секунд\nРезервная ссылка не задана\nURL: " + state->activeInputUri);
        }

        GstMessage* msg = gst_bus_timed_pop(bus, 500000000LL);
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
                notifyStreamState(state->config, "🔴", "Ошибка потока", "Причина: " + message);
                gst_message_unref(msg);
                return;
            }
            case GST_MESSAGE_EOS:
                state->statusMessage = "ended";
                state->active = false;
                notifyStreamState(state->config, "⚫", "Поток завершился", "GStreamer получил EOS");
                gst_message_unref(msg);
                return;
            default:
                gst_message_unref(msg);
                break;
        }
    }
}
