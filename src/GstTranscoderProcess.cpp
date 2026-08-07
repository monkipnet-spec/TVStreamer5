#include "GstTranscoderProcess.h"

#include "TranscoderModule.h"
#include "protocols/GstInputProtocols.h"
#include "protocols/GstOutputProtocols.h"
#include "protocols/GstProtocolTypes.h"
#include "utils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <gst/gst.h>
#include <sys/wait.h>
#include <unistd.h>

using tvs::protocols::ContainerKind;
using tvs::protocols::GstOutputSpec;

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

bool validateFactories(const std::vector<std::string>& names, std::vector<std::string>& missing) {
    bool ok = true;
    for (const auto& name : names) {
        if (!hasFactory(name.c_str())) {
            missing.push_back(name);
            ok = false;
        }
    }
    return ok;
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

void addQueue(std::vector<std::string>& args, const std::string& name, uint64_t maxTimeNs = 5000000000ULL) {
    args.insert(args.end(), {
        "queue",
        "name=" + name,
        "max-size-buffers=0",
        "max-size-bytes=0",
        "max-size-time=" + std::to_string(maxTimeNs)
    });
}

std::string property(const std::string& name, const std::string& value) {
    return name + "=" + value;
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

bool validateOutputAvailability(const StreamConfig& outputConfig, std::string& error) {
    std::vector<std::string> missing;
    validateFactories(tvs::protocols::requiredElementsForOutput(tvs::protocols::outputKind(outputConfig)), missing);
    if (!missing.empty()) {
        std::ostringstream ss;
        ss << "missing output protocol elements for " << tvs::protocols::normalizedOutputType(outputConfig);
        for (size_t i = 0; i < missing.size(); ++i) {
            ss << (i == 0 ? ": " : ", ") << missing[i];
        }
        error = ss.str();
        return false;
    }
    return true;
}

void addVideoBranch(std::vector<std::string>& args, const StreamConfig& cfg, const GstOutputSpec& spec) {
    int width = 1920;
    int height = 1080;
    TranscoderModule::resolutionSize(cfg.transcodeResolution, width, height);
    const uint64_t bitrateKbps = tvs::protocols::safeVideoBitrate(cfg) / 1000;
    const bool flv = spec.container == ContainerKind::Flv;
    const bool rtsp = spec.container == ContainerKind::Rtsp;

    args.insert(args.end(), {"dec.", "!"});
    addQueue(args, "transcode_video_queue", 8000000000ULL);
    args.insert(args.end(), {
        "!", "video/x-raw",
        "!", "videoconvert",
        "!", "deinterlace", "method=linear",
        "!", "videoscale", "add-borders=true",
        "!", "videorate", "drop-only=false",
        "!", "video/x-raw,format=I420,width=" + std::to_string(width) +
              ",height=" + std::to_string(height) + ",framerate=25/1,pixel-aspect-ratio=1/1,interlace-mode=progressive",
        "!", "x264enc",
        "tune=zerolatency",
        "speed-preset=ultrafast",
        property("bitrate", std::to_string(bitrateKbps)),
        "key-int-max=50",
        "bframes=0",
        property("byte-stream", flv ? "false" : "true"),
        "aud=true",
        "sliced-threads=true",
        "vbv-buf-capacity=500",
        "option-string=nal-hrd=cbr:force-cfr=1:repeat-headers=1:scenecut=0",
        "!", "h264parse", property("config-interval", flv ? "-1" : "1"),
        "!", flv
            ? "video/x-h264,stream-format=avc,alignment=au"
            : "video/x-h264,stream-format=byte-stream,alignment=au",
        "!"
    });
    addQueue(args, "transcode_video_mux_queue", 3000000000ULL);
    args.insert(args.end(), {"!", spec.videoPad});
}

void addAudioBranch(std::vector<std::string>& args, const StreamConfig& cfg, const GstOutputSpec& spec, std::string& error) {
    const std::string audioCodec = toLower(cfg.transcodeAudioCodec);
    const uint64_t bitrate = tvs::protocols::safeAudioBitrate(cfg);
    const bool flv = spec.container == ContainerKind::Flv;
    const bool rtsp = spec.container == ContainerKind::Rtsp;

    args.insert(args.end(), {"dec.", "!"});
    addQueue(args, "transcode_audio_queue", 8000000000ULL);

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
        "!", "audiorate",
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
        const std::string encoder = selectedAacEncoder;
        if (encoder.empty()) {
            error = "AAC encoder is not available";
            return;
        }
        args.insert(args.end(), {
            encoder,
            property("bitrate", std::to_string(bitrate)),
            "!", "aacparse",
            "!", (flv || rtsp)
                ? "audio/mpeg,mpegversion=4,stream-format=raw"
                : "audio/mpeg,mpegversion=4,stream-format=adts"
        });
    }

    args.insert(args.end(), {"!"});
    addQueue(args, "transcode_audio_mux_queue", 3000000000ULL);
    args.insert(args.end(), {"!", spec.audioPad});
}

void addTestSources(std::vector<std::string>& args, const StreamConfig& cfg, const GstOutputSpec& spec, std::string& error) {
    StreamConfig testCfg = cfg;
    testCfg.transcodeResolution = cfg.transcodeResolution.empty() ? "1280x720" : cfg.transcodeResolution;

    args.insert(args.end(), {
        "videotestsrc", "is-live=true", "pattern=smpte", "!", "video/x-raw,framerate=25/1", "!"
    });
    addQueue(args, "test_video_queue", 3000000000ULL);
    args.insert(args.end(), {
        "!", "videoconvert", "!", "videoscale", "add-borders=true", "!", "videorate",
        "!", "video/x-raw,format=I420,width=1280,height=720,framerate=25/1,interlace-mode=progressive",
        "!", "x264enc", "tune=zerolatency", "speed-preset=ultrafast",
        property("bitrate", std::to_string(tvs::protocols::safeVideoBitrate(testCfg) / 1000)),
        "key-int-max=50", "bframes=0",
        property("byte-stream", spec.container == ContainerKind::Flv ? "false" : "true"),
        "aud=true", "sliced-threads=true", "vbv-buf-capacity=500",
        "option-string=nal-hrd=cbr:force-cfr=1:repeat-headers=1:scenecut=0",
        "!", "h264parse", property("config-interval", spec.container == ContainerKind::Flv ? "-1" : "1"),
        "!", spec.container == ContainerKind::Flv
            ? "video/x-h264,stream-format=avc,alignment=au"
            : "video/x-h264,stream-format=byte-stream,alignment=au",
        "!", spec.videoPad,
        "audiotestsrc", "is-live=true", "wave=sine", "freq=1000", "!", "audio/x-raw,rate=48000,channels=2", "!"
    });
    const std::string encoder = findAacEncoder();
    if (encoder.empty()) {
        error = "AAC encoder is not available";
        return;
    }
    args.insert(args.end(), {
        encoder, property("bitrate", std::to_string(tvs::protocols::safeAudioBitrate(cfg))),
        "!", "aacparse", "!",
        (spec.container == ContainerKind::Flv || spec.container == ContainerKind::Rtsp)
            ? "audio/mpeg,mpegversion=4,stream-format=raw"
            : "audio/mpeg,mpegversion=4,stream-format=adts",
        "!", spec.audioPad
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

    std::vector<std::string> required = tvs::protocols::requiredInputElements();
    const std::vector<std::string> common = {
        "queue", "videoconvert", "deinterlace", "videoscale", "videorate",
        "x264enc", "h264parse", "audioconvert", "audioresample", "audiorate", "aacparse"
    };
    required.insert(required.end(), common.begin(), common.end());

    std::vector<std::string> missing;
    validateFactories(required, missing);
    if (findAacEncoder().empty()) {
        missing.emplace_back("AAC encoder: fdkaacenc, voaacenc or avenc_aac");
    }
    if (!missing.empty()) {
        std::ostringstream ss;
        ss << "missing GStreamer transcoder elements";
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
    if (!validateOutputAvailability(outputConfig, error)) {
        return {};
    }

    std::vector<std::string> args = {"gst-launch-1.0", "-e"};
    GstOutputSpec outputSpec;
    if (!tvs::protocols::appendOutputMuxAndSink(args, outputConfig, outputSpec, error)) {
        return {};
    }

    if (baseConfig.testPattern) {
        addTestSources(args, baseConfig, outputSpec, error);
    } else {
        tvs::protocols::appendDecodeInput(args, baseConfig);
        addVideoBranch(args, baseConfig, outputSpec);
        addAudioBranch(args, baseConfig, outputSpec, error);
    }
    if (!error.empty()) return {};

    description = outputSpec.description;
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

    const auto outputs = tvs::protocols::outputConfigs(config);
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
