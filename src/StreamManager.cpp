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
constexpr guint64 kCbrInitialLatencyNs = 100 * GST_MSECOND;
constexpr guint64 kCbrMaxLateNs = 25 * GST_MSECOND;
constexpr guint64 kCbrMaxAheadNs = 500 * GST_MSECOND;

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

void configureUdpSink(GstElement* sink, const StreamConfig& cfg) {
    const bool multicastOutput = isMulticastHost(cfg.outputHost);

    g_object_set(sink,
        "host", cfg.outputHost.c_str(),
        "port", cfg.outputPort,
        "async", FALSE,
        "sync", FALSE,
        "buffer-size", kUdpSocketBufferSize,
        nullptr);

    if (cfg.cbr && cfg.targetBitrate > 0) {
        setUInt64PropertyIfPresent(sink, "max-bitrate", static_cast<guint64>(cfg.targetBitrate));
        setBooleanPropertyIfPresent(sink, "qos", FALSE);
        setInt64PropertyIfPresent(sink, "max-lateness", -1);
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
    if (cfg.remapEnabled || cfg.cbr) {
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
            gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER, inputPadProbe, state, nullptr);
            gst_object_unref(sinkPad);
            inputAttached = TRUE;
        }
        gst_object_unref(inputQueue);
    }

    GstElement* outputSink = gst_bin_get_by_name(GST_BIN(state->pipeline), "output_sink");
    if (outputSink) {
        GstPad* sinkPad = gst_element_get_static_pad(outputSink, "sink");
        if (sinkPad) {
            if (state->config.cbr && state->config.targetBitrate > 0) {
                gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER, cbrPacingPadProbe, state, nullptr);
            }
            gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER, outputPadProbe, state, nullptr);
            gst_object_unref(sinkPad);
            outputAttached = TRUE;
        }
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
                gst_pad_add_probe(srcPad, GST_PAD_PROBE_TYPE_BUFFER, inputPadProbe, state, nullptr);
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
                if (state->config.cbr && state->config.targetBitrate > 0) {
                    gst_pad_add_probe(sinkPad, GST_PAD_PROBE_TYPE_BUFFER, cbrPacingPadProbe, state, nullptr);
                }
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
    if (state->config.cbr && state->config.targetBitrate > 0) {
        state->outputBitrate = state->config.targetBitrate;
    } else {
        state->outputBitrate = static_cast<uint64_t>((outputDelta * 8) / seconds);
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

GstPadProbeReturn StreamManager::cbrPacingPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    auto* state = static_cast<StreamState*>(user_data);
    if (!state || !(info->type & GST_PAD_PROBE_TYPE_BUFFER) ||
        !state->config.cbr || state->config.targetBitrate == 0) {
        return GST_PAD_PROBE_OK;
    }

    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
    if (!buffer) {
        return GST_PAD_PROBE_OK;
    }

    const gsize size = gst_buffer_get_size(buffer);
    if (size == 0) {
        return GST_PAD_PROBE_OK;
    }

    const guint64 targetBitrate = state->config.targetBitrate;
    const guint64 bits = static_cast<guint64>(size) * 8ULL;
    guint64 ptsNs = 0;
    guint64 durationNs = 1;
    bool canWait = false;
    GstClockTime targetClockTime = GST_CLOCK_TIME_NONE;
    GstClockTime nowClockTime = GST_CLOCK_TIME_NONE;
    GstClock* clock = nullptr;
    GstElement* element = GST_ELEMENT(gst_pad_get_parent(pad));
    GstClockTime baseTime = GST_CLOCK_TIME_NONE;

    if (element) {
        clock = gst_element_get_clock(element);
        baseTime = gst_element_get_base_time(element);
        if (clock && GST_CLOCK_TIME_IS_VALID(baseTime)) {
            nowClockTime = gst_clock_get_time(clock);
            canWait = GST_CLOCK_TIME_IS_VALID(nowClockTime) && nowClockTime >= baseTime;
        }
    }

    {
        std::lock_guard<std::mutex> lock(state->cbrMutex);

        const __uint128_t scaledDuration =
            static_cast<__uint128_t>(bits) * GST_SECOND + state->cbrDurationRemainder;
        durationNs = static_cast<guint64>(scaledDuration / targetBitrate);
        state->cbrDurationRemainder = static_cast<uint64_t>(scaledDuration % targetBitrate);
        durationNs = std::max<guint64>(durationNs, 1);

        if (canWait) {
            const guint64 nowRunningNs = static_cast<guint64>(nowClockTime - baseTime);
            const bool scheduleIsLate =
                state->cbrPacingStarted && state->cbrNextPtsNs + kCbrMaxLateNs < nowRunningNs;
            const bool scheduleIsTooFarAhead =
                state->cbrPacingStarted && state->cbrNextPtsNs > nowRunningNs + kCbrMaxAheadNs;

            if (!state->cbrPacingStarted || scheduleIsLate || scheduleIsTooFarAhead) {
                state->cbrNextPtsNs = nowRunningNs + kCbrInitialLatencyNs;
                state->cbrDurationRemainder = 0;
            }
        } else if (!state->cbrPacingStarted) {
            state->cbrNextPtsNs = 0;
            state->cbrDurationRemainder = 0;
        }

        if (!state->cbrPacingStarted) {
            state->cbrPacingStarted = true;
        }

        ptsNs = state->cbrNextPtsNs;
        state->cbrNextPtsNs += durationNs;

        if (canWait) {
            targetClockTime = baseTime + ptsNs;
        }
    }

    if (canWait && GST_CLOCK_TIME_IS_VALID(targetClockTime) && targetClockTime > nowClockTime) {
        GstClockID clockId = gst_clock_new_single_shot_id(clock, targetClockTime);
        if (clockId) {
            gst_clock_id_wait(clockId, nullptr);
            gst_clock_id_unref(clockId);
        }
    }

    if (clock) {
        gst_object_unref(clock);
    }
    if (element) {
        gst_object_unref(element);
    }

    buffer = gst_buffer_make_writable(buffer);
    GST_BUFFER_PTS(buffer) = ptsNs;
    GST_BUFFER_DTS(buffer) = ptsNs;
    GST_BUFFER_DURATION(buffer) = durationNs;
    GST_PAD_PROBE_INFO_DATA(info) = buffer;

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
