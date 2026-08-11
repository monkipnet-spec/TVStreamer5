#include "MpegTsServiceDetector.h"

#include "protocols/GstInputProtocols.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kTsPacketSize = 188;

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool hasProperty(GObject* object, const char* name) {
    return object && g_object_class_find_property(G_OBJECT_GET_CLASS(object), name) != nullptr;
}

struct SourceLinkContext {
    GstElement* queue = nullptr;
};

void onSourcePadAdded(GstElement*, GstPad* srcPad, gpointer userData) {
    auto* context = static_cast<SourceLinkContext*>(userData);
    if (!context || !context->queue || !srcPad) return;

    GstPad* sinkPad = gst_element_get_static_pad(context->queue, "sink");
    if (!sinkPad) return;
    if (!gst_pad_is_linked(sinkPad)) {
        const GstPadLinkReturn result = gst_pad_link(srcPad, sinkPad);
        if (result != GST_PAD_LINK_OK) {
            std::cerr << "Input SID detector: failed to link urisourcebin pad, code="
                      << static_cast<int>(result) << std::endl;
        }
    }
    gst_object_unref(sinkPad);
}

class PatParser {
public:
    void feed(const uint8_t* data, std::size_t size) {
        if (!data || size == 0 || complete_) return;
        pending_.insert(pending_.end(), data, data + size);
        parsePending();
    }

    const std::vector<uint32_t>& serviceIds() const { return serviceIds_; }
    bool complete() const { return complete_; }

private:
    void parsePending() {
        while (pending_.size() >= kTsPacketSize && !complete_) {
            std::size_t sync = 0;
            while (sync < pending_.size() && pending_[sync] != 0x47) ++sync;
            if (sync > 0) {
                pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(sync));
                if (pending_.size() < kTsPacketSize) return;
            }
            if (pending_.front() != 0x47) return;
            parsePacket(pending_.data());
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(kTsPacketSize));
        }
    }

    void appendSectionBytes(const uint8_t* data, std::size_t size) {
        if (!data || size == 0 || complete_) return;
        section_.insert(section_.end(), data, data + size);
        if (expectedSectionBytes_ == 0 && section_.size() >= 3) {
            if (section_[0] != 0x00) {
                section_.clear();
                return;
            }
            const std::size_t sectionLength =
                static_cast<std::size_t>(((section_[1] & 0x0F) << 8) | section_[2]);
            expectedSectionBytes_ = 3 + sectionLength;
            if (expectedSectionBytes_ < 12 || expectedSectionBytes_ > 1024) {
                section_.clear();
                expectedSectionBytes_ = 0;
                return;
            }
        }
        if (expectedSectionBytes_ > 0 && section_.size() >= expectedSectionBytes_) {
            parsePatSection();
        }
    }

    void parsePatSection() {
        if (section_.size() < expectedSectionBytes_ || expectedSectionBytes_ < 12) return;
        // PAT header is 8 bytes. The final 4 bytes are CRC32.
        const std::size_t programsEnd = expectedSectionBytes_ - 4;
        std::set<uint32_t> seen;
        for (std::size_t pos = 8; pos + 4 <= programsEnd; pos += 4) {
            const uint32_t programNumber =
                (static_cast<uint32_t>(section_[pos]) << 8) | section_[pos + 1];
            if (programNumber == 0) continue; // NIT PID entry, not a service.
            if (seen.insert(programNumber).second) {
                serviceIds_.push_back(programNumber);
            }
        }
        complete_ = !serviceIds_.empty();
        if (!complete_) {
            section_.clear();
            expectedSectionBytes_ = 0;
        }
    }

    void parsePacket(const uint8_t* packet) {
        if (!packet || packet[0] != 0x47 || complete_) return;
        const bool payloadUnitStart = (packet[1] & 0x40) != 0;
        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
        if (pid != 0x0000) return;

        const uint8_t adaptationFieldControl = (packet[3] >> 4) & 0x03;
        if (adaptationFieldControl == 0 || adaptationFieldControl == 2) return;

        std::size_t pos = 4;
        if (adaptationFieldControl == 3) {
            if (pos >= kTsPacketSize) return;
            const std::size_t adaptationLength = packet[pos];
            pos += 1 + adaptationLength;
            if (pos >= kTsPacketSize) return;
        }

        if (payloadUnitStart) {
            if (pos >= kTsPacketSize) return;
            const std::size_t pointerField = packet[pos++];
            if (pos + pointerField > kTsPacketSize) return;
            pos += pointerField;
            section_.clear();
            expectedSectionBytes_ = 0;
        } else if (section_.empty()) {
            return;
        }

        if (pos < kTsPacketSize) {
            appendSectionBytes(packet + pos, kTsPacketSize - pos);
        }
    }

    std::vector<uint8_t> pending_;
    std::vector<uint8_t> section_;
    std::size_t expectedSectionBytes_ = 0;
    std::vector<uint32_t> serviceIds_;
    bool complete_ = false;
};

} // namespace

bool MpegTsServiceDetector::supports(const StreamConfig& config) {
    if (config.testPattern || config.inputUri.empty()) return false;
    const std::string uri = toLower(tvs::protocols::inputUriForGstreamer(config));
    return uri.rfind("srt://", 0) == 0 ||
           uri.rfind("udp://", 0) == 0 ||
           uri.rfind("rtp://", 0) == 0 ||
           uri.rfind("file://", 0) == 0 ||
           uri.size() >= 3 && uri.substr(uri.size() - 3) == ".ts";
}

MpegTsServiceDetectionResult MpegTsServiceDetector::detect(
    const StreamConfig& config,
    std::chrono::milliseconds timeout) {
    MpegTsServiceDetectionResult result;
    if (!supports(config)) {
        result.error = "input protocol is not a direct MPEG-TS source";
        return result;
    }

    GstElement* pipeline = gst_pipeline_new("input_sid_detector");
    GstElement* source = gst_element_factory_make("urisourcebin", "input_sid_detector_source");
    GstElement* queue = gst_element_factory_make("queue", "input_sid_detector_queue");
    GstElement* tsparse = gst_element_factory_make("tsparse", "input_sid_detector_tsparse");
    GstElement* appsink = gst_element_factory_make("appsink", "input_sid_detector_sink");
    if (!pipeline || !source || !queue || !tsparse || !appsink) {
        result.error = "missing GStreamer elements: urisourcebin/queue/tsparse/appsink";
        if (pipeline) gst_object_unref(pipeline);
        if (source) gst_object_unref(source);
        if (queue) gst_object_unref(queue);
        if (tsparse) gst_object_unref(tsparse);
        if (appsink) gst_object_unref(appsink);
        return result;
    }

    const std::string uri = tvs::protocols::inputUriForGstreamer(config);
    g_object_set(source, "uri", uri.c_str(), nullptr);
    if (hasProperty(G_OBJECT(source), "use-buffering")) {
        g_object_set(source, "use-buffering", FALSE, nullptr);
    }
    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", static_cast<guint64>(2 * GST_SECOND),
        nullptr);
    if (hasProperty(G_OBJECT(tsparse), "alignment")) {
        g_object_set(tsparse, "alignment", 7, nullptr);
    }
    g_object_set(appsink,
        "emit-signals", FALSE,
        "sync", FALSE,
        "max-buffers", 128u,
        "drop", TRUE,
        nullptr);

    gst_bin_add_many(GST_BIN(pipeline), source, queue, tsparse, appsink, nullptr);
    if (!gst_element_link_many(queue, tsparse, appsink, nullptr)) {
        result.error = "failed to link SID detector queue/tsparse/appsink";
        gst_object_unref(pipeline);
        return result;
    }

    SourceLinkContext linkContext{queue};
    g_signal_connect(source, "pad-added", G_CALLBACK(onSourcePadAdded), &linkContext);

    GstStateChangeReturn stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        result.error = "failed to start MPEG-TS SID detector";
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return result;
    }

    PatParser parser;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline && !parser.complete()) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 100 * GST_MSECOND);
        if (!sample) {
            GstBus* bus = gst_element_get_bus(pipeline);
            if (bus) {
                GstMessage* message = gst_bus_pop_filtered(
                    bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
                if (message) {
                    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                        GError* error = nullptr;
                        gchar* debug = nullptr;
                        gst_message_parse_error(message, &error, &debug);
                        result.error = error && error->message ? error->message : "SID detector GStreamer error";
                        if (error) g_error_free(error);
                        g_free(debug);
                    }
                    gst_message_unref(message);
                    gst_object_unref(bus);
                    break;
                }
                gst_object_unref(bus);
            }
            continue;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (buffer) {
            GstMapInfo map {};
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                parser.feed(map.data, map.size);
                gst_buffer_unmap(buffer, &map);
            }
        }
        gst_sample_unref(sample);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    result.serviceIds = parser.serviceIds();
    if (result.serviceIds.empty() && result.error.empty()) {
        result.error = "PAT did not contain a service before timeout";
    }
    return result;
}
