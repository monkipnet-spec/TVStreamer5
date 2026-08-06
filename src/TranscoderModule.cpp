#include "TranscoderModule.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <mutex>

namespace {


struct TimestampNormalizer {
    std::mutex mutex;
    GstClockTime lastPts = GST_CLOCK_TIME_NONE;
    GstClockTime lastDts = GST_CLOCK_TIME_NONE;
    GstClockTime fallbackDuration = 20 * GST_MSECOND;
};

GstPadProbeReturn normalizeEncodedTimestamps(GstPad*, GstPadProbeInfo* info, gpointer userData) {
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    auto* state = static_cast<TimestampNormalizer*>(userData);
    GstBuffer* input = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!state || !input) return GST_PAD_PROBE_OK;

    GstBuffer* buffer = gst_buffer_make_writable(input);
    if (!buffer) return GST_PAD_PROBE_OK;
    GST_PAD_PROBE_INFO_DATA(info) = buffer;

    std::lock_guard<std::mutex> lock(state->mutex);
    GstClockTime duration = GST_BUFFER_DURATION(buffer);
    if (!GST_CLOCK_TIME_IS_VALID(duration) || duration == 0) duration = state->fallbackDuration;

    GstClockTime pts = GST_BUFFER_PTS(buffer);
    GstClockTime dts = GST_BUFFER_DTS(buffer);

    if (!GST_CLOCK_TIME_IS_VALID(pts)) {
        pts = GST_CLOCK_TIME_IS_VALID(state->lastPts) ? state->lastPts + duration : 0;
    } else if (GST_CLOCK_TIME_IS_VALID(state->lastPts) && pts <= state->lastPts) {
        pts = state->lastPts + duration;
    }

    if (!GST_CLOCK_TIME_IS_VALID(dts)) {
        dts = GST_CLOCK_TIME_IS_VALID(state->lastDts) ? state->lastDts + duration : pts;
    } else if (GST_CLOCK_TIME_IS_VALID(state->lastDts) && dts <= state->lastDts) {
        dts = state->lastDts + duration;
    }

    if (dts > pts) pts = dts;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = dts;
    GST_BUFFER_DURATION(buffer) = duration;
    state->lastPts = pts;
    state->lastDts = dts;
    return GST_PAD_PROBE_OK;
}

void attachTimestampNormalizer(GstElement* element, GstClockTime fallbackDuration) {
    if (!element) return;
    GstPad* srcPad = gst_element_get_static_pad(element, "src");
    if (!srcPad) return;
    auto* state = new TimestampNormalizer();
    state->fallbackDuration = fallbackDuration;
    gst_pad_add_probe(srcPad, GST_PAD_PROBE_TYPE_BUFFER, normalizeEncodedTimestamps, state,
        [](gpointer data) { delete static_cast<TimestampNormalizer*>(data); });
    gst_object_unref(srcPad);
}

struct TranscodeContext {
    GstElement* bin = nullptr;
    GstElement* mux = nullptr;
    StreamConfig config;
    bool videoLinked = false;
    bool audioLinked = false;
};

bool add(GstElement* bin, GstElement* element) {
    return bin && element && gst_bin_add(GST_BIN(bin), element);
}

void sync(GstElement* element) {
    if (element) gst_element_sync_state_with_parent(element);
}

struct AudioEncoderSelection {
    GstElement* element = nullptr;
    std::string factory;
};

AudioEncoderSelection makeAudioEncoder(const std::string& codec) {
    const char* const* factories = nullptr;
    static const char* aacFactories[] = {"avenc_aac", "fdkaacenc", "voaacenc", nullptr};
    static const char* mp3Factories[] = {"lamemp3enc", "avenc_mp3", nullptr};
    factories = codec == "mp3" ? mp3Factories : aacFactories;
    for (const char* const* name = factories; *name; ++name) {
        GstElementFactory* factory = gst_element_factory_find(*name);
        if (factory) {
            gst_object_unref(factory);
            return {gst_element_factory_make(*name, nullptr), *name};
        }
    }
    return {};
}

void configureAudioBitrate(GstElement* encoder, const std::string& factory, uint64_t bitrate) {
    if (!encoder) return;
    bitrate = std::clamp<uint64_t>(bitrate, 64000, 320000);
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "bitrate")) return;
    // lamemp3enc uses kbit/s; libav and AAC encoders use bit/s.
    if (factory == "lamemp3enc") {
        g_object_set(encoder, "bitrate", static_cast<gint>(bitrate / 1000), nullptr);
    } else {
        g_object_set(encoder, "bitrate", static_cast<gint>(bitrate), nullptr);
    }
}

bool linkElementToMux(GstElement* source, GstElement* mux) {
    if (!source || !mux) return false;
    GstPad* srcPad = gst_element_get_static_pad(source, "src");
    GstPad* sinkPad = gst_element_request_pad_simple(mux, "sink_%d");
    if (!srcPad || !sinkPad) {
        if (srcPad) gst_object_unref(srcPad);
        if (sinkPad) gst_object_unref(sinkPad);
        return false;
    }
    const bool ok = gst_pad_link(srcPad, sinkPad) == GST_PAD_LINK_OK;
    gst_object_unref(srcPad);
    gst_object_unref(sinkPad);
    return ok;
}

void drainPad(GstElement* bin, GstPad* pad) {
    if (!bin || !pad || gst_pad_is_linked(pad)) return;
    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (!queue || !sink || !add(bin, queue) || !add(bin, sink) || !gst_element_link(queue, sink)) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (sink && !GST_OBJECT_PARENT(sink)) gst_object_unref(sink);
        return;
    }
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    GstPad* queueSink = gst_element_get_static_pad(queue, "sink");
    if (queueSink) {
        gst_pad_link(pad, queueSink);
        gst_object_unref(queueSink);
    }
    sync(queue);
    sync(sink);
}

void onDecodedPadAdded(GstElement*, GstPad* pad, gpointer userData) {
    auto* context = static_cast<TranscodeContext*>(userData);
    if (!context || !context->bin || !context->mux) return;

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps || gst_caps_is_empty(caps)) {
        if (caps) gst_caps_unref(caps);
        drainPad(context->bin, pad);
        return;
    }
    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    const std::string media = gst_structure_get_name(structure);

    if (media.rfind("video/x-raw", 0) == 0 && !context->videoLinked) {
        int width = 1920, height = 1080;
        TranscoderModule::resolutionSize(context->config.transcodeResolution, width, height);
        const guint bitrateKbps = static_cast<guint>(
            std::max<uint64_t>(500000, context->config.transcodeVideoBitrate) / 1000);

        GstElement* queue = gst_element_factory_make("queue", nullptr);
        GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
        GstElement* deinterlace = gst_element_factory_make("deinterlace", nullptr);
        GstElement* scale = gst_element_factory_make("videoscale", nullptr);
        GstElement* rate = gst_element_factory_make("videorate", nullptr);
        GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* encoder = gst_element_factory_make("x264enc", nullptr);
        GstElement* parser = gst_element_factory_make("h264parse", nullptr);
        GstElement* outQueue = gst_element_factory_make("queue", nullptr);
        if (!queue || !convert || !deinterlace || !scale || !rate || !filter || !encoder || !parser || !outQueue) {
            std::cerr << "Transcoder: missing video elements" << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        GstCaps* rawCaps = gst_caps_new_simple("video/x-raw",
            "format", G_TYPE_STRING, "I420",
            "width", G_TYPE_INT, width,
            "height", G_TYPE_INT, height,
            "framerate", GST_TYPE_FRACTION, 25, 1,
            "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
            "interlace-mode", G_TYPE_STRING, "progressive",
            nullptr);
        g_object_set(filter, "caps", rawCaps, nullptr);
        gst_caps_unref(rawCaps);
        g_object_set(encoder,
            "bitrate", bitrateKbps,
            "key-int-max", 50,
            "bframes", 2,
            "byte-stream", TRUE,
            "aud", TRUE,
            "vbv-buf-capacity", 1000u,
            nullptr);
        gst_util_set_object_arg(G_OBJECT(encoder), "speed-preset", "veryfast");
        gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
        g_object_set(encoder, "option-string", "nal-hrd=cbr:force-cfr=1", nullptr);
        g_object_set(parser, "config-interval", 1, nullptr);

        if (!add(context->bin, queue) || !add(context->bin, convert) || !add(context->bin, deinterlace) ||
            !add(context->bin, scale) || !add(context->bin, rate) || !add(context->bin, filter) ||
            !add(context->bin, encoder) || !add(context->bin, parser) || !add(context->bin, outQueue) ||
            !gst_element_link_many(queue, convert, deinterlace, scale, rate, filter, encoder, parser, outQueue, nullptr) ||
            !linkElementToMux(outQueue, context->mux)) {
            std::cerr << "Transcoder: failed to build video branch" << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
        if (sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK) {
            context->videoLinked = true;
        }
        if (sinkPad) gst_object_unref(sinkPad);
        for (GstElement* e : {queue, convert, deinterlace, scale, rate, filter, encoder, parser, outQueue}) sync(e);
    } else if (media.rfind("audio/x-raw", 0) == 0 && !context->audioLinked) {
        const std::string codec = context->config.transcodeAudioCodec == "mp3" ? "mp3" : "aac";
        GstElement* queue = gst_element_factory_make("queue", nullptr);
        GstElement* convert = gst_element_factory_make("audioconvert", nullptr);
        GstElement* resample = gst_element_factory_make("audioresample", nullptr);
        GstElement* rate = gst_element_factory_make("audiorate", nullptr);
        GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
        const auto encoderSelection = makeAudioEncoder(codec);
        GstElement* encoder = encoderSelection.element;
        GstElement* parser = gst_element_factory_make(codec == "mp3" ? "mpegaudioparse" : "aacparse", nullptr);
        GstElement* encodedFilter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* outQueue = gst_element_factory_make("queue", nullptr);
        if (!queue || !convert || !resample || !rate || !filter || !encoder || !parser || !encodedFilter || !outQueue) {
            std::cerr << "Transcoder: missing " << codec << " audio elements" << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        GstCaps* audioCaps = gst_caps_new_simple("audio/x-raw",
            "rate", G_TYPE_INT, 48000,
            "channels", G_TYPE_INT, 2,
            "layout", G_TYPE_STRING, "interleaved",
            nullptr);
        g_object_set(filter, "caps", audioCaps, nullptr);
        gst_caps_unref(audioCaps);
        configureAudioBitrate(encoder, encoderSelection.factory, context->config.transcodeAudioBitrate);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(rate), "skip-to-first")) {
            g_object_set(rate, "skip-to-first", TRUE, nullptr);
        }
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(rate), "tolerance")) {
            g_object_set(rate, "tolerance", static_cast<guint64>(20 * GST_MSECOND), nullptr);
        }

        GstCaps* encodedCaps = nullptr;
        if (codec == "mp3") {
            encodedCaps = gst_caps_from_string(
                "audio/mpeg,mpegversion=(int)1,layer=(int)3");
        } else {
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(parser), "disable-passthrough")) {
                g_object_set(parser, "disable-passthrough", TRUE, nullptr);
            }
            encodedCaps = gst_caps_from_string(
                "audio/mpeg,mpegversion=(int)4,stream-format=(string)raw");
        }
        g_object_set(encodedFilter, "caps", encodedCaps, nullptr);
        gst_caps_unref(encodedCaps);

        // Encoder delay and discontinuous source timestamps can make AAC/MP3 DTS move
        // backwards. mpegtsmux then accepts only the first audio frames and silently
        // drops the rest. Normalize encoded timestamps immediately before muxing.
        const GstClockTime audioFrameDuration = codec == "mp3"
            ? gst_util_uint64_scale_int(GST_SECOND, 1152, 48000)
            : gst_util_uint64_scale_int(GST_SECOND, 1024, 48000);
        attachTimestampNormalizer(encodedFilter, audioFrameDuration);

        if (!add(context->bin, queue) || !add(context->bin, convert) || !add(context->bin, resample) ||
            !add(context->bin, rate) || !add(context->bin, filter) || !add(context->bin, encoder) || !add(context->bin, parser) ||
            !add(context->bin, encodedFilter) || !add(context->bin, outQueue) ||
            !gst_element_link_many(queue, convert, resample, rate, filter, encoder, parser, encodedFilter, outQueue, nullptr) ||
            !linkElementToMux(outQueue, context->mux)) {
            std::cerr << "Transcoder: failed to build " << codec << " audio branch with "
                      << encoderSelection.factory << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
        if (sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK) {
            context->audioLinked = true;
            std::cerr << "Transcoder: audio linked using " << encoderSelection.factory
                      << " at " << context->config.transcodeAudioBitrate << " bit/s" << std::endl;
        }
        if (sinkPad) gst_object_unref(sinkPad);
        for (GstElement* e : {queue, convert, resample, rate, filter, encoder, parser, encodedFilter, outQueue}) sync(e);
    } else {
        // Multiple programs, subtitles, data PIDs, and duplicate audio/video tracks must
        // be consumed. Leaving a tsdemux pad unlinked can propagate GST_FLOW_NOT_LINKED
        // and stop the complete stream.
        drainPad(context->bin, pad);
    }
    gst_caps_unref(caps);
}

void onDemuxPadAdded(GstElement*, GstPad* pad, gpointer userData) {
    auto* context = static_cast<TranscodeContext*>(userData);
    if (!context || !context->bin) return;

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    std::string capsText = caps ? gst_caps_to_string(caps) : "";
    if (caps) gst_caps_unref(caps);
    const bool mediaPad = capsText.find("video/") != std::string::npos ||
                          capsText.find("audio/") != std::string::npos;
    if (!mediaPad) {
        drainPad(context->bin, pad);
        return;
    }

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* decode = gst_element_factory_make("decodebin", nullptr);
    if (!queue || !decode || !add(context->bin, queue) || !add(context->bin, decode) ||
        !gst_element_link(queue, decode)) {
        std::cerr << "Transcoder: failed to create decoder branch" << std::endl;
        drainPad(context->bin, pad);
        return;
    }
    GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
    const bool linked = sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK;
    if (sinkPad) gst_object_unref(sinkPad);
    if (!linked) {
        drainPad(context->bin, pad);
        return;
    }
    g_signal_connect(decode, "pad-added", G_CALLBACK(onDecodedPadAdded), context);
    sync(queue);
    sync(decode);
}

} // namespace

TranscoderCapabilities TranscoderModule::inspectCapabilities() {
    TranscoderCapabilities result;
    const char* required[] = {
        "tsparse", "tsdemux", "decodebin", "queue", "fakesink",
        "videoconvert", "deinterlace", "videoscale", "videorate", "capsfilter",
        "x264enc", "h264parse",
        "audioconvert", "audioresample",
        "mpegtsmux", nullptr
    };

    for (const char** name = required; *name; ++name) {
        GstElementFactory* factory = gst_element_factory_find(*name);
        if (!factory) result.missingElements.emplace_back(*name);
        else gst_object_unref(factory);
    }

    GstElementFactory* videoFactory = gst_element_factory_find("x264enc");
    if (videoFactory) {
        result.videoEncoder = "x264enc";
        gst_object_unref(videoFactory);
    }
    GstElementFactory* aacParser = gst_element_factory_find("aacparse");
    if (aacParser) {
        gst_object_unref(aacParser);
        for (const char* name : {"avenc_aac", "fdkaacenc", "voaacenc"}) {
            GstElementFactory* factory = gst_element_factory_find(name);
            if (factory) {
                result.aacEncoder = name;
                gst_object_unref(factory);
                break;
            }
        }
    }
    GstElementFactory* mp3Parser = gst_element_factory_find("mpegaudioparse");
    if (mp3Parser) {
        gst_object_unref(mp3Parser);
        for (const char* name : {"lamemp3enc", "avenc_mp3"}) {
            GstElementFactory* factory = gst_element_factory_find(name);
            if (factory) {
                result.mp3Encoder = name;
                gst_object_unref(factory);
                break;
            }
        }
    }
    result.audioEncoder = !result.aacEncoder.empty() ? result.aacEncoder : result.mp3Encoder;
    if (GstElementFactory* factory = gst_element_factory_find("deinterlace")) {
        result.deinterlaceAvailable = true;
        gst_object_unref(factory);
    }
    if (result.aacEncoder.empty() && result.mp3Encoder.empty()) {
        result.missingElements.emplace_back("audio encoder (AAC or MP3)");
    }

    result.available = result.missingElements.empty();
    result.message = result.available
        ? "Software transcoding is available"
        : "Transcoding is unavailable because required GStreamer elements are missing";
    return result;
}

bool TranscoderModule::resolutionSize(const std::string& value, int& width, int& height) {
    if (value == "3840x2160") { width = 3840; height = 2160; return true; }
    if (value == "3200x1800") { width = 3200; height = 1800; return true; }
    if (value == "2560x1440") { width = 2560; height = 1440; return true; }
    if (value == "1920x1080") { width = 1920; height = 1080; return true; }
    if (value == "1280x720") { width = 1280; height = 720; return true; }
    if (value == "720x576") { width = 720; height = 576; return true; }
    return false;
}

uint64_t TranscoderModule::recommendedVideoBitrate(const std::string& value) {
    if (value == "3840x2160") return 25000000;
    if (value == "3200x1800") return 18000000;
    if (value == "2560x1440") return 12000000;
    if (value == "1920x1080") return 6000000;
    if (value == "1280x720") return 3500000;
    if (value == "720x576") return 2000000;
    return 6000000;
}

GstElement* TranscoderModule::createBin(const StreamConfig& config, std::string& error) {
    int width = 0, height = 0;
    if (!resolutionSize(config.transcodeResolution, width, height)) {
        error = "unsupported transcode resolution";
        return nullptr;
    }
    const auto capabilities = inspectCapabilities();
    if (!capabilities.available) {
        error = capabilities.message;
        if (!capabilities.missingElements.empty()) {
            error += ": ";
            for (size_t i = 0; i < capabilities.missingElements.size(); ++i) {
                if (i) error += ", ";
                error += capabilities.missingElements[i];
            }
        }
        return nullptr;
    }

    const std::string audioCodec = config.transcodeAudioCodec == "mp3" ? "mp3" : "aac";
    if ((audioCodec == "aac" && capabilities.aacEncoder.empty()) ||
        (audioCodec == "mp3" && capabilities.mp3Encoder.empty())) {
        error = audioCodec + " encoder is not available";
        return nullptr;
    }

    GstElement* bin = gst_bin_new("transcoder_bin");
    GstElement* parse = gst_element_factory_make("tsparse", "transcode_tsparse");
    GstElement* queue = gst_element_factory_make("queue", "transcode_input_queue");
    GstElement* demux = gst_element_factory_make("tsdemux", "transcode_demux");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "transcode_mux");
    GstElement* outputParse = gst_element_factory_make("tsparse", "transcode_output_tsparse");
    if (!bin || !parse || !queue || !demux || !mux || !outputParse ||
        !add(bin, parse) || !add(bin, queue) || !add(bin, demux) || !add(bin, mux) ||
        !add(bin, outputParse)) {
        error = "failed to create transcoder bin elements";
        if (bin) gst_object_unref(bin);
        return nullptr;
    }

    g_object_set(parse, "set-timestamps", TRUE, nullptr);
    g_object_set(mux,
        "alignment", 7,
        "bitrate", static_cast<guint64>(config.transcodeVideoBitrate + config.transcodeAudioBitrate + 350000),
        nullptr);
    g_object_set(outputParse, "set-timestamps", TRUE, nullptr);
    if (!gst_element_link_many(parse, queue, demux, nullptr) ||
        !gst_element_link(mux, outputParse)) {
        error = "failed to link transcoder bin core";
        gst_object_unref(bin);
        return nullptr;
    }

    GstPad* parseSink = gst_element_get_static_pad(parse, "sink");
    GstPad* outputSrc = gst_element_get_static_pad(outputParse, "src");
    GstPad* ghostSink = parseSink ? gst_ghost_pad_new("sink", parseSink) : nullptr;
    GstPad* ghostSrc = outputSrc ? gst_ghost_pad_new("src", outputSrc) : nullptr;
    if (parseSink) gst_object_unref(parseSink);
    if (outputSrc) gst_object_unref(outputSrc);
    if (!ghostSink || !ghostSrc || !gst_element_add_pad(bin, ghostSink) ||
        !gst_element_add_pad(bin, ghostSrc)) {
        if (ghostSink && !GST_OBJECT_PARENT(ghostSink)) gst_object_unref(ghostSink);
        if (ghostSrc && !GST_OBJECT_PARENT(ghostSrc)) gst_object_unref(ghostSrc);
        error = "failed to create transcoder ghost pads";
        gst_object_unref(bin);
        return nullptr;
    }

    auto* context = new TranscodeContext();
    context->bin = bin;
    context->mux = mux;
    context->config = config;
    g_object_set_data_full(G_OBJECT(bin), "tvstreamer-transcode-context", context,
        [](gpointer p) { delete static_cast<TranscodeContext*>(p); });
    g_signal_connect(demux, "pad-added", G_CALLBACK(onDemuxPadAdded), context);
    return bin;
}
