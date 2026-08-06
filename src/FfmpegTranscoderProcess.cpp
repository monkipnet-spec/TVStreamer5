#include "FfmpegTranscoderProcess.h"

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
#include <sys/wait.h>
#include <unistd.h>

namespace {

bool isUdpType(const std::string& type) {
    return type == "udp" || type == "udp-vbr" || type == "udp-cbr" ||
           type == "udp_vbr" || type == "udp_cbr" || type == "udpvbr" || type == "udpcbr";
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
    if (type != "udp-vbr" && type != "udp-cbr" && type != "srt" &&
        type != "http" && type != "hls" && type != "rtmp" && type != "youtube") {
        return "udp-cbr";
    }
    return type;
}

std::string srtOutputMode(const StreamConfig& cfg) {
    return toLower(cfg.outputMode) == "caller" ? "caller" : "listener";
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
    if (type == "udp-cbr") {
        cfg.cbr = true;
    } else if (type == "udp-vbr") {
        cfg.cbr = false;
    }
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

bool isRtmpUri(const std::string& uriLower) {
    return uriLower.rfind("rtmp://", 0) == 0 ||
           uriLower.rfind("rtmps://", 0) == 0 ||
           uriLower.rfind("rtmpt://", 0) == 0 ||
           uriLower.rfind("rtmpe://", 0) == 0 ||
           uriLower.rfind("rtmpte://", 0) == 0 ||
           uriLower.rfind("rtmpts://", 0) == 0;
}

std::string rtmpOutputLocation(const StreamConfig& cfg) {
    const std::string type = normalizedOutputType(cfg);
    const std::string host = cfg.outputHost;
    const std::string hostLower = toLower(host);
    if (isRtmpUri(hostLower)) {
        return host;
    }
    if (type == "youtube") {
        return "rtmp://a.rtmp.youtube.com/live2/" + host;
    }
    const std::string targetHost = host.empty() ? "127.0.0.1" : host;
    return "rtmp://" + targetHost + ":" + std::to_string(cfg.outputPort) + "/live/" + cfg.id;
}

std::string hlsDirectory(const StreamConfig& cfg) {
    return "/tmp/tvstreamer5-hls/" + cfg.id;
}

bool hasQuery(const std::string& url) {
    return url.find('?') != std::string::npos;
}

void appendQuery(std::string& url, const std::string& option) {
    if (option.empty()) return;
    url += hasQuery(url) ? "&" : "?";
    url += option;
}


std::string shellQuote(const std::string& value) {
    if (value.empty()) {
        return "''";
    }
    bool safe = true;
    for (unsigned char ch : value) {
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/' || ch == ':' || ch == '=' || ch == ',' || ch == '+' || ch == '?' || ch == '&' || ch == '@' || ch == '%')) {
            safe = false;
            break;
        }
    }
    if (safe) {
        return value;
    }
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted += ch;
        }
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

std::string inputUriForFfmpeg(const StreamConfig& cfg) {
    std::string input = cfg.inputUri;
    if (cfg.testPattern) {
        return input;
    }

    const std::string lower = toLower(input);
    if (lower.rfind("hls://", 0) == 0) {
        input = "http://" + input.substr(6);
    }

    if (lower.rfind("udp://", 0) == 0) {
        appendQuery(input, "reuse=1");
        appendQuery(input, "fifo_size=1000000");
        appendQuery(input, "overrun_nonfatal=1");
        const std::string inputIface = cfg.inputInterfaceAddressConfigured
            ? cfg.inputInterfaceAddress
            : cfg.interfaceAddress;
        if (!inputIface.empty()) {
            appendQuery(input, "localaddr=" + inputIface);
        }
    }
    return input;
}

std::string udpOutputUrl(const StreamConfig& cfg) {
    std::string url = "udp://" + cfg.outputHost + ":" + std::to_string(cfg.outputPort);
    appendQuery(url, "pkt_size=1316");
    appendQuery(url, "ttl=32");
    if (!cfg.interfaceAddress.empty()) {
        appendQuery(url, "localaddr=" + cfg.interfaceAddress);
    }
    return url;
}

std::string srtOutputUrl(const StreamConfig& cfg) {
    const std::string mode = srtOutputMode(cfg);
    const bool caller = mode == "caller";
    const std::string targetHost = cfg.outputHost.empty() || cfg.outputHost == "0.0.0.0" || cfg.outputHost == "::"
        ? "127.0.0.1"
        : cfg.outputHost;
    const std::string bindHost = cfg.interfaceAddress.empty() ? "0.0.0.0" : cfg.interfaceAddress;
    std::string url = "srt://" + (caller ? targetHost : bindHost) + ":" + std::to_string(cfg.outputPort);
    appendQuery(url, "mode=" + mode);
    appendQuery(url, "latency=250");
    if (!caller) {
        appendQuery(url, "reuseaddr=1");
    }
    return url;
}

std::string httpOutputUrl(const StreamConfig& cfg) {
    const std::string bindHost = cfg.outputHost.empty() ?
        (cfg.interfaceAddress.empty() ? "0.0.0.0" : cfg.interfaceAddress) :
        cfg.outputHost;
    std::string url = "http://" + bindHost + ":" + std::to_string(cfg.outputPort) + "/stream/" + cfg.id + ".ts";
    return url;
}

void addCommonInputArgs(std::vector<std::string>& args, const StreamConfig& cfg) {
    if (cfg.testPattern) {
        args.insert(args.end(), {
            "-f", "lavfi", "-re", "-i", "testsrc2=size=1920x1080:rate=25",
            "-f", "lavfi", "-re", "-i", "sine=frequency=1000:sample_rate=48000"
        });
        return;
    }

    const std::string input = inputUriForFfmpeg(cfg);
    const std::string lower = toLower(input);
    if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) {
        args.insert(args.end(), {
            "-reconnect", "1",
            "-reconnect_streamed", "1",
            "-reconnect_delay_max", "5"
        });
    }
    args.insert(args.end(), {
        // Read live sources at their native cadence. This prevents ffmpeg from
        // bursting HTTP/file replacement inputs into the MPEG-TS muxer and keeps
        // PCR/DTS generation stable for UDP-CBR receivers.
        "-re",
        // Give ffmpeg enough data to see SPS/PPS and audio configuration before
        // starting the encoder. The previous low-latency/nobuffer input setup
        // could start from incomplete H.264 state and produce dts < pcr output.
        "-analyzeduration", "20000000",
        "-probesize", "20000000",
        "-fflags", "+genpts+discardcorrupt",
        "-err_detect", "ignore_err",
        "-thread_queue_size", "4096",
        "-i", input
    });
}

void addEncodeArgs(std::vector<std::string>& args, const StreamConfig& cfg, bool flvOutput) {
    int width = 1920;
    int height = 1080;
    TranscoderModule::resolutionSize(cfg.transcodeResolution, width, height);
    const uint64_t videoBitrate = std::max<uint64_t>(cfg.transcodeVideoBitrate, 500000);
    const uint64_t audioBitrate = std::clamp<uint64_t>(cfg.transcodeAudioBitrate, 64000, 320000);

    if (cfg.testPattern) {
        args.insert(args.end(), {"-map", "0:v:0", "-map", "1:a:0"});
    } else {
        args.insert(args.end(), {"-map", "0:v:0", "-map", "0:a:0?"});
    }

    std::ostringstream vf;
    vf << "scale=" << width << ":" << height
       << ":force_original_aspect_ratio=decrease,"
       << "pad=" << width << ":" << height << ":(ow-iw)/2:(oh-ih)/2,"
       << "fps=25,format=yuv420p";

    args.insert(args.end(), {
        "-vf", vf.str(),
        "-c:v", "libx264",
        "-preset", "veryfast",
        "-tune", "zerolatency",
        "-b:v", std::to_string(videoBitrate),
        "-minrate", std::to_string(videoBitrate),
        "-maxrate", std::to_string(videoBitrate),
        "-bufsize", std::to_string(std::max<uint64_t>(videoBitrate * 2, 1000000)),
        "-r", "25",
        "-g", "50",
        "-keyint_min", "50",
        "-sc_threshold", "0",
        "-bf", "0",
        "-profile:v", "high",
        "-level:v", "4.0",
        "-flags:v", "-global_header",
        "-pix_fmt", "yuv420p",
        "-x264-params", "nal-hrd=cbr:force-cfr=1:repeat-headers=1:aud=1:keyint=50:min-keyint=50:scenecut=0:bframes=0",
        // Force SPS/PPS codec extradata to be present in the elementary
        // stream at every key frame before it enters the MPEG-TS muxer.
        // Some receivers and ffprobe joins were seeing H.264 slices before
        // PPS/SPS even though x264 repeat-headers was enabled.
        "-bsf:v", "dump_extra=freq=keyframe",
        "-vsync", "cfr"
    });

    const std::string audioCodec = toLower(cfg.transcodeAudioCodec);
    if (audioCodec == "copy" && !cfg.testPattern) {
        args.insert(args.end(), {"-c:a", "copy"});
    } else if (audioCodec == "mp3") {
        args.insert(args.end(), {
            "-c:a", "libmp3lame",
            "-b:a", std::to_string(audioBitrate),
            "-ar", "48000",
            "-ac", "2"
        });
    } else {
        args.insert(args.end(), {
            "-c:a", "aac",
            "-b:a", std::to_string(audioBitrate),
            "-ar", "48000",
            "-ac", "2"
        });
    }

    // FLV/RTMP is most compatible with AAC audio. If the user selected copy and
    // the source is not AAC/MP3, ffmpeg will fail loudly instead of producing a
    // silent or invalid stream.
    if (flvOutput && audioCodec == "mp3") {
        args.insert(args.end(), {"-flvflags", "no_duration_filesize"});
    }
}

void addMpegTsMuxArgs(std::vector<std::string>& args, const StreamConfig& cfg, bool cbr) {
    const uint32_t serviceId = cfg.serviceId > 0 ? cfg.serviceId : 1;
    args.insert(args.end(), {
        "-f", "mpegts",
        "-mpegts_service_id", std::to_string(serviceId),
        "-mpegts_flags", "+resend_headers+initial_discontinuity",
        "-pcr_period", "20",
        "-pat_period", "0.1",
        "-sdt_period", "0.5",
        "-flush_packets", "1",
        // Keep the live TS muxer from buffering a PCR window ahead of DTS.
        // v41 improved input probing but could still emit streams where
        // receivers reported only H.264 PPS errors after a multicast join.
        "-muxdelay", "0",
        "-muxpreload", "0",
        "-max_interleave_delta", "0"
    });
    if (cfg.videoPid > 0) {
        args.insert(args.end(), {"-streamid", "0:" + std::to_string(cfg.videoPid)});
    }
    if (cfg.audioPid > 0) {
        args.insert(args.end(), {"-streamid", "1:" + std::to_string(cfg.audioPid)});
    }
    if (cbr && cfg.targetBitrate > 0) {
        args.insert(args.end(), {"-muxrate", std::to_string(cfg.targetBitrate)});
    }
}

} // namespace

FfmpegTranscoderProcess::~FfmpegTranscoderProcess() {
    stop();
}

bool FfmpegTranscoderProcess::isAvailable(std::string* path) {
    const char* envPath = std::getenv("PATH");
    std::string paths = envPath ? envPath : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    std::stringstream ss(paths);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        std::filesystem::path candidate = std::filesystem::path(dir) / "ffmpeg";
        if (::access(candidate.c_str(), X_OK) == 0) {
            if (path) *path = candidate.string();
            return true;
        }
    }
    return false;
}

bool FfmpegTranscoderProcess::spawnProcess(
    const std::vector<std::string>& args,
    const std::string& description,
    ChildProcess& child,
    std::string& error) {
    if (args.empty()) {
        error = "empty ffmpeg command";
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
        for (auto& arg : storage) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        std::cerr << "FFmpeg transcoder exec failed: " << std::strerror(errno) << std::endl;
        std::_Exit(127);
    }

    child.pid = pid;
    child.description = description;
    return true;
}

std::vector<std::string> FfmpegTranscoderProcess::buildCommand(
    const StreamConfig& baseConfig,
    const StreamConfig& outputConfig,
    std::string& description,
    std::string& error) {
    (void)error;
    const std::string type = normalizedOutputType(outputConfig);
    const bool flvOutput = type == "rtmp" || type == "youtube";
    const bool hlsOutput = type == "hls";
    const bool cbrTs = type == "udp-cbr" || (type == "udp" && outputConfig.cbr);

    std::vector<std::string> args = {
        "ffmpeg",
        "-hide_banner",
        "-nostdin",
        "-loglevel", "warning"
    };

    addCommonInputArgs(args, baseConfig);
    addEncodeArgs(args, baseConfig, flvOutput);

    if (!baseConfig.serviceName.empty()) {
        args.insert(args.end(), {"-metadata", "service_name=" + baseConfig.serviceName});
    }
    if (!baseConfig.serviceProvider.empty()) {
        args.insert(args.end(), {"-metadata", "service_provider=" + baseConfig.serviceProvider});
    }

    std::string url;
    if (isUdpType(type)) {
        addMpegTsMuxArgs(args, outputConfig, cbrTs);
        url = udpOutputUrl(outputConfig);
    } else if (type == "srt") {
        addMpegTsMuxArgs(args, outputConfig, cbrTs);
        url = srtOutputUrl(outputConfig);
    } else if (type == "http") {
        args.insert(args.end(), {"-listen", "1"});
        addMpegTsMuxArgs(args, outputConfig, cbrTs);
        url = httpOutputUrl(outputConfig);
    } else if (hlsOutput) {
        std::filesystem::create_directories(hlsDirectory(outputConfig));
        const std::string directory = hlsDirectory(outputConfig);
        args.insert(args.end(), {
            "-f", "hls",
            "-hls_time", "4",
            "-hls_list_size", "8",
            "-hls_flags", "delete_segments+omit_endlist+program_date_time",
            "-hls_segment_filename", directory + "/segment%05d.ts"
        });
        url = directory + "/playlist.m3u8";
    } else if (flvOutput) {
        args.insert(args.end(), {"-f", "flv"});
        url = rtmpOutputLocation(outputConfig);
    } else {
        addMpegTsMuxArgs(args, outputConfig, cbrTs);
        url = udpOutputUrl(outputConfig);
    }

    args.push_back(url);
    description = type + "@" + url;
    return args;
}

bool FfmpegTranscoderProcess::start(const StreamConfig& config, std::string& error) {
    stop();
    stopping = false;

    std::string ffmpegPath;
    if (!isAvailable(&ffmpegPath)) {
        error = "ffmpeg executable was not found in PATH";
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
        std::cerr << "FFmpeg transcoder command: " << commandLineForLog(args) << std::endl;
        std::cerr << "FFmpeg transcoder started pid=" << child.pid
                  << " output=" << description << std::endl;
        started.push_back(child);
    }

    children = std::move(started);
    return true;
}

void FfmpegTranscoderProcess::stop() {
    stopping = true;
    for (auto& child : children) {
        if (child.pid <= 0) continue;
        int status = 0;
        pid_t done = ::waitpid(child.pid, &status, WNOHANG);
        if (done == 0) {
            ::kill(child.pid, SIGTERM);
            for (int i = 0; i < 20; ++i) {
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

bool FfmpegTranscoderProcess::isRunning() {
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
            std::cerr << "FFmpeg transcoder exited pid=" << child.pid
                      << " output=" << child.description
                      << " status=" << status << std::endl;
            child.pid = -1;
        }
    }
    return anyRunning;
}

std::string FfmpegTranscoderProcess::description() const {
    std::ostringstream ss;
    for (size_t i = 0; i < children.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << children[i].description;
    }
    return ss.str();
}
