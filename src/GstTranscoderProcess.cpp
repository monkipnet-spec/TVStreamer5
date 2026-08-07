#include "GstTranscoderProcess.h"

#include "TranscoderModule.h"
#include "utils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <gst/gst.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

bool executableInPath(const std::string& name, std::string* path = nullptr) {
    const char* envPath = std::getenv("PATH");
    std::string paths = envPath ? envPath : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    std::stringstream ss(paths);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            if (path) *path = candidate.string();
            return true;
        }
    }
    return false;
}

bool hasFactory(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

std::string findAacEncoder() {
    for (const char* name : {"voaacenc", "fdkaacenc", "avenc_aac"}) {
        if (hasFactory(name)) return name;
    }
    return {};
}

std::string findMp3Encoder() {
    for (const char* name : {"lamemp3enc", "avenc_mp3"}) {
        if (hasFactory(name)) return name;
    }
    return {};
}

std::string normalizedOutputType(const StreamConfig& cfg) {
    std::string type = toLower(cfg.outputType);
    if (type == "udp_vbr" || type == "udpvbr") {
        type = "udp-vbr";
    } else if (type == "udp_cbr" || type == "udpcbr") {
        type = "udp-cbr";
    }
    if (type == "udp") {
        return cfg.cbr ? "udp-cbr" : "udp-vbr";
    }
    if (type != "udp-vbr" && type != "udp-cbr") {
        return type;
    }
    return type;
}

bool isUdpType(const std::string& type) {
    return type == "udp" || type == "udp-vbr" || type == "udp-cbr";
}

StreamOutputConfig primaryOutputConfig(const StreamConfig& cfg) {
    StreamOutputConfig output;
    output.outputType = cfg.outputType;
    output.outputMode = cfg.outputMode;
    output.outputHost = cfg.outputHost;
    output.outputPort = cfg.outputPort;
    return output;
}

StreamConfig configForOutput(const StreamConfig& base, const StreamOutputConfig& output) {
    StreamConfig cfg = base;
    cfg.outputType = output.outputType;
    cfg.outputMode = output.outputMode;
    cfg.outputHost = output.outputHost;
    cfg.outputPort = output.outputPort;
    cfg.additionalOutputs.clear();
    const std::string type = normalizedOutputType(cfg);
    if (type == "udp-cbr") cfg.cbr = true;
    if (type == "udp-vbr") cfg.cbr = false;
    return cfg;
}

std::vector<StreamConfig> outputConfigs(const StreamConfig& cfg) {
    std::vector<StreamConfig> outputs;
    outputs.push_back(configForOutput(cfg, primaryOutputConfig(cfg)));
    for (const auto& output : cfg.additionalOutputs) {
        outputs.push_back(configForOutput(cfg, output));
    }
    return outputs;
}

void addArg(std::vector<std::string>& args, const std::string& value) {
    if (!value.empty()) args.push_back(value);
}

std::string property(const std::string& name, const std::string& value) {
    return name + "=" + value;
}

std::string inputUriForGstreamer(const StreamConfig& cfg) {
    std::string input = cfg.inputUri;
    const std::string lower = toLower(input);
    if (lower.rfind("hls://", 0) == 0) {
        input = "http://" + input.substr(6);
    }
    return input;
}

std::string shellQuote(const std::string& value) {
    if (value.empty()) return "''";
    bool safe = true;
    for (unsigned char ch : value) {
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/' ||
              ch == ':' || ch == '=' || ch == ',' || ch == '+' || ch == '?' ||
              ch == '&' || ch == '@' || ch == '%' || ch == ';')) {
            safe = false;
            break;
        }
    }
    if (safe) return value;
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') quoted += "'\\''";
        else quoted += ch;
    }
    quoted += "'";
    return quoted;
}

std::string commandLineForLog(const std::vector<std::string>& args) {
    std::ostringstream ss;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) ss << ' ';
        ss << shellQuote(args[i]);
    }
    return ss.str();
}

uint64_t safeVideoBitrate(const StreamConfig& cfg) {
    return std::max<uint64_t>(cfg.transcodeVideoBitrate, 500000);
}

uint64_t safeAudioBitrate(const StreamConfig& cfg) {
    return std::clamp<uint64_t>(cfg.transcodeAudioBitrate, 64000, 320000);
}

uint64_t muxBitrate(const StreamConfig& cfg) {
    if (cfg.targetBitrate > 0) return cfg.targetBitrate;
    const uint64_t video = safeVideoBitrate(cfg);
    const uint64_t audio = safeAudioBitrate(cfg);
    return video + audio + 500000;
}

void addQueue(std::vector<std::string>& args, const std::string& name, guint64 maxTimeNs = 3000000000ULL) {
    args.insert(args.end(), {
        "queue",
        property("name", name),
        "max-size-buffers=0",
        "max-size-bytes=0",
        property("max-size-time", std::to_string(maxTimeNs))
    });
}

void addVideoBranch(std::vector<std::string>& args, const StreamConfig& cfg, const std::string& muxPad) {
    int width = 1920;
    int height = 1080;
    TranscoderModule::resolutionSize(cfg.transcodeResolution, width, height);
    const uint64_t bitrateKbps = safeVideoBitrate(cfg) / 1000;

    args.insert(args.end(), {"dec.", "!"});
    addQueue(args, "transcode_video_queue");
    args.insert(args.end(), {
        "!", "video/x-raw",
        "!", "videoconvert",
        "!", "videoscale", "add-borders=true",
        "!", "videorate",
        "!", "video/x-raw,format=I420,width=" + std::to_string(width) +
              ",height=" + std::to_string(height) + ",framerate=25/1",
        "!", "x264enc",
        "tune=zerolatency",
        "speed-preset=veryfast",
        property("bitrate", std::to_string(bitrateKbps)),
        "key-int-max=50",
        "bframes=0",
        "byte-stream=true",
        "aud=true",
        "sliced-threads=true",
        "option-string=nal-hrd=cbr:force-cfr=1:repeat-headers=1:scenecut=0",
        "!", "h264parse", "config-interval=1",
        "!", "video/x-h264,stream-format=byte-stream,alignment=au",
        "!"
    });
    addQueue(args, "transcode_video_mux_queue", 1000000000ULL);
    args.insert(args.end(), {"!", muxPad});
}

void addAudioBranch(std::vector<std::string>& args, const StreamConfig& cfg, const std::string& muxPad, std::string& error) {
    const std::string audioCodec = toLower(cfg.transcodeAudioCodec);
    const uint64_t bitrate = safeAudioBitrate(cfg);

    args.insert(args.end(), {"dec.", "!"});
    addQueue(args, "transcode_audio_queue");
    std::string selectedAacEncoder;
    std::string selectedMp3Encoder;
    if (audioCodec == "mp3") {
        selectedMp3Encoder = findMp3Encoder();
    } else {
        selectedAacEncoder = findAacEncoder();
    }
    const std::string rawAudioCaps = selectedAacEncoder == "avenc_aac"
        ? "audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2"
        : "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2";

    args.insert(args.end(), {
        "!", "audio/x-raw",
        "!", "audioconvert",
        "!", "audioresample",
        "!", rawAudioCaps,
        "!"
    });

    if (audioCodec == "mp3") {
        const std::string encoder = selectedMp3Encoder;
        if (encoder.empty()) {
            error = "MP3 encoder is not available";
            return;
        }
        if (encoder == "lamemp3enc") {
            args.insert(args.end(), {
                "lamemp3enc",
                "target=bitrate",
                "cbr=true",
                property("bitrate", std::to_string(std::max<uint64_t>(bitrate / 1000, 64))),
                "!", "mpegaudioparse",
                "!", "audio/mpeg,mpegversion=1,layer=3"
            });
        } else {
            args.insert(args.end(), {
                "avenc_mp3",
                property("bitrate", std::to_string(bitrate)),
                "!", "mpegaudioparse",
                "!", "audio/mpeg,mpegversion=1,layer=3"
            });
        }
    } else {
        // Stable mode intentionally re-encodes audio even when the UI value is
        // "copy". Copy/passthrough caused the previous AAC caps-list and PMT
        // problems; raw audio -> AAC makes the muxed TS deterministic.
        const std::string encoder = selectedAacEncoder;
        if (encoder.empty()) {
            error = "AAC encoder is not available";
            return;
        }
        args.insert(args.end(), {
            encoder,
            property("bitrate", std::to_string(bitrate)),
            "!", "aacparse",
            "!", "audio/mpeg,mpegversion=4,stream-format=adts"
        });
    }

    args.insert(args.end(), {"!"});
    addQueue(args, "transcode_audio_mux_queue", 1000000000ULL);
    args.insert(args.end(), {"!", muxPad});
}

void addMuxAndUdpSink(std::vector<std::string>& args, const StreamConfig& cfg) {
    args.insert(args.end(), {
        "mpegtsmux",
        "name=mux",
        "alignment=7",
        property("bitrate", std::to_string(muxBitrate(cfg))),
        "pat-interval=9000",
        "pmt-interval=9000",
        "pcr-interval=3600",
        "si-interval=9000",
        "!"
    });
    addQueue(args, "transcode_output_queue", 1000000000ULL);
    args.insert(args.end(), {
        "!", "udpsink",
        property("host", cfg.outputHost.empty() ? "127.0.0.1" : cfg.outputHost),
        property("port", std::to_string(cfg.outputPort)),
        "sync=true",
        "async=false",
        "auto-multicast=true",
        "ttl-mc=32"
    });
    if (!cfg.interfaceAddress.empty()) {
        args.push_back(property("bind-address", cfg.interfaceAddress));
    }
}

void addTestSources(std::vector<std::string>& args, const StreamConfig& cfg, const std::string& videoPad, const std::string& audioPad, std::string& error) {
    args.insert(args.end(), {
        "videotestsrc", "is-live=true", "pattern=smpte", "!", "video/x-raw,framerate=25/1", "!"
    });
    addQueue(args, "test_video_queue");
    args.insert(args.end(), {
        "!", "videoconvert", "!", "videoscale", "add-borders=true", "!", "videorate",
        "!", "video/x-raw,format=I420,width=1280,height=720,framerate=25/1",
        "!", "x264enc", "tune=zerolatency", "speed-preset=veryfast",
        property("bitrate", std::to_string(safeVideoBitrate(cfg) / 1000)),
        "key-int-max=50", "bframes=0", "byte-stream=true", "aud=true",
        "option-string=nal-hrd=cbr:force-cfr=1:repeat-headers=1:scenecut=0",
        "!", "h264parse", "config-interval=1",
        "!", "video/x-h264,stream-format=byte-stream,alignment=au", "!", videoPad,
        "audiotestsrc", "is-live=true", "wave=sine", "freq=1000", "!", "audio/x-raw,rate=48000,channels=2", "!"
    });
    const std::string encoder = findAacEncoder();
    if (encoder.empty()) {
        error = "AAC encoder is not available";
        return;
    }
    args.insert(args.end(), {
        encoder, property("bitrate", std::to_string(safeAudioBitrate(cfg))),
        "!", "aacparse", "!", "audio/mpeg,mpegversion=4,stream-format=adts", "!", audioPad
    });
}

} // namespace

GstTranscoderProcess::~GstTranscoderProcess() {
    stop();
}

bool GstTranscoderProcess::isAvailable(std::string* error) {
    std::string gstLaunchPath;
    if (!executableInPath("gst-launch-1.0", &gstLaunchPath)) {
        if (error) *error = "gst-launch-1.0 executable was not found in PATH";
        return false;
    }

    const char* required[] = {
        "uridecodebin", "queue", "videoconvert", "videoscale", "videorate",
        "x264enc", "h264parse", "audioconvert", "audioresample",
        "aacparse", "mpegtsmux", "udpsink", nullptr
    };
    std::vector<std::string> missing;
    for (const char** name = required; *name; ++name) {
        if (!hasFactory(*name)) missing.emplace_back(*name);
    }
    if (findAacEncoder().empty()) {
        missing.emplace_back("AAC encoder: fdkaacenc, voaacenc or avenc_aac");
    }
    if (!missing.empty()) {
        std::ostringstream ss;
        ss << "missing GStreamer elements";
        for (size_t i = 0; i < missing.size(); ++i) {
            ss << (i == 0 ? ": " : ", ") << missing[i];
        }
        if (error) *error = ss.str();
        return false;
    }
    if (error) *error = "GStreamer transcoder is available: " + gstLaunchPath;
    return true;
}

bool GstTranscoderProcess::spawnProcess(
    const std::vector<std::string>& args,
    const std::string& description,
    ChildProcess& child,
    std::string& error) {
    if (args.empty()) {
        error = "empty gst-launch command";
        return false;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }

    if (pid == 0) {
        int devNull = ::open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            if (devNull > STDERR_FILENO) ::close(devNull);
        }

        std::vector<std::string> storage = args;
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& arg : storage) argv.push_back(arg.data());
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        std::cerr << "GStreamer transcoder exec failed: " << std::strerror(errno) << std::endl;
        std::_Exit(127);
    }

    child.pid = pid;
    child.description = description;
    return true;
}

std::vector<std::string> GstTranscoderProcess::buildCommand(
    const StreamConfig& baseConfig,
    const StreamConfig& outputConfig,
    std::string& description,
    std::string& error) {
    const std::string type = normalizedOutputType(outputConfig);
    if (!isUdpType(type)) {
        error = "stable GStreamer transcoder currently supports UDP/UDP-CBR/UDP-VBR outputs only";
        return {};
    }

    const std::string videoPad = outputConfig.videoPid > 0
        ? "mux.sink_" + std::to_string(outputConfig.videoPid)
        : "mux.";
    const std::string audioPad = outputConfig.audioPid > 0
        ? "mux.sink_" + std::to_string(outputConfig.audioPid)
        : "mux.";

    std::vector<std::string> args = {"gst-launch-1.0", "-e"};
    addMuxAndUdpSink(args, outputConfig);

    if (baseConfig.testPattern) {
        addTestSources(args, baseConfig, videoPad, audioPad, error);
    } else {
        args.insert(args.end(), {
            "uridecodebin",
            "name=dec",
            property("uri", inputUriForGstreamer(baseConfig)),
            "use-buffering=false"
        });
        addVideoBranch(args, baseConfig, videoPad);
        addAudioBranch(args, baseConfig, audioPad, error);
    }
    if (!error.empty()) return {};

    description = type + "@udp://" + outputConfig.outputHost + ":" + std::to_string(outputConfig.outputPort);
    return args;
}

bool GstTranscoderProcess::start(const StreamConfig& config, std::string& error) {
    stop();
    stopping = false;

    std::string availableMessage;
    if (!isAvailable(&availableMessage)) {
        error = availableMessage;
        return false;
    }

    const auto outputs = outputConfigs(config);
    if (outputs.empty()) {
        error = "no outputs configured";
        return false;
    }

    std::vector<ChildProcess> started;
    for (const auto& output : outputs) {
        std::string description;
        std::string commandError;
        std::vector<std::string> args = buildCommand(config, output, description, commandError);
        if (!commandError.empty()) {
            error = commandError;
            for (auto& startedChild : started) {
                if (startedChild.pid > 0) {
                    ::kill(startedChild.pid, SIGTERM);
                    ::waitpid(startedChild.pid, nullptr, 0);
                }
            }
            return false;
        }

        ChildProcess child;
        if (!spawnProcess(args, description, child, error)) {
            for (auto& startedChild : started) {
                if (startedChild.pid > 0) {
                    ::kill(startedChild.pid, SIGTERM);
                    ::waitpid(startedChild.pid, nullptr, 0);
                }
            }
            return false;
        }
        std::cerr << "GStreamer transcoder command: " << commandLineForLog(args) << std::endl;
        std::cerr << "GStreamer transcoder started pid=" << child.pid
                  << " output=" << description << std::endl;
        started.push_back(child);
    }

    children = std::move(started);
    return true;
}

void GstTranscoderProcess::stop() {
    stopping = true;
    for (auto& child : children) {
        if (child.pid <= 0) continue;
        int status = 0;
        pid_t done = ::waitpid(child.pid, &status, WNOHANG);
        if (done == 0) {
            ::kill(child.pid, SIGTERM);
            for (int i = 0; i < 40; ++i) {
                done = ::waitpid(child.pid, &status, WNOHANG);
                if (done == child.pid) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (done == 0) {
                ::kill(child.pid, SIGKILL);
                ::waitpid(child.pid, &status, 0);
            }
        }
        child.pid = -1;
    }
    children.clear();
}

bool GstTranscoderProcess::isRunning() {
    bool anyRunning = false;
    for (auto& child : children) {
        if (child.pid <= 0) continue;
        int status = 0;
        pid_t done = ::waitpid(child.pid, &status, WNOHANG);
        if (done == 0) {
            anyRunning = true;
            continue;
        }
        if (done == child.pid) {
            std::cerr << "GStreamer transcoder exited pid=" << child.pid
                      << " output=" << child.description
                      << " status=" << status << std::endl;
            child.pid = -1;
        }
    }
    return anyRunning;
}

std::string GstTranscoderProcess::description() const {
    std::ostringstream ss;
    for (size_t i = 0; i < children.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << children[i].description;
    }
    return ss.str();
}
