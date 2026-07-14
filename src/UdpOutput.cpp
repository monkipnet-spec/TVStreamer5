#include "UdpOutput.h"

#include <gst/app/gstappsink.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <regex>
#include <string>
#include <thread>
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
constexpr int kSocketBufferSize = 128 * 1024 * 1024;
constexpr int kMulticastTtl = 32;
constexpr uint64_t kPcrClockHz = 27000000ULL;
constexpr uint64_t kPcrWrap = (1ULL << 33) * 300ULL;
constexpr uint64_t kMinPacedBitrate = 512000ULL;
constexpr uint64_t kMaxPacedBitrate = 200000000ULL;
constexpr uint64_t kCbrSafetyHeadroomPercent = 105ULL;
constexpr uint64_t kVbrPaceHeadroomPercent = 110ULL;
constexpr uint64_t kPaceRiseLimitPercent = 125ULL;

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

bool hasProperty(GstElement* element, const char* propertyName) {
    return element && g_object_class_find_property(G_OBJECT_GET_CLASS(element), propertyName) != nullptr;
}

void setUInt64PropertyIfPresent(GstElement* element, const char* propertyName, guint64 value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

uint64_t clampPacedBitrate(uint64_t bitrate) {
    return std::clamp(bitrate, kMinPacedBitrate, kMaxPacedBitrate);
}

uint64_t pacedBitrateWithHeadroom(uint64_t bitrate, uint64_t headroomPercent) {
    const uint64_t capped = std::min(bitrate, kMaxPacedBitrate);
    return clampPacedBitrate(capped * headroomPercent / 100ULL);
}

class UdpTsSender {
public:
    UdpTsSender(const StreamConfig& cfg, std::string& error)
        : nextSend(std::chrono::steady_clock::now())
    {
        cbrPacing = cfg.cbr && cfg.targetBitrate > 0;
        if (cfg.targetBitrate > 0) {
            configuredBitrate = clampPacedBitrate(cfg.targetBitrate);
            pacedBitrate = configuredBitrate;
        }

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

            updatePacingFromDatagram(pending.data(), kUdpPayloadSize);
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

    static bool extractPcr(const guint8* packet, uint64_t& pcr) {
        if (!packet || packet[0] != 0x47) {
            return false;
        }

        const guint8 adaptationControl = (packet[3] >> 4) & 0x03;
        if (adaptationControl != 2 && adaptationControl != 3) {
            return false;
        }

        const guint8 adaptationLength = packet[4];
        if (adaptationLength < 7 || 5 + adaptationLength > kTsPacketSize) {
            return false;
        }

        const guint8 flags = packet[5];
        if ((flags & 0x10) == 0) {
            return false;
        }

        const uint64_t pcrBase =
            (static_cast<uint64_t>(packet[6]) << 25) |
            (static_cast<uint64_t>(packet[7]) << 17) |
            (static_cast<uint64_t>(packet[8]) << 9) |
            (static_cast<uint64_t>(packet[9]) << 1) |
            (static_cast<uint64_t>(packet[10]) >> 7);
        const uint64_t pcrExt =
            ((static_cast<uint64_t>(packet[10]) & 0x01) << 8) |
            static_cast<uint64_t>(packet[11]);
        pcr = pcrBase * 300ULL + pcrExt;
        return true;
    }

    void updatePacingFromDatagram(const guint8* data, std::size_t size) {
        for (std::size_t offset = 0; offset + kTsPacketSize <= size; offset += kTsPacketSize) {
            bytesSinceLastPcr += kTsPacketSize;

            uint64_t pcr = 0;
            if (!extractPcr(data + offset, pcr)) {
                continue;
            }

            if (haveLastPcr) {
                const uint64_t pcrDelta = pcr >= lastPcr
                    ? pcr - lastPcr
                    : kPcrWrap - lastPcr + pcr;
                if (pcrDelta > kPcrClockHz / 1000 && pcrDelta < kPcrClockHz * 2 && bytesSinceLastPcr > 0) {
                    const uint64_t estimatedBitrate = bytesSinceLastPcr * 8ULL * kPcrClockHz / pcrDelta;
                    updatePacedBitrate(estimatedBitrate);
                }
            }

            lastPcr = pcr;
            haveLastPcr = true;
            bytesSinceLastPcr = 0;
        }
    }

    void updatePacedBitrate(uint64_t estimatedBitrate) {
        if (estimatedBitrate < kMinPacedBitrate || estimatedBitrate > kMaxPacedBitrate) {
            return;
        }

        estimatedBitrate = pacedBitrateWithHeadroom(
            estimatedBitrate,
            cbrPacing ? kCbrSafetyHeadroomPercent : kVbrPaceHeadroomPercent);

        if (cbrPacing && configuredBitrate > 0) {
            const uint64_t cbrSafetyCeiling = pacedBitrateWithHeadroom(
                configuredBitrate,
                kCbrSafetyHeadroomPercent);
            if (estimatedBitrate <= cbrSafetyCeiling) {
                pacedBitrate = configuredBitrate;
                return;
            }
        }

        if (pacedBitrate == 0) {
            pacedBitrate = estimatedBitrate;
            return;
        }

        if (estimatedBitrate > pacedBitrate) {
            const uint64_t riseLimit = pacedBitrate * kPaceRiseLimitPercent / 100ULL;
            estimatedBitrate = std::min(estimatedBitrate, std::max(riseLimit, pacedBitrate + 1));
        }
        pacedBitrate = (pacedBitrate * 7ULL + estimatedBitrate * 3ULL) / 10ULL;
    }

    void sendDatagram(const guint8* data, std::size_t size) {
        pace(size);
        const auto* destination = reinterpret_cast<const sockaddr*>(&destinationAddress);
        const socklen_t destinationSize = sizeof(destinationAddress);
        ssize_t sent = ::sendto(socketFd, data, size, 0, destination, destinationSize);
        if (sent < 0) {
            std::cerr << "UDP output send failed: " << std::strerror(errno) << std::endl;
        }
    }

    void pace(std::size_t bytes) {
        if (pacedBitrate == 0) {
            return;
        }

        const uint64_t nanos = static_cast<uint64_t>(bytes) * 8ULL * 1000000000ULL / pacedBitrate;
        const auto interval = std::chrono::nanoseconds(nanos);
        const auto now = std::chrono::steady_clock::now();

        if (now > nextSend && now - nextSend > interval * 2) {
            nextSend = now;
        }

        if (nextSend > now) {
            const auto wait = nextSend - now;
            if (wait < std::chrono::milliseconds(50)) {
                std::this_thread::sleep_until(nextSend);
            } else {
                nextSend = now;
            }
        }

        nextSend += interval;
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
    bool cbrPacing = false;
    uint64_t configuredBitrate = 0;
    uint64_t pacedBitrate = 0;
    bool haveLastPcr = false;
    uint64_t lastPcr = 0;
    uint64_t bytesSinceLastPcr = 0;
    sockaddr_in destinationAddress {};
    std::vector<guint8> pending;
    std::chrono::steady_clock::time_point nextSend;
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
    setUInt64PropertyIfPresent(sink, "processing-deadline", 0);
    gst_caps_unref(caps);

    GstAppSinkCallbacks callbacks {};
    callbacks.new_sample = onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, sender, destroySender);
    return sink;
}

} // namespace UdpOutput
