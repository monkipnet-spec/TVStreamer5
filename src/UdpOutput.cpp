#include "UdpOutput.h"

#include <regex>

#include "utils.h"

namespace {

constexpr gint kSocketBufferSize = 64 * 1024 * 1024;
constexpr guint kTsPacketsPerDatagram = 7;
constexpr guint kSmoothingLatencyUs = 100'000;

bool isMulticastHost(const std::string& host) {
    static const std::regex pattern(R"(^((22[4-9])|(23[0-9]))\.)");
    return std::regex_search(host, pattern);
}

std::string interfaceNameOrAddress(const std::string& address) {
    for (const auto& iface : enumerateNetworkInterfaces()) {
        if (iface.address == address) {
            return iface.name;
        }
    }
    return address;
}

} // namespace

namespace UdpOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    std::string& error) {
    GstElement* outputBin = gst_bin_new("output_sink");
    GstElement* packetizer = gst_element_factory_make("tsparse", "udp_packetizer");
    GstElement* queue = gst_element_factory_make("queue", "udp_output_queue");
    GstElement* pacer = gst_element_factory_make("clocksync", "udp_pacer");
    GstElement* sink = gst_element_factory_make("udpsink", "udp_socket_sink");
    if (!outputBin || !packetizer || !queue || !pacer || !sink) {
        error = "missing UDP output elements";
        return nullptr;
    }

    gst_bin_add_many(GST_BIN(outputBin), packetizer, queue, pacer, sink, nullptr);
    if (!gst_element_link_many(packetizer, queue, pacer, sink, nullptr)) {
        error = "failed to link UDP output elements";
        gst_object_unref(outputBin);
        return nullptr;
    }

    GstPad* packetizerSinkPad = gst_element_get_static_pad(packetizer, "sink");
    GstPad* ghostSinkPad = packetizerSinkPad ? gst_ghost_pad_new("sink", packetizerSinkPad) : nullptr;
    if (packetizerSinkPad) {
        gst_object_unref(packetizerSinkPad);
    }
    if (!ghostSinkPad || !gst_element_add_pad(outputBin, ghostSinkPad)) {
        if (ghostSinkPad) {
            gst_object_unref(ghostSinkPad);
        }
        error = "failed to expose UDP output sink pad";
        gst_object_unref(outputBin);
        return nullptr;
    }

    g_object_set(packetizer,
        "alignment", kTsPacketsPerDatagram,
        "set-timestamps", TRUE,
        "smoothing-latency", kSmoothingLatencyUs,
        nullptr);
    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", static_cast<guint64>(3 * GST_SECOND),
        nullptr);
    g_object_set(pacer,
        "sync", TRUE,
        "sync-to-first", TRUE,
        "qos", FALSE,
        nullptr);

    const bool multicastOutput = isMulticastHost(config.outputHost);
    g_object_set(sink,
        "host", config.outputHost.c_str(),
        "port", config.outputPort,
        "async", FALSE,
        "sync", FALSE,
        "qos", FALSE,
        "max-lateness", static_cast<gint64>(-1),
        "processing-deadline", static_cast<guint64>(0),
        "enable-last-sample", FALSE,
        "buffer-size", kSocketBufferSize,
        nullptr);

    if (!config.interfaceAddress.empty() && !multicastOutput) {
        g_object_set(sink, "bind-address", config.interfaceAddress.c_str(), nullptr);
    }
    if (multicastOutput) {
        g_object_set(sink, "auto-multicast", TRUE, nullptr);
        if (!config.interfaceAddress.empty()) {
            const std::string iface = interfaceNameOrAddress(config.interfaceAddress);
            g_object_set(sink, "multicast-iface", iface.c_str(), nullptr);
        }
    }
    if (!gst_bin_add(GST_BIN(pipeline), outputBin)) {
        error = "failed to add UDP output module to pipeline";
        gst_object_unref(outputBin);
        return nullptr;
    }
    return outputBin;
}

} // namespace UdpOutput
