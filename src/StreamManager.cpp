#include "StreamManager.h"
#include "UdpInput.h"
#include "UdpOutput.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include <glib.h>
#include <unistd.h>
#define GST_USE_UNSTABLE_API
#include <gst/mpegts/mpegts.h>

namespace {

constexpr guint kTsPacketsPerUdpBuffer = 7;
constexpr guint64 kTsSmoothingLatency = 300 * GST_MSECOND;
constexpr guint64 kUdpQueueLatency = 10 * GST_SECOND;
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

void setUIntPropertyIfPresent(GstElement* element, const char* propertyName, guint value) {
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

std::string outputType(const StreamConfig& cfg) {
    std::string type = toLower(cfg.outputType);
    if (type != "srt" && type != "http" && type != "hls" && type != "rtmp" && type != "youtube") {
        type = "udp";
    }
    return type;
}

std::string srtOutputMode(const StreamConfig& cfg) {
    const std::string mode = toLower(cfg.outputMode);
    return mode == "caller" ? "caller" : "listener";
}

bool isRtmpUri(const std::string& uriLower) {
    return uriLower.rfind("rtmp://", 0) == 0 ||
           uriLower.rfind("rtmps://", 0) == 0 ||
           uriLower.rfind("rtmpt://", 0) == 0 ||
           uriLower.rfind("rtmpe://", 0) == 0 ||
           uriLower.rfind("rtmpte://", 0) == 0 ||
           uriLower.rfind("rtmpts://", 0) == 0;
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

void configureSrtSink(GstElement* sink, const StreamConfig& cfg) {
    const std::string mode = srtOutputMode(cfg);
    const bool caller = mode == "caller";
    const std::string targetHost = cfg.outputHost.empty() || cfg.outputHost == "0.0.0.0" || cfg.outputHost == "::"
        ? "127.0.0.1"
        : cfg.outputHost;
    const std::string bindHost = cfg.interfaceAddress.empty() ? "0.0.0.0" : cfg.interfaceAddress;
    const std::string uri = "srt://" + (caller ? targetHost : bindHost) + ":" +
        std::to_string(cfg.outputPort) + "?mode=" + mode;

    g_object_set(sink,
        "uri", uri.c_str(),
        "sync", FALSE,
        "async", FALSE,
        "blocksize", static_cast<guint>(kTsPacketsPerUdpBuffer * 188),
        nullptr);

    setIntPropertyIfPresent(sink, "mode", caller ? 1 : 2);
    setBooleanPropertyIfPresent(sink, "wait-for-connection", FALSE);
    if (!caller) {
        setBooleanPropertyIfPresent(sink, "keep-listening", TRUE);
        setUIntPropertyIfPresent(sink, "localport", static_cast<guint>(cfg.outputPort));
    }
    setBooleanPropertyIfPresent(sink, "auto-reconnect", TRUE);
    setBooleanPropertyIfPresent(sink, "qos", FALSE);
    setIntPropertyIfPresent(sink, "latency", 250);
    setInt64PropertyIfPresent(sink, "max-lateness", -1);
    setStringPropertyIfPresent(sink, "localaddress", cfg.interfaceAddress);
    if (caller) {
        setUIntPropertyIfPresent(sink, "localport", 0);
    }

    if (cfg.targetBitrate > 0) {
        setUInt64PropertyIfPresent(sink, "max-bitrate", static_cast<guint64>(cfg.targetBitrate * 12 / 10));
    }
}

std::string rtmpOutputLocation(const StreamConfig& cfg) {
    const std::string type = outputType(cfg);
    const std::string host = cfg.outputHost;
    const std::string hostLower = toLower(host);

    if (isRtmpUri(hostLower)) {
        return host;
    }

    if (type == "youtube") {
        return "rtmp://a.rtmp.youtube.com/live2/" + host;
    }

    const std::string targetHost = host.empty() ? "127.0.0.1" : host;
    return "rtmp://" + targetHost + ":" + std::to_string(cfg.outputPort) + "/live/" + cfg.id;
}

void configureRtmpSink(GstElement* sink, const StreamConfig& cfg) {
    const std::string location = rtmpOutputLocation(cfg);
    g_object_set(sink,
        "location", location.c_str(),
        "sync", FALSE,
        "async", FALSE,
        "qos", FALSE,
        nullptr);
    setInt64PropertyIfPresent(sink, "max-lateness", -1);
    if (cfg.targetBitrate > 0) {
        setUInt64PropertyIfPresent(sink, "max-bitrate", static_cast<guint64>(cfg.targetBitrate * 12 / 10));
    }
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

void configureOutputQueue(GstElement* queue, const StreamConfig& cfg) {
    configureQueue(queue, outputType(cfg) == "udp" ? kUdpQueueLatency : 3000000000ULL);
}

void configureTsPacketAlignment(GstElement* element) {
    setIntPropertyIfPresent(element, "alignment", static_cast<gint>(kTsPacketsPerUdpBuffer));
}

void configureCbrPacer(GstElement* pacer, const StreamConfig& cfg) {
    if (!pacer || !cfg.cbr || cfg.targetBitrate == 0) {
        return;
    }

    g_object_set(pacer,
        "sync", TRUE,
        "silent", TRUE,
        "single-segment", TRUE,
        nullptr);

    const uint64_t bytesPerSecond = cfg.targetBitrate / 8;
    if (bytesPerSecond > 0 && bytesPerSecond <= static_cast<uint64_t>(G_MAXINT)) {
        setIntPropertyIfPresent(pacer, "datarate", static_cast<gint>(bytesPerSecond));
    }
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
        "alignment", static_cast<gint>(kTsPacketsPerUdpBuffer),
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

GstPad* requestFlvMuxSinkPad(GstElement* mux, bool isVideo) {
    return gst_element_request_pad_simple(mux, isVideo ? "video" : "audio");
}

uint32_t pidFromDemuxPadName(GstPad* pad) {
    const gchar* padName = pad ? GST_PAD_NAME(pad) : nullptr;
    if (!padName) {
        return 0;
    }

    std::string name(padName);
    const std::size_t separator = name.rfind('_');
    if (separator == std::string::npos || separator + 1 >= name.size()) {
        return 0;
    }

    try {
        const unsigned long pid = std::stoul(name.substr(separator + 1), nullptr, 16);
        return pid <= 0x1FFF ? static_cast<uint32_t>(pid) : 0;
    } catch (...) {
        return 0;
    }
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

GstElement* capsFilterForMux(bool flvMux, bool isVideo, bool isAudio, const std::string& capsString) {
    std::string capsDescription;
    if (flvMux && isVideo && capsString.find("video/x-h264") != std::string::npos) {
        capsDescription = "video/x-h264,stream-format=(string)avc";
    } else if (flvMux && isAudio && capsString.find("audio/mpeg") != std::string::npos &&
               capsString.find("mpegversion=(int)4") != std::string::npos) {
        capsDescription = "audio/mpeg,mpegversion=(int)4,stream-format=(string)raw";
    } else if (!flvMux && isVideo && capsString.find("video/x-h264") != std::string::npos) {
        capsDescription = "video/x-h264,stream-format=(string)byte-stream";
    }

    if (capsDescription.empty()) {
        return nullptr;
    }

    GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
    if (!filter) {
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string(capsDescription.c_str());
    if (!caps) {
        gst_object_unref(filter);
        return nullptr;
    }
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    return filter;
}

struct RtspPayloadFactories {
    const char* depay = nullptr;
    const char* parser = nullptr;
    const char* muxCaps = nullptr;
    bool isVideo = false;
    bool isAudio = false;
};

RtspPayloadFactories rtspPayloadFactories(const std::string& capsString) {
    const std::string capsLower = toLower(capsString);
    if (capsLower.find("media=(string)video") != std::string::npos &&
        capsLower.find("encoding-name=(string)h264") != std::string::npos) {
        return {"rtph264depay", "h264parse", "video/x-h264,stream-format=(string)byte-stream", true, false};
    }
    if (capsLower.find("media=(string)video") != std::string::npos &&
        (capsLower.find("encoding-name=(string)h265") != std::string::npos ||
         capsLower.find("encoding-name=(string)hevc") != std::string::npos)) {
        return {"rtph265depay", "h265parse", "video/x-h265,stream-format=(string)byte-stream", true, false};
    }
    if (capsLower.find("media=(string)audio") != std::string::npos &&
        (capsLower.find("encoding-name=(string)mpeg4-generic") != std::string::npos ||
         capsLower.find("encoding-name=(string)mp4a-latm") != std::string::npos)) {
        return {"rtpmp4gdepay", "aacparse", nullptr, false, true};
    }
    if (capsLower.find("media=(string)audio") != std::string::npos &&
        capsLower.find("encoding-name=(string)mpa") != std::string::npos) {
        return {"rtpmpadepay", "mpegaudioparse", nullptr, false, true};
    }
    return {};
}

GstElement* makeCapsFilter(const char* capsDescription) {
    if (!capsDescription) {
        return nullptr;
    }

    GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
    GstCaps* caps = gst_caps_from_string(capsDescription);
    if (!filter || !caps) {
        if (filter) gst_object_unref(filter);
        if (caps) gst_caps_unref(caps);
        return nullptr;
    }

    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    return filter;
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
    state->sourceContext = std::make_unique<RemapContext>();
    state->sourceContext->config = streamConfig;
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
    state.sourceContext.reset();

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
        state.sourceContext.reset();
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
         << " output_mode=" << srtOutputMode(cfg)
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
    state->sourceContext = std::make_unique<RemapContext>();
    state->sourceContext->config = state->config;
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
    if (!createSourceChain(state, pipeline, sourceTail) || !sourceTail) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    const std::string type = outputType(cfg);
    if (type == "rtmp" || type == "youtube") {
        if (!buildRtmpOutputPipeline(state, pipeline, sourceTail)) {
            gst_object_unref(pipeline);
            return nullptr;
        }
        return pipeline;
    }

    const bool needsRemux = cfg.remapEnabled || (cfg.cbr && cfg.targetBitrate > 0);
    bool ok = false;
    if (needsRemux) {
        if (!state->remapContext) {
            state->remapContext = std::make_unique<RemapContext>();
        }
        state->remapContext->config = cfg;
        ok = buildRemapPipeline(state, pipeline, sourceTail);
    } else {
        ok = buildPassthroughPipeline(state, pipeline, sourceTail);
    }

    if (!ok) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    return pipeline;
}

GstElement* StreamManager::createSourceChain(StreamState* state, GstElement* pipeline, GstElement*& terminalElement) {
    terminalElement = nullptr;
    if (!state) {
        return nullptr;
    }
    const StreamConfig& cfg = state->config;
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

    if (isRtmpUri(inputLower)) {
        if (!hasElementFactory("rtmpsrc") || !hasElementFactory("flvdemux") || !hasElementFactory("mpegtsmux")) {
            std::cerr << "missing RTMP input elements: rtmpsrc, flvdemux or mpegtsmux" << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("rtmpsrc", "input_src");
        GstElement* inputQueue = addQueue("input_queue", 5000000000ULL);
        GstElement* demux = gst_element_factory_make("flvdemux", "input_flv_demux");
        GstElement* mux = gst_element_factory_make("mpegtsmux", "input_rtmp_ts_mux");
        GstElement* outputQueue = gst_element_factory_make("queue", "input_rtmp_ts_queue");
        if (!src || !inputQueue || !demux || !mux || !outputQueue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, demux) ||
            !addElementOrFail(pipeline, mux) ||
            !addElementOrFail(pipeline, outputQueue)) {
            return nullptr;
        }

        g_object_set(src,
            "location", input.c_str(),
            "do-timestamp", TRUE,
            "timeout", 15,
            nullptr);
        configureQueue(outputQueue);
        configureTsMux(mux, cfg);

        if (!gst_element_link_many(src, inputQueue, demux, nullptr) ||
            !gst_element_link(mux, outputQueue)) {
            return nullptr;
        }

        if (!state->sourceContext) {
            state->sourceContext = std::make_unique<RemapContext>();
        }
        state->sourceContext->mux = mux;
        state->sourceContext->config = cfg;
        state->sourceContext->flvMux = false;
        g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onFlvDemuxPadAdded), state->sourceContext.get());

        terminalElement = outputQueue;
        return src;
    }

    if (inputLower.rfind("rtsp://", 0) == 0 || inputLower.rfind("rtsps://", 0) == 0) {
        if (!hasElementFactory("rtspsrc") || !hasElementFactory("mpegtsmux")) {
            std::cerr << "missing RTSP input elements: rtspsrc or mpegtsmux" << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("rtspsrc", "input_src");
        GstElement* mux = gst_element_factory_make("mpegtsmux", "input_rtsp_ts_mux");
        GstElement* outputQueue = gst_element_factory_make("queue", "input_queue");
        if (!src || !mux || !outputQueue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, mux) ||
            !addElementOrFail(pipeline, outputQueue)) {
            return nullptr;
        }

        g_object_set(src,
            "location", input.c_str(),
            "latency", 300,
            "do-rtsp-keep-alive", TRUE,
            nullptr);
        setUInt64PropertyIfPresent(src, "timeout", 15000000);
        setBooleanPropertyIfPresent(src, "ntp-sync", FALSE);
        configureQueue(outputQueue, 5000000000ULL);
        configureTsMux(mux, cfg);

        if (!gst_element_link(mux, outputQueue)) {
            return nullptr;
        }

        if (!state->sourceContext) {
            state->sourceContext = std::make_unique<RemapContext>();
        }
        state->sourceContext->mux = mux;
        state->sourceContext->config = cfg;
        state->sourceContext->flvMux = false;
        g_signal_connect(src, "pad-added", G_CALLBACK(StreamManager::onRtspPadAdded), state->sourceContext.get());

        terminalElement = outputQueue;
        return src;
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
            setUIntPropertyIfPresent(src, "localport", 0);
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

    if (UdpInput::handles(input)) {
        std::string error;
        GstElement* src = UdpInput::build(pipeline, cfg, terminalElement, error);
        if (!src) {
            std::cerr << error << std::endl;
        }
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
        "videotestsrc", "capsfilter", "videoconvert", "x264enc", "h264parse",
        "audiotestsrc", "audioconvert", "audioresample", "avenc_aac", "aacparse",
        "mpegtsmux", "queue"
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
    GstElement* videoQueue = gst_element_factory_make("queue", "test_bars_video_queue");
    GstElement* audioSrc = gst_element_factory_make("audiotestsrc", "test_tone_src");
    GstElement* audioConvert = gst_element_factory_make("audioconvert", "test_tone_convert");
    GstElement* audioResample = gst_element_factory_make("audioresample", "test_tone_resample");
    GstElement* audioCapsfilter = gst_element_factory_make("capsfilter", "test_tone_caps");
    GstElement* audioEncoder = gst_element_factory_make("avenc_aac", "test_tone_encoder");
    GstElement* audioParser = gst_element_factory_make("aacparse", "test_tone_aacparse");
    GstElement* audioQueue = gst_element_factory_make("queue", "test_tone_queue");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "test_bars_mux");
    GstElement* queue = gst_element_factory_make("queue", "test_bars_queue");

    if (!src || !capsfilter || !convert || !encoder || !parser || !videoQueue ||
        !audioSrc || !audioConvert || !audioResample || !audioCapsfilter || !audioEncoder ||
        !audioParser || !audioQueue || !mux || !queue) {
        return nullptr;
    }

    if (!addElementOrFail(pipeline, src) ||
        !addElementOrFail(pipeline, capsfilter) ||
        !addElementOrFail(pipeline, convert) ||
        !addElementOrFail(pipeline, encoder) ||
        !addElementOrFail(pipeline, parser) ||
        !addElementOrFail(pipeline, videoQueue) ||
        !addElementOrFail(pipeline, audioSrc) ||
        !addElementOrFail(pipeline, audioConvert) ||
        !addElementOrFail(pipeline, audioResample) ||
        !addElementOrFail(pipeline, audioCapsfilter) ||
        !addElementOrFail(pipeline, audioEncoder) ||
        !addElementOrFail(pipeline, audioParser) ||
        !addElementOrFail(pipeline, audioQueue) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, queue)) {
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string(
        "video/x-raw,format=I420,width=1920,height=1080,framerate=25/1,pixel-aspect-ratio=1/1");
    g_object_set(capsfilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    GstCaps* audioCaps = gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2");
    g_object_set(audioCapsfilter, "caps", audioCaps, nullptr);
    gst_caps_unref(audioCaps);

    constexpr guint audioBitrate = 128000;
    const uint64_t availableVideoBitrate = cfg.targetBitrate > 500000
        ? (cfg.targetBitrate * 85 / 100) - audioBitrate
        : 1000000;
    guint bitrateKbps = static_cast<guint>(std::max<uint64_t>(availableVideoBitrate / 1000, 350));
    g_object_set(src,
        "is-live", TRUE,
        "pattern", 0,
        nullptr);
    g_object_set(encoder,
        "bitrate", bitrateKbps,
        "key-int-max", 50,
        "bframes", 0,
        "byte-stream", TRUE,
        nullptr);
    gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
    gst_util_set_object_arg(G_OBJECT(encoder), "speed-preset", "veryfast");
    g_object_set(parser, "config-interval", 1, nullptr);
    g_object_set(audioSrc, "is-live", TRUE, "wave", 0, "freq", 1000.0, nullptr);
    g_object_set(audioEncoder, "bitrate", static_cast<gint64>(audioBitrate), nullptr);
    configureTsMux(mux, cfg);
    configureQueue(videoQueue);
    configureQueue(audioQueue);
    configureQueue(queue);

    if (!gst_element_link_many(src, capsfilter, convert, encoder, parser, videoQueue, mux, nullptr) ||
        !gst_element_link_many(audioSrc, audioConvert, audioResample, audioCapsfilter,
            audioEncoder, audioParser, audioQueue, mux, nullptr) ||
        !gst_element_link(mux, queue)) {
        return nullptr;
    }

    terminalElement = queue;
    return src;
}

bool StreamManager::buildPassthroughPipeline(StreamState* state, GstElement* pipeline, GstElement* sourceTail) {
    if (!state) {
        return false;
    }
    const StreamConfig& cfg = state->config;
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

    configureOutputQueue(queue, cfg);
    configureTsPacketAlignment(tsparse);
    if (outputType(cfg) == "udp") {
        setBooleanPropertyIfPresent(tsparse, "set-timestamps", TRUE);
        setUInt64PropertyIfPresent(tsparse, "smoothing-latency", kTsSmoothingLatency);
    }

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
    const bool cbrActive = outputType(state->config) != "udp" &&
        state->config.cbr && state->config.targetBitrate > 0;
    GstElement* outputQueue = gst_element_factory_make("queue", "output_queue");
    GstElement* pacer = cbrActive ? gst_element_factory_make("identity", "cbr_pacer") : nullptr;
    GstElement* sink = createOutputSink(state->config, pipeline);
    if (!tsparse || !preDemuxQueue || !demux || !mux || !outputQueue || !sink ||
        (cbrActive && !pacer)) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, preDemuxQueue) ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, outputQueue) ||
        (pacer && !addElementOrFail(pipeline, pacer))) {
        return false;
    }

    configureQueue(preDemuxQueue);
    configureOutputQueue(outputQueue, state->config);
    configureCbrPacer(pacer, state->config);
    configureTsMux(mux, state->config);
    sendServiceDescription(mux, state->config);

    if (!gst_element_link_many(sourceTail, tsparse, preDemuxQueue, demux, nullptr)) {
        return false;
    }
    const bool outputLinked = pacer
        ? gst_element_link_many(mux, outputQueue, pacer, sink, nullptr)
        : gst_element_link_many(mux, outputQueue, sink, nullptr);
    if (!outputLinked) {
        return false;
    }

    state->remapContext->mux = mux;
    state->remapContext->sink = sink;
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), state->remapContext.get());
    return true;
}

bool StreamManager::buildRtmpOutputPipeline(StreamState* state, GstElement* pipeline, GstElement* sourceTail) {
    if (!state) {
        return false;
    }
    if (!hasElementFactory("tsparse") || !hasElementFactory("tsdemux") || !hasElementFactory("flvmux")) {
        std::cerr << "missing RTMP output elements: tsparse, tsdemux or flvmux" << std::endl;
        return false;
    }

    GstElement* tsparse = gst_element_factory_make("tsparse", "rtmp_tsparse");
    GstElement* preDemuxQueue = gst_element_factory_make("queue", "rtmp_pre_demux_queue");
    GstElement* demux = gst_element_factory_make("tsdemux", "rtmp_ts_demux");
    GstElement* mux = gst_element_factory_make("flvmux", "rtmp_flv_mux");
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
    configureTsPacketAlignment(tsparse);
    g_object_set(mux,
        "streamable", TRUE,
        "enforce-increasing-timestamps", TRUE,
        "skip-backwards-streams", TRUE,
        nullptr);

    if (!gst_element_link_many(sourceTail, tsparse, preDemuxQueue, demux, nullptr)) {
        return false;
    }
    if (!gst_element_link_many(mux, outputQueue, sink, nullptr)) {
        return false;
    }

    if (!state->remapContext) {
        state->remapContext = std::make_unique<RemapContext>();
    }
    state->remapContext->mux = mux;
    state->remapContext->sink = sink;
    state->remapContext->config = state->config;
    state->remapContext->flvMux = true;
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), state->remapContext.get());
    return true;
}

GstElement* StreamManager::createOutputSink(const StreamConfig& cfg, GstElement* pipeline) {
    const std::string type = outputType(cfg);
    if (type == "udp") {
        std::string error;
        GstElement* sink = UdpOutput::createSink(pipeline, cfg, error);
        if (!sink) {
            std::cerr << error << std::endl;
        }
        return sink;
    }

    const char* factory = "srtsink";
    if (type == "http") {
        factory = "multifdsink";
    } else if (type == "hls") {
        factory = "hlssink";
    } else if (type == "rtmp" || type == "youtube") {
        factory = "rtmpsink";
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
    } else if (type == "rtmp" || type == "youtube") {
        configureRtmpSink(sink, cfg);
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
    GstElement* capsfilter = capsFilterForMux(ctx->flvMux, isVideo, isAudio, capsString);

    if (!queue || !parser) {
        std::cerr << "remap skipped unsupported elementary stream caps: " << capsString << std::endl;
        if (queue) gst_object_unref(queue);
        if (parser) gst_object_unref(parser);
        if (capsfilter) gst_object_unref(capsfilter);
        return;
    }

    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(ctx->mux));
    if (!pipeline) {
        gst_object_unref(queue);
        return;
    }

    if (!gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), parser) ||
        (capsfilter && !gst_bin_add(GST_BIN(pipeline), capsfilter))) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (parser && !GST_OBJECT_PARENT(parser)) gst_object_unref(parser);
        if (capsfilter && !GST_OBJECT_PARENT(capsfilter)) gst_object_unref(capsfilter);
        gst_object_unref(pipeline);
        return;
    }

    configureQueue(queue);
    if (parserFactory == "h264parse" || parserFactory == "h265parse") {
        g_object_set(parser, "config-interval", 1, nullptr);
    }
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(parser);
    if (capsfilter) {
        gst_element_sync_state_with_parent(capsfilter);
    }

    const bool parserLinked = capsfilter
        ? gst_element_link_many(queue, parser, capsfilter, nullptr)
        : gst_element_link(queue, parser);
    if (!parserLinked) {
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

    uint32_t requestedPid = isVideo ? ctx->config.videoPid : ctx->config.audioPid;
    if (requestedPid == 0) {
        requestedPid = pidFromDemuxPadName(pad);
    }

    GstElement* muxSourceElement = capsfilter ? capsfilter : parser;
    GstPad* parserSrcPad = gst_element_get_static_pad(muxSourceElement, "src");
    GstPad* muxSinkPad = ctx->flvMux
        ? requestFlvMuxSinkPad(ctx->mux, isVideo)
        : requestMuxSinkPad(ctx->mux, requestedPid);
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

void StreamManager::onFlvDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data) {
    onDemuxPadAdded(demux, pad, user_data);
}

void StreamManager::onRtspPadAdded(GstElement* src, GstPad* pad, gpointer user_data) {
    (void)src;
    auto* ctx = static_cast<RemapContext*>(user_data);
    if (!ctx || !ctx->mux) {
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    std::string capsString = caps ? gst_caps_to_string(caps) : "unknown";
    if (caps) {
        gst_caps_unref(caps);
    }

    RtspPayloadFactories factories = rtspPayloadFactories(capsString);
    if (!factories.depay || !factories.parser) {
        std::cerr << "RTSP skipped unsupported RTP caps: " << capsString << std::endl;
        return;
    }
    if ((factories.isVideo && ctx->videoLinked) || (factories.isAudio && ctx->audioLinked)) {
        return;
    }
    if (!hasElementFactory(factories.depay) || !hasElementFactory(factories.parser)) {
        std::cerr << "missing RTSP payload elements: " << factories.depay
                  << " or " << factories.parser << std::endl;
        return;
    }

    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(ctx->mux));
    if (!pipeline) {
        return;
    }

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* depay = gst_element_factory_make(factories.depay, nullptr);
    GstElement* parser = gst_element_factory_make(factories.parser, nullptr);
    GstElement* capsfilter = makeCapsFilter(factories.muxCaps);
    if (!queue || !depay || !parser) {
        if (queue) gst_object_unref(queue);
        if (depay) gst_object_unref(depay);
        if (parser) gst_object_unref(parser);
        if (capsfilter) gst_object_unref(capsfilter);
        gst_object_unref(pipeline);
        return;
    }

    if (!gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), depay) ||
        !gst_bin_add(GST_BIN(pipeline), parser) ||
        (capsfilter && !gst_bin_add(GST_BIN(pipeline), capsfilter))) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (depay && !GST_OBJECT_PARENT(depay)) gst_object_unref(depay);
        if (parser && !GST_OBJECT_PARENT(parser)) gst_object_unref(parser);
        if (capsfilter && !GST_OBJECT_PARENT(capsfilter)) gst_object_unref(capsfilter);
        gst_object_unref(pipeline);
        return;
    }

    configureQueue(queue, 5000000000ULL);
    if (g_strcmp0(factories.parser, "h264parse") == 0 || g_strcmp0(factories.parser, "h265parse") == 0) {
        g_object_set(parser, "config-interval", 1, nullptr);
    }

    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(parser);
    if (capsfilter) {
        gst_element_sync_state_with_parent(capsfilter);
    }

    const bool parserLinked = capsfilter
        ? gst_element_link_many(queue, depay, parser, capsfilter, nullptr)
        : gst_element_link_many(queue, depay, parser, nullptr);
    if (!parserLinked) {
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

    const uint32_t requestedPid = factories.isVideo ? ctx->config.videoPid : ctx->config.audioPid;
    GstElement* muxSourceElement = capsfilter ? capsfilter : parser;
    GstPad* parserSrcPad = gst_element_get_static_pad(muxSourceElement, "src");
    GstPad* muxSinkPad = requestMuxSinkPad(ctx->mux, requestedPid);
    if (!parserSrcPad || !muxSinkPad) {
        if (parserSrcPad) gst_object_unref(parserSrcPad);
        if (muxSinkPad) gst_object_unref(muxSinkPad);
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(parserSrcPad, muxSinkPad) == GST_PAD_LINK_OK) {
        const gchar* padName = GST_PAD_NAME(muxSinkPad);
        if (factories.isVideo) {
            ctx->videoLinked = true;
            ctx->videoPadName = padName ? padName : "";
        }
        if (factories.isAudio) {
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
             g_strcmp0(factoryName, "rtspsrc") == 0 ||
             g_strcmp0(factoryName, "rtmpsrc") == 0 ||
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
             g_strcmp0(factoryName, "rtmpsink") == 0 ||
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
