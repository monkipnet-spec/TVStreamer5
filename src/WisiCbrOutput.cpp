#include "WisiCbrOutput.h"

#include <gst/app/gstappsink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <iterator>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <time.h>
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
constexpr std::size_t kMaxBufferedBytes = 16 * 1024 * 1024;
constexpr int kSocketBufferSize = 128 * 1024 * 1024;
constexpr int kMulticastTtl = 32;
constexpr uint64_t kPrebufferNanoseconds = 400ULL * 1000ULL * 1000ULL;
constexpr uint64_t kLateResetIntervals = 4ULL;

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

uint64_t monotonicNanoseconds() {
    timespec now {};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(now.tv_nsec);
}

timespec toTimespec(uint64_t nanoseconds) {
    timespec value {};
    value.tv_sec = static_cast<time_t>(nanoseconds / 1000000000ULL);
    value.tv_nsec = static_cast<long>(nanoseconds % 1000000000ULL);
    return value;
}

void sleepUntilMonotonic(uint64_t deadlineNanoseconds) {
    const timespec deadline = toTimespec(deadlineNanoseconds);
    int result = 0;
    do {
        result = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
    } while (result == EINTR);
}

struct TimedChunk {
    std::vector<guint8> bytes;
    uint64_t eligibleAtNanoseconds = 0;
};

class WisiCbrSender {
public:
    WisiCbrSender(const StreamConfig& cfg, std::string& error)
        : targetBitrate(cfg.targetBitrate) {
        if (targetBitrate == 0) {
            error = "WISI CBR target_bitrate must be greater than zero";
            return;
        }

        socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socketFd < 0) {
            error = std::string("failed to create WISI UDP socket: ") + std::strerror(errno);
            return;
        }

        int sendBufferSize = kSocketBufferSize;
        ::setsockopt(socketFd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, sizeof(sendBufferSize));

        const std::string outputHost = cfg.outputHost.empty() ? "127.0.0.1" : cfg.outputHost;
        destinationAddress.sin_family = AF_INET;
        destinationAddress.sin_port = htons(static_cast<uint16_t>(cfg.outputPort));
        if (::inet_pton(AF_INET, outputHost.c_str(), &destinationAddress.sin_addr) != 1) {
            error = "invalid WISI UDP output host: " + outputHost;
            closeSocket();
            return;
        }

        const bool multicastOutput = isMulticastHost(outputHost);
        if (!cfg.interfaceAddress.empty()) {
            const std::string ifaceAddress = interfaceAddressFor(cfg.interfaceAddress);
            in_addr localAddress {};
            if (::inet_pton(AF_INET, ifaceAddress.c_str(), &localAddress) != 1) {
                error = "invalid WISI UDP interface address: " + cfg.interfaceAddress;
                closeSocket();
                return;
            }

            if (multicastOutput) {
                if (::setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_IF,
                        &localAddress, sizeof(localAddress)) != 0) {
                    error = std::string("failed to set WISI multicast interface: ") +
                        std::strerror(errno);
                    closeSocket();
                    return;
                }
            } else {
                sockaddr_in bindAddress {};
                bindAddress.sin_family = AF_INET;
                bindAddress.sin_port = 0;
                bindAddress.sin_addr = localAddress;
                if (::bind(socketFd, reinterpret_cast<sockaddr*>(&bindAddress),
                        sizeof(bindAddress)) != 0) {
                    error = std::string("failed to bind WISI UDP interface: ") +
                        std::strerror(errno);
                    closeSocket();
                    return;
                }
            }
        }

        if (multicastOutput) {
            unsigned char ttl = kMulticastTtl;
            ::setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        }

        const uint64_t numerator = kUdpPayloadSize * 8ULL * 1000000000ULL;
        intervalNanoseconds = numerator / targetBitrate;
        intervalRemainder = numerator % targetBitrate;
        if (intervalNanoseconds == 0) {
            error = "WISI CBR target_bitrate is too high";
            closeSocket();
            return;
        }

        ready = true;
        senderThread = std::thread(&WisiCbrSender::sendLoop, this);
    }

    ~WisiCbrSender() {
        stopping.store(true, std::memory_order_relaxed);
        queueReady.notify_all();
        queueSpace.notify_all();
        if (senderThread.joinable()) {
            senderThread.join();
        }
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

        TimedChunk chunk;
        chunk.bytes.assign(map.data, map.data + map.size);
        gst_buffer_unmap(buffer, &map);
        if (chunk.bytes.empty()) {
            return GST_FLOW_OK;
        }

        // Keep a fixed sender-side de-jitter window. A real packet becomes
        // eligible only after this delay; the CBR clock fills empty slots with
        // null TS packets instead of pulling future video packets forward.
        chunk.eligibleAtNanoseconds = monotonicNanoseconds() + kPrebufferNanoseconds;

        std::unique_lock<std::mutex> lock(queueMutex);
        queueSpace.wait(lock, [&]() {
            return stopping.load(std::memory_order_relaxed) ||
                   bufferedBytes.load(std::memory_order_relaxed) + chunk.bytes.size() <= kMaxBufferedBytes;
        });
        if (stopping.load(std::memory_order_relaxed)) {
            return GST_FLOW_FLUSHING;
        }

        bufferedBytes.fetch_add(chunk.bytes.size(), std::memory_order_relaxed);
        queuedChunks.push_back(std::move(chunk));
        lock.unlock();
        queueReady.notify_one();
        return GST_FLOW_OK;
    }

private:
    void sendLoop() {
        uint64_t nextSendNanoseconds = 0;
        uint64_t scheduleRemainder = 0;

        while (!stopping.load(std::memory_order_relaxed)) {
            if (nextSendNanoseconds == 0) {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueReady.wait(lock, [&]() {
                    return stopping.load(std::memory_order_relaxed) || !queuedChunks.empty();
                });
                if (stopping.load(std::memory_order_relaxed)) {
                    break;
                }
                nextSendNanoseconds = queuedChunks.front().eligibleAtNanoseconds;
            }

            sleepUntilMonotonic(nextSendNanoseconds);
            if (stopping.load(std::memory_order_relaxed)) {
                break;
            }

            const uint64_t now = monotonicNanoseconds();
            const uint64_t lateResetThreshold = intervalNanoseconds * kLateResetIntervals;
            if (now > nextSendNanoseconds && now - nextSendNanoseconds > lateResetThreshold) {
                // Never compensate a scheduler stall with a burst. Resume the
                // CBR clock from "now" and keep the queued transport intact.
                nextSendNanoseconds = now;
                scheduleRemainder = 0;
            }

            moveEligibleChunks(nextSendNanoseconds);

            std::array<guint8, kUdpPayloadSize> datagram {};
            const std::size_t realPackets = fillDatagram(datagram.data());
            sendDatagram(datagram.data(), datagram.size());
            if (realPackets > 0) {
                queueSpace.notify_all();
            }

            nextSendNanoseconds += intervalNanoseconds;
            scheduleRemainder += intervalRemainder;
            if (scheduleRemainder >= targetBitrate) {
                nextSendNanoseconds += scheduleRemainder / targetBitrate;
                scheduleRemainder %= targetBitrate;
            }
        }
    }

    void moveEligibleChunks(uint64_t deadlineNanoseconds) {
        std::deque<TimedChunk> eligible;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            while (!queuedChunks.empty() &&
                   queuedChunks.front().eligibleAtNanoseconds <= deadlineNanoseconds) {
                eligible.push_back(std::move(queuedChunks.front()));
                queuedChunks.pop_front();
            }
        }

        while (!eligible.empty()) {
            auto& bytes = eligible.front().bytes;
            pending.insert(pending.end(), bytes.begin(), bytes.end());
            eligible.pop_front();
        }
        resyncPending();
    }

    std::size_t fillDatagram(guint8* destination) {
        if (!destination) {
            return 0;
        }

        resyncPending();
        std::size_t realPackets = 0;
        while (realPackets < kTsPacketsPerDatagram && pending.size() >= kTsPacketSize) {
            if (pending.front() != 0x47) {
                discardPendingByte();
                resyncPending();
                continue;
            }
            std::copy_n(pending.data(), kTsPacketSize,
                destination + realPackets * kTsPacketSize);
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(kTsPacketSize));
            bufferedBytes.fetch_sub(kTsPacketSize, std::memory_order_relaxed);
            ++realPackets;
        }

        for (std::size_t packetIndex = realPackets;
             packetIndex < kTsPacketsPerDatagram; ++packetIndex) {
            makeNullPacket(destination + packetIndex * kTsPacketSize);
        }
        return realPackets;
    }

    void makeNullPacket(guint8* packet) {
        packet[0] = 0x47;
        packet[1] = 0x1F;
        packet[2] = 0xFF;
        packet[3] = static_cast<guint8>(0x10 | (nullContinuityCounter & 0x0F));
        std::fill(packet + 4, packet + kTsPacketSize, 0xFF);
        nullContinuityCounter = static_cast<guint8>((nullContinuityCounter + 1) & 0x0F);
    }

    void resyncPending() {
        while (!pending.empty() && pending.front() != 0x47) {
            auto found = std::find(pending.begin() + 1, pending.end(), 0x47);
            const std::size_t discard = static_cast<std::size_t>(std::distance(pending.begin(), found));
            pending.erase(pending.begin(), found);
            bufferedBytes.fetch_sub(discard, std::memory_order_relaxed);
            queueSpace.notify_all();
        }
    }

    void discardPendingByte() {
        if (pending.empty()) {
            return;
        }
        pending.erase(pending.begin());
        bufferedBytes.fetch_sub(1, std::memory_order_relaxed);
    }

    void sendDatagram(const guint8* data, std::size_t size) {
        const auto* destination = reinterpret_cast<const sockaddr*>(&destinationAddress);
        const socklen_t destinationSize = sizeof(destinationAddress);
        const ssize_t sent = ::sendto(socketFd, data, size, 0, destination, destinationSize);
        if (sent < 0) {
            std::cerr << "WISI UDP send failed: " << std::strerror(errno) << std::endl;
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
    uint64_t targetBitrate = 0;
    uint64_t intervalNanoseconds = 0;
    uint64_t intervalRemainder = 0;
    sockaddr_in destinationAddress {};

    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> bufferedBytes{0};
    std::thread senderThread;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::condition_variable queueSpace;
    std::deque<TimedChunk> queuedChunks;
    std::vector<guint8> pending;
    guint8 nullContinuityCounter = 0;
};

GstFlowReturn onNewSample(GstAppSink* sink, gpointer userData) {
    auto* sender = static_cast<WisiCbrSender*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    const GstFlowReturn result = sender ? sender->pushBuffer(buffer) : GST_FLOW_ERROR;
    gst_sample_unref(sample);
    return result;
}

void destroySender(gpointer data) {
    delete static_cast<WisiCbrSender*>(data);
}

} // namespace

namespace WisiCbrOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const std::string& sinkName,
    std::string& error) {
    if (config.targetBitrate == 0) {
        error = "WISI CBR target_bitrate must be greater than zero";
        return nullptr;
    }

    GstElement* sink = gst_element_factory_make(
        "appsink",
        sinkName.empty() ? "wisi_output_sink" : sinkName.c_str());
    if (!sink || !gst_bin_add(GST_BIN(pipeline), sink)) {
        if (sink) {
            gst_object_unref(sink);
        }
        error = "failed to create WISI CBR appsink";
        return nullptr;
    }

    auto* sender = new WisiCbrSender(config, error);
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

    std::cerr << "WISI compatibility TS shaper: target_bitrate=" << config.targetBitrate
              << " packetization=7x188 prebuffer_ms=400"
              << " null_pid=0x1fff clock=clock_nanosleep-abstime busywait=off"
              << std::endl;
    return sink;
}

} // namespace WisiCbrOutput
