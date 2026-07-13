#include "UdpOutput.h"

#include <arpa/inet.h>

#include "utils.h"

namespace {

constexpr guint kTsPacketsPerDatagram = 7;
constexpr guint kSmoothingLatencyUs = 100'000;
constexpr gint kSocketBufferSize = 16 * 1024 * 1024;
constexpr guint64 kQueueLatency = 2 * GST_SECOND;

bool hasFactory(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

bool isMulticast(const std::string& host) {
    in_addr address{};
    return inet_pton(AF_INET, host.c_str(), &address) == 1 && IN_MULTICAST(ntohl(address.s_addr));
}

std::string interfaceName(const std::string& address) {
    for (const auto& iface : enumerateNetworkInterfaces()) {
        if (iface.address == address) {
            return iface.name;
        }
    }
    return address;
}

} // namespace

namespace UdpOutput {

bool build(
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& config,
    std::string& error) {
    if (!pipeline || !sourceTail) {
        error = "invalid UDP pipeline";
        return false;
    }
    if (!hasFactory("tsparse") || !hasFactory("queue") || !hasFactory("udpsink")) {
        error = "missing UDP elements: tsparse, queue or udpsink";
        return false;
    }
    if (config.outputHost.empty() || config.outputPort <= 0 || config.outputPort > 65535) {
        error = "invalid UDP destination";
        return false;
    }

    GstElement* packetizer = gst_element_factory_make("tsparse", "udp_packetizer");
    GstElement* queue = gst_element_factory_make("queue", "output_queue");
    GstElement* sink = gst_element_factory_make("udpsink", "output_sink");
    if (!packetizer || !queue || !sink ||
        !gst_bin_add(GST_BIN(pipeline), packetizer) ||
        !gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), sink)) {
        error = "failed to create UDP output elements";
        return false;
    }

    g_object_set(packetizer,
        "alignment", kTsPacketsPerDatagram,
        "set-timestamps", TRUE,
        "smoothing-latency", kSmoothingLatencyUs,
        nullptr);
    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", kQueueLatency,
        nullptr);
    g_object_set(sink,
        "host", config.outputHost.c_str(),
        "port", config.outputPort,
        "sync", TRUE,
        "async", FALSE,
        "qos", FALSE,
        "max-lateness", static_cast<gint64>(-1),
        "buffer-size", kSocketBufferSize,
        nullptr);

    const bool multicast = isMulticast(config.outputHost);
    if (multicast) {
        g_object_set(sink, "auto-multicast", TRUE, nullptr);
        if (!config.interfaceAddress.empty()) {
            const std::string iface = interfaceName(config.interfaceAddress);
            g_object_set(sink, "multicast-iface", iface.c_str(), nullptr);
        }
    } else if (!config.interfaceAddress.empty()) {
        g_object_set(sink, "bind-address", config.interfaceAddress.c_str(), nullptr);
    }

    if (!gst_element_link_many(sourceTail, packetizer, queue, sink, nullptr)) {
        error = "failed to link UDP output pipeline";
        return false;
    }
    return true;
}

} // namespace UdpOutput
