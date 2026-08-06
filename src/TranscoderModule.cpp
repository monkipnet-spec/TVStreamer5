#include "TranscoderModule.h"

#include <algorithm>
#include <iostream>
#include <memory>

namespace {

struct TranscodeContext {
    GstElement* pipeline = nullptr;
    GstElement* mux = nullptr;
    StreamConfig config;
    bool videoLinked = false;
    bool audioLinked = false;
};

bool add(GstElement* pipeline, GstElement* element) {
    return pipeline && element && gst_bin_add(GST_BIN(pipeline), element);
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

void onDecodedPadAdded(GstElement*, GstPad* pad, gpointer userData) {
    auto* context = static_cast<TranscodeContext*>(userData);
    if (!context || !context->pipeline || !context->mux) return;

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps || gst_caps_is_empty(caps)) {
        if (caps) gst_caps_unref(caps);
        return;
    }
    const GstStructure* structure = gst_caps_get_structure(caps, 0);
    const std::string media = gst_structure_get_name(structure);

    if (media.rfind("video/x-raw", 0) == 0 && !context->videoLinked) {
        int width = 1920, height = 1080;
        TranscoderModule::resolutionSize(context->config.transcodeResolution, width, height);
        const guint bitrateKbps = static_cast<guint>(std::max<uint64_t>(500000, context->config.transcodeVideoBitrate) / 1000);

        GstElement* queue = gst_element_factory_make("queue", nullptr);
        GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
        GstElement* scale = gst_element_factory_make("videoscale", nullptr);
        GstElement* rate = gst_element_factory_make("videorate", nullptr);
        GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* encoder = gst_element_factory_make("x264enc", nullptr);
        GstElement* parser = gst_element_factory_make("h264parse", nullptr);
        GstElement* outQueue = gst_element_factory_make("queue", nullptr);
        if (!queue || !convert || !scale || !rate || !filter || !encoder || !parser || !outQueue) {
            std::cerr << "Transcoder: missing video elements (videoconvert/videoscale/videorate/x264enc/h264parse)" << std::endl;
            gst_caps_unref(caps);
            return;
        }
        GstCaps* rawCaps = gst_caps_new_simple("video/x-raw",
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

        if (!add(context->pipeline, queue) || !add(context->pipeline, convert) || !add(context->pipeline, scale) ||
            !add(context->pipeline, rate) || !add(context->pipeline, filter) || !add(context->pipeline, encoder) ||
            !add(context->pipeline, parser) || !add(context->pipeline, outQueue) ||
            !gst_element_link_many(queue, convert, scale, rate, filter, encoder, parser, outQueue, context->mux, nullptr)) {
            std::cerr << "Transcoder: failed to build video branch" << std::endl;
            gst_caps_unref(caps);
            return;
        }
        GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
        if (sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK) context->videoLinked = true;
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
            std::cerr << "Transcoder: missing AAC audio encoder or audio elements" << std::endl;
            gst_caps_unref(caps);
            return;
        }
        GstCaps* audioCaps = gst_caps_new_simple("audio/x-raw",
            "rate", G_TYPE_INT, 48000,
            "channels", G_TYPE_INT, 2,
            nullptr);
        g_object_set(filter, "caps", audioCaps, nullptr);
        gst_caps_unref(audioCaps);
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "bitrate")) {
            g_object_set(encoder, "bitrate", 192000, nullptr);
        }
        if (!add(context->pipeline, queue) || !add(context->pipeline, convert) || !add(context->pipeline, resample) ||
            !add(context->pipeline, filter) || !add(context->pipeline, encoder) || !add(context->pipeline, parser) ||
            !add(context->pipeline, outQueue) ||
            !gst_element_link_many(queue, convert, resample, filter, encoder, parser, outQueue, context->mux, nullptr)) {
            std::cerr << "Transcoder: failed to build audio branch" << std::endl;
            gst_caps_unref(caps);
            return;
        }
        GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
        if (sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK) context->audioLinked = true;
        if (sinkPad) gst_object_unref(sinkPad);
        for (GstElement* e : {queue, convert, resample, filter, encoder, parser, outQueue}) sync(e);
    }
    gst_caps_unref(caps);
}

void onDemuxPadAdded(GstElement*, GstPad* pad, gpointer userData) {
    auto* context = static_cast<TranscodeContext*>(userData);
    if (!context || !context->pipeline) return;

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* decode = gst_element_factory_make("decodebin", nullptr);
    if (!queue || !decode || !add(context->pipeline, queue) || !add(context->pipeline, decode) || !gst_element_link(queue, decode)) {
        std::cerr << "Transcoder: failed to create decoder branch" << std::endl;
        return;
    }
    GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
    const bool linked = sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK;
    if (sinkPad) gst_object_unref(sinkPad);
    if (!linked) return;
    g_signal_connect(decode, "pad-added", G_CALLBACK(onDecodedPadAdded), context);
    sync(queue);
    sync(decode);
}

} // namespace

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

GstElement* TranscoderModule::build(GstElement* pipeline, GstElement* sourceTail, const StreamConfig& config, std::string& error) {
    if (!pipeline || !sourceTail) { error = "invalid transcoder pipeline"; return nullptr; }
    int width = 0, height = 0;
    if (!resolutionSize(config.transcodeResolution, width, height)) { error = "unsupported transcode resolution"; return nullptr; }
    for (const char* factory : {"tsparse", "tsdemux", "decodebin", "videoconvert", "videoscale", "videorate", "x264enc", "h264parse", "mpegtsmux"}) {
        GstElementFactory* found = gst_element_factory_find(factory);
        if (!found) { error = std::string("missing GStreamer element: ") + factory; return nullptr; }
        gst_object_unref(found);
    }

    GstElement* parse = gst_element_factory_make("tsparse", "transcode_tsparse");
    GstElement* queue = gst_element_factory_make("queue", "transcode_input_queue");
    GstElement* demux = gst_element_factory_make("tsdemux", "transcode_demux");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "transcode_mux");
    if (!parse || !queue || !demux || !mux || !add(pipeline, parse) || !add(pipeline, queue) || !add(pipeline, demux) || !add(pipeline, mux)) {
        error = "failed to create transcoder elements";
        return nullptr;
    }
    g_object_set(mux, "alignment", 7, "bitrate", static_cast<guint64>(config.transcodeVideoBitrate + 500000), nullptr);
    if (!gst_element_link_many(sourceTail, parse, queue, demux, nullptr)) {
        error = "failed to link transcoder input";
        return nullptr;
    }

    auto* context = new TranscodeContext();
    context->pipeline = pipeline;
    context->mux = mux;
    context->config = config;
    g_object_set_data_full(G_OBJECT(demux), "tvstreamer-transcode-context", context, [](gpointer p) {
        delete static_cast<TranscodeContext*>(p);
    });
    g_signal_connect(demux, "pad-added", G_CALLBACK(onDemuxPadAdded), context);
    return mux;
}
