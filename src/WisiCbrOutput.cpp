#include "WisiCbrOutput.h"

#include <gst/app/gstappsink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <time.h>
#include <utility>
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
constexpr uint64_t kPcrClockHz = 27000000ULL;
constexpr uint64_t kPcrBaseModulus = (1ULL << 33);
constexpr uint64_t kPcrTicksModulus = kPcrBaseModulus * 300ULL;
constexpr uint64_t kMaxReasonablePcrGapTicks = 2ULL * kPcrClockHz;
constexpr uint64_t kStatsIntervalNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;

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

uint64_t multiplyDivide(uint64_t value, uint64_t multiplier, uint64_t divisor) {
    if (divisor == 0) {
        return 0;
    }
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 product =
        static_cast<unsigned __int128>(value) * static_cast<unsigned __int128>(multiplier);
    return static_cast<uint64_t>(product / divisor);
#else
    const long double product = static_cast<long double>(value) *
        static_cast<long double>(multiplier);
    return static_cast<uint64_t>(product / static_cast<long double>(divisor));
#endif
}

uint64_t pcrTicksToNanoseconds(uint64_t ticks) {
    return multiplyDivide(ticks, 1000000000ULL, kPcrClockHz);
}

uint64_t nanosecondsToPcrTicks(uint64_t nanoseconds) {
    return multiplyDivide(nanoseconds, kPcrClockHz, 1000000000ULL);
}

uint64_t forwardPcrDelta(uint64_t from, uint64_t to) {
    from %= kPcrTicksModulus;
    to %= kPcrTicksModulus;
    return to >= from ? to - from : (kPcrTicksModulus - from) + to;
}

struct TimedChunk {
    std::vector<guint8> bytes;
    uint64_t eligibleAtNanoseconds = 0;
};

struct TsPacket {
    std::array<guint8, kTsPacketSize> bytes {};
    bool hasPcr = false;
    bool discontinuity = false;
    uint64_t pcrTicks = 0;
};

struct ScheduledPacket {
    TsPacket packet;
    uint64_t dueNanoseconds = 0;
};

bool parsePcr(const std::array<guint8, kTsPacketSize>& packet,
              uint64_t& pcrTicks,
              bool& discontinuity) {
    pcrTicks = 0;
    discontinuity = false;
    if (packet[0] != 0x47) {
        return false;
    }

    const guint8 adaptationFieldControl = static_cast<guint8>((packet[3] >> 4) & 0x03);
    if (adaptationFieldControl != 2 && adaptationFieldControl != 3) {
        return false;
    }

    const std::size_t adaptationLength = packet[4];
    if (adaptationLength < 1 || 5 + adaptationLength > kTsPacketSize) {
        return false;
    }

    const guint8 flags = packet[5];
    discontinuity = (flags & 0x80) != 0;
    if ((flags & 0x10) == 0 || adaptationLength < 7) {
        return false;
    }

    const guint64 base =
        (static_cast<guint64>(packet[6]) << 25) |
        (static_cast<guint64>(packet[7]) << 17) |
        (static_cast<guint64>(packet[8]) << 9) |
        (static_cast<guint64>(packet[9]) << 1) |
        (static_cast<guint64>(packet[10]) >> 7);
    const guint64 extension =
        (static_cast<guint64>(packet[10] & 0x01) << 8) |
        static_cast<guint64>(packet[11]);
    pcrTicks = (base * 300ULL + extension) % kPcrTicksModulus;
    return true;
}

void writePcr(std::array<guint8, kTsPacketSize>& packet, uint64_t pcrTicks) {
    pcrTicks %= kPcrTicksModulus;
    const uint64_t base = pcrTicks / 300ULL;
    const uint64_t extension = pcrTicks % 300ULL;

    packet[6] = static_cast<guint8>((base >> 25) & 0xFF);
    packet[7] = static_cast<guint8>((base >> 17) & 0xFF);
    packet[8] = static_cast<guint8>((base >> 9) & 0xFF);
    packet[9] = static_cast<guint8>((base >> 1) & 0xFF);
    packet[10] = static_cast<guint8>(((base & 0x01) << 7) | 0x7E |
        ((extension >> 8) & 0x01));
    packet[11] = static_cast<guint8>(extension & 0xFF);
}

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

        const uint64_t datagramNumerator = kUdpPayloadSize * 8ULL * 1000000000ULL;
        intervalNanoseconds = datagramNumerator / targetBitrate;
        intervalRemainder = datagramNumerator % targetBitrate;
        packetIntervalNanoseconds =
            multiplyDivide(kTsPacketSize * 8ULL, 1000000000ULL, targetBitrate);
        if (intervalNanoseconds == 0 || packetIntervalNanoseconds == 0) {
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

        // Fixed sender-side de-jitter delay. Unlike v77, the packets are not
        // emitted simply because they became eligible. After this delay the
        // PCR scheduler decides their real CBR slot.
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
                // Do not start the UDP clock until at least two PCR anchors are
                // available after the 400 ms de-jitter delay. This prevents a
                // startup burst followed by null-only gaps.
                if (!waitForInitialSchedule()) {
                    break;
                }
                nextSendNanoseconds = monotonicNanoseconds();
                if (!scheduledPackets.empty()) {
                    const uint64_t firstDue = scheduledPackets.front().dueNanoseconds;
                    if (firstDue > nextSendNanoseconds) {
                        nextSendNanoseconds = firstDue;
                    }
                }
                statsStartedNanoseconds = nextSendNanoseconds;
                lastStatsNanoseconds = nextSendNanoseconds;
            }

            sleepUntilMonotonic(nextSendNanoseconds);
            if (stopping.load(std::memory_order_relaxed)) {
                break;
            }

            const uint64_t now = monotonicNanoseconds();
            const uint64_t lateResetThreshold = intervalNanoseconds * kLateResetIntervals;
            if (now > nextSendNanoseconds && now - nextSendNanoseconds > lateResetThreshold) {
                // Do not compensate a scheduler stall with an Ethernet burst.
                // Re-anchor only the wall-clock send cadence; PCR is restamped
                // from the actual output clock below.
                nextSendNanoseconds = now;
                scheduleRemainder = 0;
                ++schedulerResets;
            }

            moveEligibleChunks(nextSendNanoseconds);
            scheduleAvailablePackets(nextSendNanoseconds);

            std::array<guint8, kUdpPayloadSize> datagram {};
            const std::size_t realPackets = fillDatagram(datagram.data(), nextSendNanoseconds);
            sendDatagram(datagram.data(), datagram.size());
            totalDatagrams.fetch_add(1, std::memory_order_relaxed);
            totalRealPackets.fetch_add(realPackets, std::memory_order_relaxed);
            totalNullPackets.fetch_add(kTsPacketsPerDatagram - realPackets, std::memory_order_relaxed);
            if (realPackets > 0) {
                queueSpace.notify_all();
            }

            maybeLogStats(now);

            nextSendNanoseconds += intervalNanoseconds;
            scheduleRemainder += intervalRemainder;
            if (scheduleRemainder >= targetBitrate) {
                nextSendNanoseconds += scheduleRemainder / targetBitrate;
                scheduleRemainder %= targetBitrate;
            }
        }
    }

    bool waitForInitialSchedule() {
        while (!stopping.load(std::memory_order_relaxed)) {
            const uint64_t now = monotonicNanoseconds();
            moveEligibleChunks(now);
            if (scheduleAvailablePackets(now) && !scheduledPackets.empty()) {
                return true;
            }

            std::unique_lock<std::mutex> lock(queueMutex);
            if (queuedChunks.empty()) {
                queueReady.wait_for(lock, std::chrono::milliseconds(10), [&]() {
                    return stopping.load(std::memory_order_relaxed) || !queuedChunks.empty();
                });
            } else {
                const uint64_t eligibleAt = queuedChunks.front().eligibleAtNanoseconds;
                lock.unlock();
                const uint64_t current = monotonicNanoseconds();
                if (eligibleAt > current) {
                    sleepUntilMonotonic(std::min<uint64_t>(eligibleAt, current + 10ULL * 1000ULL * 1000ULL));
                } else {
                    sleepUntilMonotonic(current + 1000000ULL);
                }
                continue;
            }
        }
        return false;
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
            parserBytes.insert(parserBytes.end(), bytes.begin(), bytes.end());
            eligible.pop_front();
        }
        parseAvailableTsPackets();
    }

    void parseAvailableTsPackets() {
        while (parserBytes.size() - parserOffset >= kTsPacketSize) {
            if (parserBytes[parserOffset] != 0x47) {
                ++parserOffset;
                bufferedBytes.fetch_sub(1, std::memory_order_relaxed);
                ++resyncDiscardedBytes;
                queueSpace.notify_all();
                continue;
            }

            // If another complete packet is already available, verify the next
            // sync byte before committing to this alignment.
            if (parserBytes.size() - parserOffset >= kTsPacketSize * 2 &&
                parserBytes[parserOffset + kTsPacketSize] != 0x47) {
                ++parserOffset;
                bufferedBytes.fetch_sub(1, std::memory_order_relaxed);
                ++resyncDiscardedBytes;
                queueSpace.notify_all();
                continue;
            }

            TsPacket packet;
            std::copy_n(parserBytes.data() + parserOffset, kTsPacketSize, packet.bytes.data());
            packet.hasPcr = parsePcr(packet.bytes, packet.pcrTicks, packet.discontinuity);
            unscheduledPackets.push_back(std::move(packet));
            parserOffset += kTsPacketSize;
        }

        if (parserOffset > 0 &&
            (parserOffset >= 64 * 1024 || parserOffset * 2 >= parserBytes.size())) {
            parserBytes.erase(parserBytes.begin(),
                parserBytes.begin() + static_cast<std::ptrdiff_t>(parserOffset));
            parserOffset = 0;
        }
    }

    bool scheduleAvailablePackets(uint64_t nowNanoseconds) {
        bool scheduledAnything = false;

        while (!stopping.load(std::memory_order_relaxed)) {
            if (!timelineInitialized) {
                const auto anchors = findFirstTwoPcrAnchors();
                if (anchors.first == kNoIndex || anchors.second == kNoIndex) {
                    break;
                }

                const std::size_t firstIndex = anchors.first;
                const std::size_t secondIndex = anchors.second;
                const TsPacket first = unscheduledPackets[firstIndex];
                const TsPacket second = unscheduledPackets[secondIndex];
                const uint64_t deltaTicks = forwardPcrDelta(first.pcrTicks, second.pcrTicks);
                if (!isUsablePcrDelta(first, second, deltaTicks, secondIndex - firstIndex)) {
                    // Keep moving until a sane pair is found. A discontinuity
                    // marker is a valid new origin, so drop only the unusable
                    // prefix from the scheduling model, not from the TS output.
                    schedulePrefixAtTransportRate(nowNanoseconds, secondIndex);
                    scheduledAnything = true;
                    continue;
                }

                const uint64_t deltaNs = pcrTicksToNanoseconds(deltaTicks);
                const std::size_t packetDistance = secondIndex - firstIndex;
                const uint64_t sourcePacketSpacing = std::max<uint64_t>(1, deltaNs / packetDistance);
                const uint64_t startNs = std::max<uint64_t>(nowNanoseconds, monotonicNanoseconds());
                const uint64_t firstAnchorDue = startNs + firstIndex * sourcePacketSpacing;
                const uint64_t secondAnchorDue = firstAnchorDue + deltaNs;

                for (std::size_t index = 0; index < secondIndex; ++index) {
                    uint64_t due = startNs;
                    if (index <= firstIndex) {
                        due = startNs + index * sourcePacketSpacing;
                    } else {
                        due = firstAnchorDue + multiplyDivide(
                            index - firstIndex, deltaNs, packetDistance);
                    }
                    moveFrontToScheduled(due);
                }

                anchorPcrTicks = second.pcrTicks;
                anchorDueNanoseconds = secondAnchorDue;
                timelineInitialized = true;
                scheduledAnything = true;
                updateSegmentPeak(packetDistance, deltaTicks);
                continue;
            }

            if (unscheduledPackets.empty()) {
                break;
            }

            // The previous pass always retains its second PCR anchor at the
            // front. If a malformed/discontinuous sequence breaks that
            // invariant, safely restart the source-PCR scheduler.
            if (!unscheduledPackets.front().hasPcr) {
                timelineInitialized = false;
                continue;
            }

            const std::size_t nextPcrIndex = findNextPcrIndex(1);
            if (nextPcrIndex == kNoIndex) {
                break;
            }

            const TsPacket first = unscheduledPackets.front();
            const TsPacket second = unscheduledPackets[nextPcrIndex];
            const uint64_t deltaTicks = forwardPcrDelta(first.pcrTicks, second.pcrTicks);
            if (!isUsablePcrDelta(first, second, deltaTicks, nextPcrIndex)) {
                // The TS explicitly changed clock or the PCR gap is implausible.
                // Preserve every packet, but bridge this one segment at the
                // configured transport rate. PCR restamping will establish a
                // clean new output clock on the discontinuity packet.
                const uint64_t start = std::max<uint64_t>(anchorDueNanoseconds, nowNanoseconds);
                for (std::size_t index = 0; index < nextPcrIndex; ++index) {
                    moveFrontToScheduled(start + index * packetIntervalNanoseconds);
                }
                anchorPcrTicks = second.pcrTicks;
                anchorDueNanoseconds = start + nextPcrIndex * packetIntervalNanoseconds;
                ++pcrTimelineResets;
                scheduledAnything = true;
                continue;
            }

            const uint64_t deltaNs = pcrTicksToNanoseconds(deltaTicks);
            const uint64_t secondAnchorDue = anchorDueNanoseconds + deltaNs;
            for (std::size_t index = 0; index < nextPcrIndex; ++index) {
                const uint64_t due = anchorDueNanoseconds +
                    multiplyDivide(index, deltaNs, nextPcrIndex);
                moveFrontToScheduled(due);
            }
            anchorPcrTicks = second.pcrTicks;
            anchorDueNanoseconds = secondAnchorDue;
            scheduledAnything = true;
            updateSegmentPeak(nextPcrIndex, deltaTicks);
        }

        return scheduledAnything;
    }

    static constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

    std::pair<std::size_t, std::size_t> findFirstTwoPcrAnchors() const {
        std::size_t first = kNoIndex;
        for (std::size_t index = 0; index < unscheduledPackets.size(); ++index) {
            if (!unscheduledPackets[index].hasPcr) {
                continue;
            }
            if (first == kNoIndex) {
                first = index;
            } else {
                return {first, index};
            }
        }
        return {kNoIndex, kNoIndex};
    }

    std::size_t findNextPcrIndex(std::size_t startIndex) const {
        for (std::size_t index = startIndex; index < unscheduledPackets.size(); ++index) {
            if (unscheduledPackets[index].hasPcr) {
                return index;
            }
        }
        return kNoIndex;
    }

    bool isUsablePcrDelta(const TsPacket& first,
                          const TsPacket& second,
                          uint64_t deltaTicks,
                          std::size_t packetDistance) const {
        if (packetDistance == 0 || deltaTicks == 0 || deltaTicks > kMaxReasonablePcrGapTicks) {
            return false;
        }
        if (second.discontinuity) {
            return false;
        }
        (void)first;
        return true;
    }

    void schedulePrefixAtTransportRate(uint64_t nowNanoseconds, std::size_t count) {
        const uint64_t start = std::max<uint64_t>(nowNanoseconds, monotonicNanoseconds());
        for (std::size_t index = 0; index < count && !unscheduledPackets.empty(); ++index) {
            moveFrontToScheduled(start + index * packetIntervalNanoseconds);
        }
        timelineInitialized = false;
        ++pcrTimelineResets;
    }

    void moveFrontToScheduled(uint64_t dueNanoseconds) {
        if (unscheduledPackets.empty()) {
            return;
        }
        ScheduledPacket scheduled;
        scheduled.packet = std::move(unscheduledPackets.front());
        scheduled.dueNanoseconds = dueNanoseconds;
        unscheduledPackets.pop_front();
        scheduledPackets.push_back(std::move(scheduled));
    }

    void updateSegmentPeak(std::size_t packetDistance, uint64_t deltaTicks) {
        if (packetDistance == 0 || deltaTicks == 0) {
            return;
        }
        const uint64_t bits = static_cast<uint64_t>(packetDistance) * kTsPacketSize * 8ULL;
        const uint64_t segmentBitrate = multiplyDivide(bits, kPcrClockHz, deltaTicks);
        uint64_t previous = peakPcrSegmentBitrate.load(std::memory_order_relaxed);
        while (segmentBitrate > previous &&
               !peakPcrSegmentBitrate.compare_exchange_weak(
                   previous, segmentBitrate, std::memory_order_relaxed)) {
        }
    }

    std::size_t fillDatagram(guint8* destination, uint64_t datagramDeadlineNanoseconds) {
        if (!destination) {
            return 0;
        }

        std::size_t realPackets = 0;
        for (std::size_t slot = 0; slot < kTsPacketsPerDatagram; ++slot) {
            const uint64_t slotOffset = multiplyDivide(
                slot * kTsPacketSize * 8ULL, 1000000000ULL, targetBitrate);
            const uint64_t slotTime = datagramDeadlineNanoseconds + slotOffset;
            guint8* outputPacket = destination + slot * kTsPacketSize;

            if (!scheduledPackets.empty() &&
                scheduledPackets.front().dueNanoseconds <= slotTime) {
                ScheduledPacket packet = std::move(scheduledPackets.front());
                scheduledPackets.pop_front();

                if (packet.packet.hasPcr) {
                    restampPcr(packet.packet, slotTime);
                }
                std::copy(packet.packet.bytes.begin(), packet.packet.bytes.end(), outputPacket);
                bufferedBytes.fetch_sub(kTsPacketSize, std::memory_order_relaxed);
                ++realPackets;

                if (slotTime > packet.dueNanoseconds + packetIntervalNanoseconds * 2ULL) {
                    ++lateRealPackets;
                }
            } else {
                makeNullPacket(outputPacket);
            }
        }
        return realPackets;
    }

    void restampPcr(TsPacket& packet, uint64_t slotTimeNanoseconds) {
        if (!packet.hasPcr) {
            return;
        }

        if (!outputPcrInitialized || packet.discontinuity || slotTimeNanoseconds < outputPcrOriginNanoseconds) {
            outputPcrOriginTicks = packet.pcrTicks;
            outputPcrOriginNanoseconds = slotTimeNanoseconds;
            outputPcrInitialized = true;
        }

        const uint64_t elapsedNanoseconds = slotTimeNanoseconds - outputPcrOriginNanoseconds;
        const uint64_t elapsedTicks = nanosecondsToPcrTicks(elapsedNanoseconds);
        const uint64_t rewrittenTicks = (outputPcrOriginTicks + elapsedTicks) % kPcrTicksModulus;
        writePcr(packet.bytes, rewrittenTicks);
        ++rewrittenPcrPackets;
    }

    void makeNullPacket(guint8* packet) {
        packet[0] = 0x47;
        packet[1] = 0x1F;
        packet[2] = 0xFF;
        packet[3] = static_cast<guint8>(0x10 | (nullContinuityCounter & 0x0F));
        std::fill(packet + 4, packet + kTsPacketSize, 0xFF);
        nullContinuityCounter = static_cast<guint8>((nullContinuityCounter + 1) & 0x0F);
    }

    void sendDatagram(const guint8* data, std::size_t size) {
        const auto* destination = reinterpret_cast<const sockaddr*>(&destinationAddress);
        const socklen_t destinationSize = sizeof(destinationAddress);
        const ssize_t sent = ::sendto(socketFd, data, size, 0, destination, destinationSize);
        if (sent < 0) {
            std::cerr << "WISI UDP send failed: " << std::strerror(errno) << std::endl;
        }
    }

    void maybeLogStats(uint64_t nowNanoseconds) {
        if (lastStatsNanoseconds == 0 || nowNanoseconds < lastStatsNanoseconds ||
            nowNanoseconds - lastStatsNanoseconds < kStatsIntervalNanoseconds) {
            return;
        }
        lastStatsNanoseconds = nowNanoseconds;

        const uint64_t real = totalRealPackets.load(std::memory_order_relaxed);
        const uint64_t nulls = totalNullPackets.load(std::memory_order_relaxed);
        const uint64_t elapsed = nowNanoseconds > statsStartedNanoseconds
            ? nowNanoseconds - statsStartedNanoseconds : 0;
        const uint64_t realBitrate = elapsed > 0
            ? multiplyDivide(real * kTsPacketSize * 8ULL, 1000000000ULL, elapsed)
            : 0;

        std::cerr << "WISI shaper stats: target=" << targetBitrate
                  << " real=" << realBitrate
                  << " buffered=" << bufferedBytes.load(std::memory_order_relaxed)
                  << "B null_packets=" << nulls
                  << " pcr_rewritten=" << rewrittenPcrPackets.load(std::memory_order_relaxed)
                  << " pcr_segment_peak=" << peakPcrSegmentBitrate.load(std::memory_order_relaxed)
                  << " late_real=" << lateRealPackets.load(std::memory_order_relaxed)
                  << " clock_resets=" << schedulerResets.load(std::memory_order_relaxed)
                  << " pcr_resets=" << pcrTimelineResets.load(std::memory_order_relaxed)
                  << std::endl;
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
    uint64_t packetIntervalNanoseconds = 0;
    sockaddr_in destinationAddress {};

    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> bufferedBytes{0};
    std::thread senderThread;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::condition_variable queueSpace;
    std::deque<TimedChunk> queuedChunks;

    std::vector<guint8> parserBytes;
    std::size_t parserOffset = 0;
    std::deque<TsPacket> unscheduledPackets;
    std::deque<ScheduledPacket> scheduledPackets;

    bool timelineInitialized = false;
    uint64_t anchorPcrTicks = 0;
    uint64_t anchorDueNanoseconds = 0;

    bool outputPcrInitialized = false;
    uint64_t outputPcrOriginTicks = 0;
    uint64_t outputPcrOriginNanoseconds = 0;
    guint8 nullContinuityCounter = 0;

    uint64_t statsStartedNanoseconds = 0;
    uint64_t lastStatsNanoseconds = 0;
    std::atomic<uint64_t> totalDatagrams{0};
    std::atomic<uint64_t> totalRealPackets{0};
    std::atomic<uint64_t> totalNullPackets{0};
    std::atomic<uint64_t> rewrittenPcrPackets{0};
    std::atomic<uint64_t> peakPcrSegmentBitrate{0};
    std::atomic<uint64_t> lateRealPackets{0};
    std::atomic<uint64_t> schedulerResets{0};
    std::atomic<uint64_t> pcrTimelineResets{0};
    std::atomic<uint64_t> resyncDiscardedBytes{0};
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

    std::cerr << "WISI compatibility PCR-aware TS shaper: target_bitrate=" << config.targetBitrate
              << " packetization=7x188 prebuffer_ms=400"
              << " null_pid=0x1fff pcr_schedule=source pcr_restamp=output-clock"
              << " clock=clock_nanosleep-abstime busywait=off"
              << std::endl;
    return sink;
}

} // namespace WisiCbrOutput
