#include "TranscoderModule.h"

#include <algorithm>
#include <iostream>
#include <memory>

namespace {

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

GstElement* makeAudioEncoder() {
    const char* factories[] = {"avenc_aac", "fdkaacenc", "voaacenc", nullptr};
    for (const char** name = factories; *name; ++name) {
        GstElementFactory* factory = gst_element_factory_find(*name);
        if (factory) {
            gst_object_unref(factory);
            return gst_element_factory_make(*name, nullptr);
        }
    }
    return nullptr;
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
        GstElement* scale = gst_element_factory_make("videoscale", nullptr);
        GstElement* rate = gst_element_factory_make("videorate", nullptr);
        GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* encoder = gst_element_factory_make("x264enc", nullptr);
        GstElement* parser = gst_element_factory_make("h264parse", nullptr);
        GstElement* outQueue = gst_element_factory_make("queue", nullptr);
        if (!queue || !convert || !scale || !rate || !filter || !encoder || !parser || !outQueue) {
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
        g_object_set(parser, "config-interval", -1, nullptr);

        if (!add(context->bin, queue) || !add(context->bin, convert) || !add(context->bin, scale) ||
            !add(context->bin, rate) || !add(context->bin, filter) || !add(context->bin, encoder) ||
            !add(context->bin, parser) || !add(context->bin, outQueue) ||
            !gst_element_link_many(queue, convert, scale, rate, filter, encoder, parser, outQueue, nullptr) ||
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
        for (GstElement* e : {queue, convert, scale, rate, filter, encoder, parser, outQueue}) sync(e);
    } else if (media.rfind("audio/x-raw", 0) == 0 && !context->audioLinked) {
        GstElement* queue = gst_element_factory_make("queue", nullptr);
        GstElement* convert = gst_element_factory_make("audioconvert", nullptr);
        GstElement* resample = gst_element_factory_make("audioresample", nullptr);
        GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* encoder = makeAudioEncoder();
        GstElement* parser = gst_element_factory_make("aacparse", nullptr);
        GstElement* outQueue = gst_element_factory_make("queue", nullptr);
        if (!queue || !convert || !resample || !filter || !encoder || !parser || !outQueue) {
            std::cerr << "Transcoder: missing audio elements" << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        GstCaps* audioCaps = gst_caps_new_simple("audio/x-raw",
            "format", G_TYPE_STRING, "S16LE",
            "rate", G_TYPE_INT, 48000,
            "channels", G_TYPE_INT, 2,
            "layout", G_TYPE_STRING, "interleaved",
            nullptr);
        g_object_set(filter, "caps", audioCaps, nullptr);
        gst_caps_unref(audioCaps);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "bitrate")) {
            g_object_set(encoder, "bitrate", 192000, nullptr);
        }

        if (!add(context->bin, queue) || !add(context->bin, convert) || !add(context->bin, resample) ||
            !add(context->bin, filter) || !add(context->bin, encoder) || !add(context->bin, parser) ||
            !add(context->bin, outQueue) ||
            !gst_element_link_many(queue, convert, resample, filter, encoder, parser, outQueue, nullptr) ||
            !linkElementToMux(outQueue, context->mux)) {
            std::cerr << "Transcoder: failed to build audio branch" << std::endl;
            gst_caps_unref(caps);
            drainPad(context->bin, pad);
            return;
        }

        GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
        if (sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK) {
            context->audioLinked = true;
        }
        if (sinkPad) gst_object_unref(sinkPad);
        for (GstElement* e : {queue, convert, resample, filter, encoder, parser, outQueue}) sync(e);
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
        "videoconvert", "videoscale", "videorate", "capsfilter",
        "x264enc", "h264parse",
        "audioconvert", "audioresample", "aacparse",
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
    for (const char* name : {"avenc_aac", "fdkaacenc", "voaacenc"}) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (factory) {
            result.audioEncoder = name;
            gst_object_unref(factory);
            break;
        }
    }
    if (result.audioEncoder.empty()) {
        result.missingElements.emplace_back("AAC encoder (avenc_aac, fdkaacenc or voaacenc)");
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
        "bitrate", static_cast<guint64>(config.transcodeVideoBitrate + 500000),
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
