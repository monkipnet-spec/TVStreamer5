#include "UdpOutput.h"

#include <regex>

#include "utils.h"

namespace {

constexpr gint kSocketBufferSize = 64 * 1024 * 1024;

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
    GstElement* sink = gst_element_factory_make("udpsink", "output_sink");
    if (!sink || !gst_bin_add(GST_BIN(pipeline), sink)) {
        if (sink) {
            gst_object_unref(sink);
        }
        error = "failed to create UDP output sink";
        return nullptr;
    }

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
    return sink;
}

} // namespace UdpOutput
