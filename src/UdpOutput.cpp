#include "UdpOutput.h"

#include <gst/app/gstappsink.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <regex>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "utils.h"

namespace {

constexpr std::size_t kTsPacketSize = 188;
constexpr std::size_t kTsPacketsPerDatagram = 7;
constexpr std::size_t kUdpPayloadSize = kTsPacketSize * kTsPacketsPerDatagram;
constexpr int kSocketBufferSize = 64 * 1024 * 1024;
constexpr int kMulticastTtl = 32;

bool isMulticastHost(const std::string& host) {
    static const std::regex pattern(R"(^((22[4-9])|(23[0-9]))\.)");
    return std::regex_search(host, pattern);
}

std::string interfaceAddressFor(const std::string& address) {
    for (const auto& iface : enumerateNetworkInterfaces()) {
        if (iface.name == address || iface.address == address) {
            return iface.address;
        }
    }
    return address;
}

class UdpTsSender {
public:
    UdpTsSender(const StreamConfig& cfg, std::string& error)
    {
        socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socketFd < 0) {
            error = std::string("failed to create UDP socket: ") + std::strerror(errno);
            return;
        }

        int sendBufferSize = kSocketBufferSize;
        ::setsockopt(socketFd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, sizeof(sendBufferSize));

        sockaddr_in destination {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(static_cast<uint16_t>(cfg.outputPort));
        if (::inet_pton(AF_INET, cfg.outputHost.c_str(), &destination.sin_addr) != 1) {
            error = "invalid UDP output host: " + cfg.outputHost;
            closeSocket();
            return;
        }
        destinationAddress = destination;

        const bool multicastOutput = isMulticastHost(cfg.outputHost);
        if (!cfg.interfaceAddress.empty()) {
            const std::string ifaceAddress = interfaceAddressFor(cfg.interfaceAddress);
            in_addr localAddress {};
            if (::inet_pton(AF_INET, ifaceAddress.c_str(), &localAddress) != 1) {
                error = "invalid UDP interface address: " + cfg.interfaceAddress;
                closeSocket();
                return;
            }

            if (multicastOutput) {
                if (::setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_IF, &localAddress, sizeof(localAddress)) != 0) {
                    error = std::string("failed to set multicast interface: ") + std::strerror(errno);
                    closeSocket();
                    return;
                }
            } else {
                sockaddr_in bindAddress {};
                bindAddress.sin_family = AF_INET;
                bindAddress.sin_port = 0;
                bindAddress.sin_addr = localAddress;
                if (::bind(socketFd, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
                    error = std::string("failed to bind UDP output interface: ") + std::strerror(errno);
                    closeSocket();
                    return;
                }
            }
        }

        if (multicastOutput) {
            unsigned char ttl = kMulticastTtl;
            ::setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        }

        ready = true;
    }

    ~UdpTsSender() {
        closeSocket();
    }

    bool isReady() const {
        return ready;
    }

    GstFlowReturn pushBuffer(GstBuffer* buffer) {
        if (!ready || !buffer) {
            return GST_FLOW_ERROR;
        }

        GstMapInfo map {};
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            return GST_FLOW_ERROR;
        }

        appendAndSend(map.data, map.size);
        gst_buffer_unmap(buffer, &map);
        return GST_FLOW_OK;
    }

private:
    void appendAndSend(const guint8* data, std::size_t size) {
        if (!data || size == 0) {
            return;
        }

        pending.insert(pending.end(), data, data + size);
        resyncPending();

        while (pending.size() >= kUdpPayloadSize) {
            if (!isAlignedDatagram(pending.data())) {
                pending.erase(pending.begin());
                resyncPending();
                continue;
            }

            sendDatagram(pending.data(), kUdpPayloadSize);
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(kUdpPayloadSize));
        }
    }

    void resyncPending() {
        while (pending.size() >= kTsPacketSize && pending.front() != 0x47) {
            auto found = std::find(pending.begin() + 1, pending.end(), 0x47);
            pending.erase(pending.begin(), found);
        }
    }

    bool isAlignedDatagram(const guint8* data) const {
        for (std::size_t offset = 0; offset < kUdpPayloadSize; offset += kTsPacketSize) {
            if (data[offset] != 0x47) {
                return false;
            }
        }
        return true;
    }

    void sendDatagram(const guint8* data, std::size_t size) {
        const auto* destination = reinterpret_cast<const sockaddr*>(&destinationAddress);
        const socklen_t destinationSize = sizeof(destinationAddress);
        ssize_t sent = ::sendto(socketFd, data, size, 0, destination, destinationSize);
        if (sent < 0) {
            std::cerr << "UDP output send failed: " << std::strerror(errno) << std::endl;
        }
    }

    void closeSocket() {
        if (socketFd >= 0) {
            ::close(socketFd);
            socketFd = -1;
        }
        ready = false;
    }

    int socketFd = -1;
    bool ready = false;
    sockaddr_in destinationAddress {};
    std::vector<guint8> pending;
};

GstFlowReturn onNewSample(GstAppSink* sink, gpointer userData) {
    auto* sender = static_cast<UdpTsSender*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstFlowReturn result = sender ? sender->pushBuffer(buffer) : GST_FLOW_ERROR;
    gst_sample_unref(sample);
    return result;
}

void destroySender(gpointer data) {
    delete static_cast<UdpTsSender*>(data);
}

} // namespace

namespace UdpOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    std::string& error) {
    GstElement* sink = gst_element_factory_make("appsink", "output_sink");
    if (!sink || !gst_bin_add(GST_BIN(pipeline), sink)) {
        if (sink) {
            gst_object_unref(sink);
        }
        error = "failed to create UDP appsink";
        return nullptr;
    }

    auto* sender = new UdpTsSender(config, error);
    if (!sender->isReady()) {
        delete sender;
        gst_bin_remove(GST_BIN(pipeline), sink);
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string("video/mpegts,systemstream=(boolean)true");
    g_object_set(sink,
        "caps", caps,
        "emit-signals", FALSE,
        "sync", FALSE,
        "async", FALSE,
        "qos", FALSE,
        "max-lateness", static_cast<gint64>(-1),
        "enable-last-sample", FALSE,
        "drop", FALSE,
        "max-buffers", static_cast<guint>(0),
        nullptr);
    gst_caps_unref(caps);

    GstAppSinkCallbacks callbacks {};
    callbacks.new_sample = onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, sender, destroySender);
    return sink;
}

} // namespace UdpOutput
