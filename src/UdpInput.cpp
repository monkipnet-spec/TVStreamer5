#include "UdpInput.h"

#include <regex>

#include "utils.h"

namespace {

constexpr gint kSocketBufferSize = 16 * 1024 * 1024;

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

void configureQueue(GstElement* queue) {
    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", static_cast<guint64>(3 * GST_SECOND),
        nullptr);
}

} // namespace

namespace UdpInput {

bool handles(const std::string& uri) {
    const std::string lower = toLower(uri);
    return lower.rfind("udp://", 0) == 0 || lower.rfind("rtp://", 0) == 0;
}

GstElement* build(
    GstElement* pipeline,
    const StreamConfig& config,
    GstElement*& terminalElement,
    std::string& error) {
    terminalElement = nullptr;
    std::regex uriPattern(R"(^(udp|rtp)://@?([^:/]*):(\d+).*$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_match(config.inputUri, match, uriPattern) || match.size() < 4) {
        error = "invalid UDP/RTP input URI";
        return nullptr;
    }

    GstElement* src = gst_element_factory_make("udpsrc", "input_src");
    GstElement* queue = gst_element_factory_make("queue", "input_queue");
    if (!src || !queue || !gst_bin_add(GST_BIN(pipeline), src) || !gst_bin_add(GST_BIN(pipeline), queue)) {
        error = "failed to create UDP input elements";
        return nullptr;
    }
    configureQueue(queue);

    const int port = std::stoi(match[3].str());
    std::string host = match[2].str();
    if (host.empty() || host == "@") {
        host = "0.0.0.0";
    }

    g_object_set(src,
        "address", host.c_str(),
        "port", port,
        "reuse", TRUE,
        "do-timestamp", TRUE,
        "buffer-size", kSocketBufferSize,
        nullptr);

    if (isMulticastHost(host) && !config.interfaceAddress.empty()) {
        const std::string iface = interfaceNameOrAddress(config.interfaceAddress);
        g_object_set(src, "multicast-iface", iface.c_str(), nullptr);
    }

    if (toLower(match[1].str()) == "rtp") {
        GstElement* depay = gst_element_factory_make("rtpmp2tdepay", "rtp_depay");
        if (!depay || !gst_bin_add(GST_BIN(pipeline), depay)) {
            error = "failed to create RTP depayloader";
            return nullptr;
        }

        GstCaps* caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=MP2T,clock-rate=90000");
        g_object_set(src, "caps", caps, nullptr);
        gst_caps_unref(caps);
        if (!gst_element_link_many(src, depay, queue, nullptr)) {
            error = "failed to link RTP input";
            return nullptr;
        }
    } else {
        GstCaps* caps = gst_caps_from_string("video/mpegts,systemstream=(boolean)true,packetsize=(int)188");
        g_object_set(src, "caps", caps, nullptr);
        gst_caps_unref(caps);
        if (!gst_element_link(src, queue)) {
            error = "failed to link UDP input";
            return nullptr;
        }
    }

    terminalElement = queue;
    return src;
}

} // namespace UdpInput
