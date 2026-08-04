#include "UdpInput.h"

#include <iostream>
#include <regex>
#include <unordered_set>

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

std::string allInterfaceNames() {
    std::string names;
    std::unordered_set<std::string> added;
    for (const auto& iface : enumerateNetworkInterfaces()) {
        if (iface.name.empty() || !iface.isUp || !iface.supportsMulticast ||
            !added.insert(iface.name).second) {
            continue;
        }
        if (!names.empty()) {
            names += ',';
        }
        names += iface.name;
    }
    return names;
}

std::string effectiveInputInterfaceAddress(
    const StreamConfig& config,
    bool multicastInput,
    bool wildcardUriHost) {
    if (config.inputInterfaceAddressConfigured) {
        return config.inputInterfaceAddress;
    }

    // Configs created before input_interface_address existed used the output
    // interface for multicast and udp://@:port. An explicitly configured empty
    // input interface now means "listen on all interfaces" and must not fall
    // back. An explicit unicast URI is safely received on all local addresses.
    return (multicastInput || wildcardUriHost) ? config.interfaceAddress : "";
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

    int port = 0;
    try {
        port = std::stoi(match[3].str());
    } catch (...) {
        error = "invalid UDP/RTP input port";
        return nullptr;
    }
    if (port <= 0 || port > 65535) {
        error = "UDP/RTP input port is out of range";
        return nullptr;
    }
    const std::string uriHost = match[2].str();
    const bool multicastInput = isMulticastHost(uriHost);
    const bool wildcardUriHost = uriHost.empty() || uriHost == "0.0.0.0";
    const std::string inputInterfaceAddress =
        effectiveInputInterfaceAddress(config, multicastInput, wildcardUriHost);

    // For multicast, udpsrc must receive the group address so it can join it.
    // A unicast URI host is commonly the sender/destination address (FFmpeg and
    // VLC syntax), and binding a receiving socket to that remote address fails
    // with EADDRNOTAVAIL. Always bind unicast to a selected local interface or
    // to all local interfaces instead.
    const std::string listenAddress = multicastInput
        ? uriHost
        : (inputInterfaceAddress.empty() ? "0.0.0.0" : inputInterfaceAddress);

    g_object_set(src,
        "address", listenAddress.c_str(),
        "port", port,
        "reuse", TRUE,
        "auto-multicast", multicastInput ? TRUE : FALSE,
        "do-timestamp", FALSE,
        "buffer-size", kSocketBufferSize,
        nullptr);

    std::string multicastInterfaces;
    if (multicastInput) {
        // udpsrc otherwise joins the group only on the interface selected by
        // the kernel's multicast route. Its multicast-iface property accepts
        // a comma-separated list, so the UI's "all interfaces" option must
        // explicitly pass every available interface.
        multicastInterfaces = inputInterfaceAddress.empty()
            ? allInterfaceNames()
            : interfaceNameOrAddress(inputInterfaceAddress);
        if (!multicastInterfaces.empty()) {
            g_object_set(src, "multicast-iface", multicastInterfaces.c_str(), nullptr);
        }
    }

    std::cerr << "UDP input: protocol=" << toLower(match[1].str())
              << " uri_host=" << (uriHost.empty() ? "@" : uriHost)
              << " listen=" << listenAddress << ":" << port;
    if (multicastInput) {
        std::cerr << " multicast_iface="
                  << (multicastInterfaces.empty() ? "route-default" : multicastInterfaces);
    }
    std::cerr << std::endl;

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
        // A UDP datagram commonly contains seven TS packets. Do not force a
        // buffer packet size or replace the transport stream's PCR/PTS clock.
        GstCaps* caps = gst_caps_from_string("video/mpegts,systemstream=(boolean)true");
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
