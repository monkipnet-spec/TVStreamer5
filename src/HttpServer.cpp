#include "HttpServer.h"

#include "utils.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <unistd.h>

namespace {

std::string queryValue(const std::string& target, const std::string& key) {
    const auto queryPos = target.find('?');
    if (queryPos == std::string::npos) {
        return "";
    }

    std::string query = target.substr(queryPos + 1);
    std::istringstream stream(query);
    std::string part;
    while (std::getline(stream, part, '&')) {
        const auto eq = part.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (part.substr(0, eq) == key) {
            return part.substr(eq + 1);
        }
    }
    return "";
}

int64_t unixNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string cleanPathToken(const std::string& value, bool allowDot = false) {
    std::string cleaned;
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || (allowDot && ch == '.')) {
            cleaned.push_back(ch);
        }
    }
    return cleaned;
}

std::string base64Decode(const std::string& value) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    int buffer = 0;
    int bits = -8;

    for (unsigned char ch : value) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const auto pos = alphabet.find(static_cast<char>(ch));
        if (pos == std::string::npos) {
            return "";
        }
        buffer = (buffer << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return decoded;
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }

    unsigned char diff = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

std::string advertisedHost(const StreamConfig& cfg) {
    if (cfg.outputHost.empty() || cfg.outputHost == "0.0.0.0" || cfg.outputHost == "::") {
        if (!cfg.interfaceAddress.empty()) {
            return cfg.interfaceAddress;
        }
        const auto interfaces = enumerateNetworkInterfaces();
        if (!interfaces.empty()) {
            return interfaces.front().address;
        }
        return "127.0.0.1";
    }
    return cfg.outputHost;
}

std::string streamLink(const StreamConfig& cfg, int httpPort) {
    const std::string type = toLower(cfg.outputType);
    if (type == "srt") {
        const std::string mode = toLower(cfg.outputMode) == "caller" ? "listener" : "caller";
        return "srt://" + advertisedHost(cfg) + ":" + std::to_string(cfg.outputPort) + "?mode=" + mode;
    }
    if (type == "youtube") {
        const std::string hostLower = toLower(cfg.outputHost);
        return hostLower.rfind("rtmp", 0) == 0
            ? cfg.outputHost
            : "rtmp://a.rtmp.youtube.com/live2/" + cfg.outputHost;
    }
    if (type == "rtmp") {
        const std::string hostLower = toLower(cfg.outputHost);
        return hostLower.rfind("rtmp", 0) == 0
            ? cfg.outputHost
            : "rtmp://" + advertisedHost(cfg) + ":" + std::to_string(cfg.outputPort) + "/live/" + cfg.id;
    }
    if (type == "http") {
        return "http://" + advertisedHost(cfg) + ":" + std::to_string(httpPort) + "/stream/" + cfg.id + ".ts";
    }
    if (type == "hls") {
        return "http://" + advertisedHost(cfg) + ":" + std::to_string(httpPort) + "/hls/" + cfg.id + "/playlist.m3u8";
    }
    return "udp://@" + cfg.outputHost + ":" + std::to_string(cfg.outputPort);
}

} // namespace

HttpServer::HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm)
    : acceptor(ioc), configManager(cfg), streamManager(sm) {
}

bool HttpServer::start() {
    return bindHttpPort(configManager.config.httpPort);
}

void HttpServer::doAccept() {
    if (!acceptor.is_open()) {
        return;
    }

    const uint64_t generation = acceptGeneration.load();
    acceptor.async_accept([this, generation](boost::system::error_code ec, tcp::socket socket) {
        if (generation != acceptGeneration.load()) {
            return;
        }
        if (!ec) {
            std::thread(&HttpServer::handleSession, this, std::move(socket)).detach();
        }
        if (acceptor.is_open()) {
            doAccept();
        }
    });
}

void HttpServer::handleSession(tcp::socket socket) {
    try {
        boost::beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(socket, buffer, req);
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "TVStreamer5");
        res.set(http::field::content_type, "text/html; charset=UTF-8");
        res.keep_alive(req.keep_alive());

        const std::string target(req.target());
        if (requiresAuthentication(target) && !isAuthorized(req)) {
            writeUnauthorized(res);
            res.content_length(res.body().size());
            http::write(socket, res);
            return;
        }

        if (req.method() == http::verb::get) {
            if (target.rfind("/stream/", 0) == 0 && handleHttpStream(socket, target)) {
                return;
            } else if (target.rfind("/hls/", 0) == 0 && serveHlsFile(target, res)) {
                // serveHlsFile filled the response.
            } else if (target == "/" || target == "/index.html") {
                res.body() = renderIndexPage();
            } else if (target == "/api/interfaces") {
                res.set(http::field::content_type, "application/json");
                res.body() = listInterfaces();
            } else if (target == "/api/state") {
                res.set(http::field::content_type, "application/json");
                res.body() = currentState();
            } else if (target.rfind("/api/quality-history", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                res.body() = qualityHistory(target);
            } else if (target == "/health") {
                res.set(http::field::content_type, "text/plain");
                res.body() = "Healthy";
            } else {
                res.result(http::status::not_found);
                res.body() = "Not Found";
            }
        } else if (req.method() == http::verb::post) {
            if (target == "/api/save-config") {
                handleSaveConfig(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/start-stream") {
                handleStartStream(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/stop-stream") {
                handleStopStream(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
            } else {
                res.result(http::status::not_found);
                res.body() = "Not Found";
            }
        } else {
            res.result(http::status::method_not_allowed);
            res.body() = "Method Not Allowed";
        }

        res.content_length(res.body().size());
        http::write(socket, res);
    } catch (const std::exception& ex) {
        std::cerr << "HTTP session failed: " << ex.what() << std::endl;
    }
}

bool HttpServer::requiresAuthentication(const std::string& target) const {
    return target != "/health" &&
           target.rfind("/stream/", 0) != 0 &&
           target.rfind("/hls/", 0) != 0;
}

bool HttpServer::isAuthorized(const http::request<http::string_body>& req) const {
    const auto auth = req.find(http::field::authorization);
    if (auth == req.end()) {
        return false;
    }

    const std::string header(auth->value());
    const std::string prefix = "Basic ";
    if (header.size() <= prefix.size() || !boost::algorithm::istarts_with(header, prefix)) {
        return false;
    }

    const std::string decoded = base64Decode(header.substr(prefix.size()));
    const auto separator = decoded.find(':');
    if (separator == std::string::npos) {
        return false;
    }

    const std::string login = decoded.substr(0, separator);
    const std::string password = decoded.substr(separator + 1);
    return constantTimeEquals(login, configManager.config.login) &&
           constantTimeEquals(password, configManager.config.password);
}

void HttpServer::writeUnauthorized(http::response<http::string_body>& res) const {
    res.result(http::status::unauthorized);
    res.set(http::field::www_authenticate, "Basic realm=\"TVStreamer5\"");
    res.set(http::field::content_type, "text/plain; charset=UTF-8");
    res.body() = "Unauthorized";
}

bool HttpServer::bindHttpPort(int port) {
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid HTTP port: " << port << std::endl;
        return false;
    }

    boost::system::error_code ec;
    ++acceptGeneration;
    if (acceptor.is_open()) {
        acceptor.cancel(ec);
        acceptor.close(ec);
    }

    tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port));
    acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        std::cerr << "HTTP server failed to open port " << port << ": " << ec.message() << std::endl;
        return false;
    }
    acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        std::cerr << "HTTP server failed to set reuse_address: " << ec.message() << std::endl;
        return false;
    }
    acceptor.bind(endpoint, ec);
    if (ec) {
        std::cerr << "HTTP server failed to bind port " << port << ": " << ec.message() << std::endl;
        return false;
    }
    acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        std::cerr << "HTTP server failed to listen on port " << port << ": " << ec.message() << std::endl;
        return false;
    }

    doAccept();
    std::cerr << "HTTP server listening on port " << port << std::endl;
    return true;
}

void HttpServer::rebindHttpPort(int port) {
    boost::asio::post(acceptor.get_executor(), [this, port]() {
        bindHttpPort(port);
    });
}

std::string HttpServer::listInterfaces() {
    Json::Value root;
    auto interfaces = enumerateNetworkInterfaces();
    for (auto& iface : interfaces) {
        Json::Value item;
        item["name"] = iface.name;
        item["address"] = iface.address;
        root.append(item);
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::currentState() {
    Json::Value root;
    root["login"] = configManager.config.login;
    root["server_name"] = configManager.config.serverName;
    root["http_port"] = configManager.config.httpPort;
    root["telegram_token"] = configManager.config.telegramToken;
    root["telegram_chat_id"] = configManager.config.telegramChatId;
    root["stream_count"] = Json::UInt(configManager.config.streams.size());
    root["active_count"] = Json::UInt(streamManager.activeStreams().size());
    Json::Value streams(Json::arrayValue);
    auto snap = streamManager.snapshot();
    for (const auto& cfg : configManager.config.streams) {
        Json::Value item = cfg.toJson();
        if (snap.count(cfg.id)) {
            auto* streamState = snap.at(cfg.id);
            item["active"] = streamState->active.load();
            item["status"] = streamState->statusMessage;
            item["using_backup"] = streamState->usingBackup;
            item["active_input_uri"] = streamState->activeInputUri.empty() ? cfg.inputUri : streamState->activeInputUri;
            item["active_input_label"] = streamState->usingBackup ? "Резерв" : "Основной";
            item["bitrate_in_kbps"] = Json::UInt64(streamState->inputBitrate.load() / 1000);
            item["bitrate_out_kbps"] = Json::UInt64(streamState->outputBitrate.load() / 1000);
            item["cc_errors"] = Json::UInt64(streamState->ccErrorsDelta.load());
            item["cc_errors_total"] = Json::UInt64(streamState->ccErrors.load());
        } else {
            item["active"] = false;
            item["status"] = "stopped";
            item["using_backup"] = false;
            item["active_input_uri"] = cfg.inputUri;
            item["active_input_label"] = "Основной";
            item["bitrate_in_kbps"] = Json::UInt64(0);
            item["bitrate_out_kbps"] = Json::UInt64(0);
            item["cc_errors"] = Json::UInt64(0);
            item["cc_errors_total"] = Json::UInt64(0);
        }
        item["vlc_link"] = streamLink(cfg, configManager.config.httpPort);
        recordQualitySample(cfg, item);
        streams.append(item);
    }
    root["streams"] = streams;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

bool HttpServer::handleHttpStream(tcp::socket& socket, const std::string& target) {
    const std::string prefix = "/stream/";
    if (target.size() <= prefix.size() + 3 || target.substr(target.size() - 3) != ".ts") {
        return false;
    }

    const std::string id = cleanPathToken(target.substr(prefix.size(), target.size() - prefix.size() - 3));
    if (id.empty()) {
        return false;
    }

    const std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Server: TVStreamer5\r\n"
        "Content-Type: video/MP2T\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    boost::asio::write(socket, boost::asio::buffer(header));
    int fd = socket.release();
    streamManager.addHttpClient(id, fd);
    return true;
}

bool HttpServer::serveHlsFile(const std::string& target, http::response<http::string_body>& res) {
    const std::string prefix = "/hls/";
    const auto slash = target.find('/', prefix.size());
    if (slash == std::string::npos) {
        return false;
    }

    const std::string id = cleanPathToken(target.substr(prefix.size(), slash - prefix.size()));
    const std::string rawFileName = target.substr(slash + 1);
    const std::string fileName = cleanPathToken(rawFileName, true);
    if (id.empty() || fileName.empty() || fileName != rawFileName || fileName.find("..") != std::string::npos) {
        return false;
    }

    const std::filesystem::path filePath =
        std::filesystem::path("/tmp/tvstreamer5-hls") / id / fileName;
    if (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath)) {
        res.result(http::status::not_found);
        res.set(http::field::content_type, "text/plain");
        res.body() = "Not Found";
        return true;
    }

    std::ifstream input(filePath, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    res.body() = buffer.str();
    if (filePath.extension() == ".m3u8") {
        res.set(http::field::content_type, "application/vnd.apple.mpegurl");
        res.set(http::field::cache_control, "no-cache");
    } else {
        res.set(http::field::content_type, "video/MP2T");
        res.set(http::field::cache_control, "no-cache");
    }
    return true;
}

std::string HttpServer::qualityHistory(const std::string& target) {
    const std::string id = queryValue(target, "id");
    uint64_t periodSeconds = 3600;
    try {
        const std::string period = queryValue(target, "period");
        if (!period.empty()) {
            periodSeconds = std::stoull(period);
        }
    } catch (const std::exception&) {
        periodSeconds = 3600;
    }
    periodSeconds = std::clamp<uint64_t>(periodSeconds, 60, 30ULL * 24ULL * 60ULL * 60ULL);

    Json::Value root;
    root["id"] = id;
    root["period_seconds"] = Json::UInt64(periodSeconds);
    root["generated_at"] = Json::Int64(unixNowSeconds());
    Json::Value samples(Json::arrayValue);

    const int64_t cutoff = unixNowSeconds() - static_cast<int64_t>(periodSeconds);
    std::map<std::string, unsigned int> totals = {
        {"ok", 0}, {"warn", 0}, {"error", 0}, {"offline", 0}
    };

    {
        std::lock_guard<std::mutex> lock(qualityMutex);
        auto found = qualitySamples.find(id);
        if (found != qualitySamples.end()) {
            for (const auto& sample : found->second) {
                if (sample.timestamp < cutoff) {
                    continue;
                }
                Json::Value item;
                item["ts"] = Json::Int64(sample.timestamp);
                item["active"] = sample.active;
                item["input_kbps"] = Json::UInt64(sample.inputKbps);
                item["output_kbps"] = Json::UInt64(sample.outputKbps);
                item["target_kbps"] = Json::UInt64(sample.targetKbps);
                item["cc_errors"] = Json::UInt64(sample.ccErrors);
                item["status"] = sample.status;
                item["level"] = sample.level;
                item["message"] = sample.message;
                samples.append(item);
                totals[sample.level]++;
            }
        }
    }

    root["samples"] = samples;
    Json::Value summary;
    summary["ok"] = Json::UInt(totals["ok"]);
    summary["warn"] = Json::UInt(totals["warn"]);
    summary["error"] = Json::UInt(totals["error"]);
    summary["offline"] = Json::UInt(totals["offline"]);
    uint64_t ccErrorsTotal = 0;
    for (const auto& sample : samples) {
        ccErrorsTotal += sample.get("cc_errors", Json::UInt64(0)).asUInt64();
    }
    summary["cc_errors"] = Json::UInt64(ccErrorsTotal);
    root["summary"] = summary;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

void HttpServer::recordQualitySample(const StreamConfig& cfg, const Json::Value& state) {
    const int64_t now = unixNowSeconds();
    QualitySample sample;
    sample.timestamp = now;
    sample.active = state.get("active", false).asBool();
    sample.inputKbps = state.get("bitrate_in_kbps", Json::UInt64(0)).asUInt64();
    sample.outputKbps = state.get("bitrate_out_kbps", Json::UInt64(0)).asUInt64();
    sample.targetKbps = cfg.targetBitrate / 1000;
    sample.ccErrors = state.get("cc_errors", Json::UInt64(0)).asUInt64();
    sample.status = state.get("status", "").asString();

    const std::string statusLower = toLower(sample.status);
    if (!sample.active) {
        sample.level = "offline";
        sample.message = sample.status == "stopped" ? "Поток остановлен" : "Поток не активен: " + sample.status;
    } else if (statusLower.find("error") != std::string::npos ||
               statusLower.find("failed") != std::string::npos ||
               statusLower.find("ended") != std::string::npos) {
        sample.level = "error";
        sample.message = "Ошибка GStreamer: " + sample.status;
    } else if (sample.inputKbps == 0) {
        sample.level = "warn";
        sample.message = "Нет входного битрейта при активном потоке";
    } else if (sample.ccErrors > 0) {
        sample.level = "error";
        sample.message = "CC-errors на входе MPEG-TS: " + std::to_string(sample.ccErrors);
    } else if (sample.targetKbps > 0 && sample.outputKbps > 0) {
        const double diff = std::abs(static_cast<double>(sample.outputKbps) - static_cast<double>(sample.targetKbps));
        const double deviation = diff / static_cast<double>(sample.targetKbps);
        if (deviation > 0.20) {
            sample.level = "warn";
            sample.message = "Выходной битрейт отклоняется от цели больше чем на 20%";
        } else {
            sample.level = "ok";
            sample.message = "Качество в норме";
        }
    } else {
        sample.level = "ok";
        sample.message = "Качество в норме";
    }

    std::lock_guard<std::mutex> lock(qualityMutex);
    auto& samples = qualitySamples[cfg.id];
    if (!samples.empty() && samples.back().timestamp == sample.timestamp) {
        samples.back() = sample;
    } else {
        samples.push_back(sample);
    }

    const int64_t cutoff = now - 30LL * 24LL * 60LL * 60LL;
    while (!samples.empty() && samples.front().timestamp < cutoff) {
        samples.pop_front();
    }
}

void HttpServer::handleSaveConfig(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        std::cerr << "Invalid config payload: " << errs << std::endl;
        return;
    }
    const int previousHttpPort = configManager.config.httpPort;
    AppConfig nextConfig = AppConfig::fromJson(root);
    if (!root.isMember("login") || root.get("login", "").asString().empty()) {
        nextConfig.login = configManager.config.login;
    }
    if (!root.isMember("password") || root.get("password", "").asString().empty()) {
        nextConfig.password = configManager.config.password;
    }
    if (!root.isMember("server_name") || root.get("server_name", "").asString().empty()) {
        nextConfig.serverName = configManager.config.serverName;
    }
    if (!root.isMember("http_port") || nextConfig.httpPort <= 0 || nextConfig.httpPort > 65535) {
        nextConfig.httpPort = configManager.config.httpPort;
    }
    configManager.config = nextConfig;
    configManager.save();
    if (configManager.config.httpPort != previousHttpPort) {
        rebindHttpPort(configManager.config.httpPort);
    }
}

void HttpServer::handleStartStream(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        std::cerr << "Invalid start-stream payload: " << errs << std::endl;
        return;
    }
    auto cfg = StreamConfig::fromJson(root);
    streamManager.startStream(cfg);
}

void HttpServer::handleStopStream(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        std::cerr << "Invalid stop-stream payload: " << errs << std::endl;
        return;
    }
    std::string id = root.get("id", "").asString();
    streamManager.stopStream(id);
}

void HttpServer::addEndpoint(const std::string& path, std::function<void(const boost::asio::ip::tcp::socket&)> handler) {
    // Store endpoint handler for future use
    // This is a simple implementation - in a real server you'd want proper routing
    endpointHandlers[path] = handler;
}


std::string HttpServer::renderIndexPage() {
    static const std::string html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="X-UA-Compatible" content="IE=edge">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TVStreamer5</title>
<style>
html{font-size:14px}
body{font-family:Arial,Helvetica,sans-serif;background:#0f1218;color:#EEE;margin:0;padding:0;min-height:100vh}
body:before{content:'';position:fixed;inset:0;background:radial-gradient(circle at top left,rgba(40,160,255,.18),transparent 28%),radial-gradient(circle at top right,rgba(120,90,255,.15),transparent 22%),linear-gradient(180deg,#10131a 0%,#090c12 100%);pointer-events:none;z-index:-1}
header{display:flex;align-items:center;justify-content:space-between;padding:8px 10px;background:rgba(19,23,31,.95);backdrop-filter:blur(10px);border-bottom:1px solid rgba(255,255,255,.06);gap:12px;flex-wrap:wrap}
.header-left{display:flex;align-items:flex-start;gap:10px}
.header-left .title{font-size:1.05rem;font-weight:700;letter-spacing:.02em;color:#fff}
.header-left .subtitle{font-size:.78rem;color:#9aa3b1;margin-top:2px}
.header-right{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.header-center{display:flex;align-items:center;justify-content:center;gap:12px}
.button-primary{padding:8px 14px;border:none;border-radius:999px;color:#FFF;background:#1f8bff;cursor:pointer;font-size:0.88rem;transition:background .2s ease}
.button-secondary{padding:7px 12px;border:1px solid rgba(255,255,255,.14);border-radius:999px;color:#EEE;background:rgba(255,255,255,.05);cursor:pointer;font-size:0.82rem;transition:background .2s ease,border-color .2s ease}
.button-primary:hover{background:#0f7ce7}
.button-secondary:hover{background:rgba(255,255,255,.1);border-color:rgba(255,255,255,.24)}
.container{padding:10px 12px 12px;max-width:1180px;margin:0 auto}
.stats-panel{display:grid;grid-template-columns:repeat(2,minmax(100px,1fr));gap:10px;padding:8px 12px;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.08);border-radius:16px}
.stats-panel .status{display:flex;flex-direction:column;gap:3px;font-size:.78rem;color:#d1d9ed}
.stats-panel .status strong{color:#fff;font-size:.78rem}
.stats-panel .status span{font-size:1rem;font-weight:700;color:#fff}
.tile-grid{display:grid;grid-template-columns:repeat(auto-fill, minmax(calc(180px * 1.15), 1fr));gap:12px 1ch;justify-content:start}
.tile{position:relative;background:rgba(22,27,37,.94);padding:10px 10px 10px 16px;border-radius:18px;border:1px solid rgba(255,255,255,.06);display:flex;flex-direction:column;gap:6px;min-height:130px;width:100%;max-width:none;box-sizing:border-box;box-shadow:0 18px 42px rgba(0,0,0,.14);transition:transform .2s ease,border-color .2s ease;font-size:11px}
.tile:before{content:'';position:absolute;left:0;top:12px;bottom:12px;width:4px;border-radius:999px;background:linear-gradient(180deg,#3fc8ff,#1d69ff)}
.tile:hover{transform:translateY(-1px);border-color:rgba(31,136,255,.3)}
.tile.active{border-color:#17c261}
.tile.error{border-color:#fb5f5f}
.tile .top{display:flex;align-items:center;justify-content:space-between;gap:6px}
.tile .title{font-size:11px;font-weight:700;line-height:1.2;color:#fff}
.tile .badge{padding:2px 5px;background:rgba(20,161,255,.14);color:#7dd1ff;border-radius:999px;font-size:11px;text-transform:uppercase;letter-spacing:.08em}
.tile .status-pill{padding:2px 6px;background:rgba(255,255,255,.06);color:#c9d2e4;border-radius:999px;font-size:11px;text-transform:uppercase;letter-spacing:.08em}
.tile .status-pill.active{background:rgba(23,194,97,.15);color:#b6f7c2}
.tile .status-pill.stopped{background:rgba(255,95,95,.14);color:#ffb3b3}
.tile .info{display:grid;grid-template-columns:1fr;gap:5px;font-size:11px;color:#b3b8c6}
.tile .info-row{display:flex;justify-content:space-between;gap:8px;align-items:center}
.tile .info-row strong{color:#fff;font-size:11px}
.tile .info-row span{max-width:140px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;text-align:right}
.tile .controls{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:6px}
.tile .controls button{padding:7px 8px;border:none;border-radius:10px;background:rgba(255,255,255,.06);color:#EEE;font-size:9px;cursor:pointer;transition:background .2s ease,transform .08s ease,box-shadow .2s ease}
.tile .controls button:hover{background:rgba(255,255,255,.12)}
.tile .controls button:active{transform:translateY(1px) scale(.98)}
.tile .controls .start-button{background:rgba(23,194,97,.18);color:#bdf8cb;box-shadow:inset 0 0 0 1px rgba(23,194,97,.26)}
.tile .controls .start-button:hover{background:rgba(23,194,97,.28)}
.tile .controls .stop-button{background:rgba(255,95,95,.18);color:#ffc2c2;box-shadow:inset 0 0 0 1px rgba(255,95,95,.28)}
.tile .controls .stop-button:hover{background:rgba(255,95,95,.28)}
.tile .controls .copy-button.copied{background:rgba(23,194,97,.38);color:#fff;box-shadow:0 0 0 2px rgba(23,194,97,.28)}
.tile .controls .copy-button.copy-error{background:rgba(255,184,77,.24);color:#ffe0a3;box-shadow:0 0 0 2px rgba(255,184,77,.22)}
.tile .controls .quality-button{background:rgba(57,189,248,.14);color:#bdefff;box-shadow:inset 0 0 0 1px rgba(57,189,248,.2)}
.tile .controls .quality-button:hover{background:rgba(57,189,248,.24)}
.modal{position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(8,10,15,.78);display:none;align-items:center;justify-content:center;padding:12px;z-index:20}
.modal.active{display:flex}
.modal-content{background:rgba(11,15,22,.985);padding:18px 18px;border-radius:22px;width:min(520px,100%);max-height:92%;overflow:auto;box-shadow:0 28px 70px rgba(0,0,0,.24);border:1px solid rgba(255,255,255,.08)}
.modal-content.quality-modal{width:min(920px,100%)}
.modal-content h2{margin-top:0;font-size:1.25rem;margin-bottom:14px;color:#fff}
.quality-head{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;flex-wrap:wrap;margin-bottom:10px}
.quality-title{display:flex;flex-direction:column;gap:4px}
.quality-title small{color:#9aa3b1}
.period-tabs{display:flex;gap:6px;flex-wrap:wrap}
.period-tabs button{padding:6px 8px;border:1px solid rgba(255,255,255,.1);background:rgba(255,255,255,.05);color:#d7deec;border-radius:8px;cursor:pointer;font-size:.72rem}
.period-tabs button.active{background:#1f8bff;color:#fff;border-color:#1f8bff}
.quality-board{position:relative;border:1px solid rgba(255,255,255,.08);background:#101722;border-radius:10px;padding:8px}
.quality-board canvas{display:block;width:100%;height:320px;cursor:copy}
.quality-legend{display:flex;gap:10px;flex-wrap:wrap;margin:10px 0;color:#cfd8ea;font-size:.78rem}
.quality-legend span{display:flex;align-items:center;gap:5px}
.quality-dot{width:9px;height:9px;border-radius:50%;display:inline-block}
.quality-ok{background:#17c261}.quality-warn{background:#ffbd4a}.quality-error{background:#ff5f5f}.quality-offline{background:#7c879b}
.quality-line{width:22px;height:3px;border-radius:999px;display:inline-block}
.quality-input{background:#58a6ff}.quality-output{background:#17c261}.quality-cc{background:#ff5f5f}
.quality-decode{display:grid;gap:5px;margin:8px 0 10px;padding:8px 10px;background:rgba(255,255,255,.045);border:1px solid rgba(255,255,255,.07);border-radius:8px;color:#cfd8ea;font-size:.78rem;line-height:1.35}
.quality-decode strong{color:#fff}
.quality-details{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:8px;margin-top:10px}
.quality-card{background:rgba(255,255,255,.045);border:1px solid rgba(255,255,255,.07);border-radius:8px;padding:8px;color:#cfd8ea;font-size:.78rem}
.quality-card strong{display:block;color:#fff;margin-bottom:4px}
.quality-errors{margin-top:10px;max-height:150px;overflow:auto;border-top:1px solid rgba(255,255,255,.08);padding-top:8px;color:#cfd8ea;font-size:.78rem}
.quality-errors div{display:flex;gap:8px;padding:3px 0}
.quality-empty{padding:30px;text-align:center;color:#9aa3b1}
.quality-copy{color:#7dd1ff;font-size:.78rem;min-height:18px;margin-top:-4px}
.form-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.form-grid.full{grid-template-columns:1fr}
.form-row.full{grid-column:1/-1}
.form-row{display:flex;flex-direction:column;gap:8px;align-items:flex-start}
.form-row label{font-size:.78rem;color:#9aa3b1}
.form-row input,.form-row select{width:100%;max-width:210px;padding:7px 9px;background:#121825;border:1px solid rgba(255,255,255,.08);border-radius:8px;color:#EEE;font-size:.8rem}
.form-row input.compact,.form-row select.compact{max-width:150px}
.row-inline{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.row-inline.compact-row input{width:100%;padding:7px 9px}
.form-row-inline small-field input{width:calc(100% - 8px)}
.form-row .checkbox-inline{display:flex;align-items:center;gap:10px;margin-top:8px}
.form-row .checkbox-inline input{width:16px;height:16px}
.modal-actions{display:flex;justify-content:flex-end;gap:10px;margin-top:16px}
.modal-actions button{min-width:100px;padding:8px 12px}
.about-list{display:grid;gap:10px;margin:4px 0 0}
.about-row{display:grid;grid-template-columns:120px 1fr;gap:12px;padding:9px 0;border-bottom:1px solid rgba(255,255,255,.08);font-size:.9rem}
.about-row:last-child{border-bottom:none}
.about-row strong{color:#9aa3b1;font-weight:600}
.about-row span,.about-row a{color:#fff;text-decoration:none;overflow-wrap:anywhere}
.about-row a:hover{color:#7dd1ff}
</style>
</head>
<body>
<header>
<div class="header-left">
<div>
<div class="title">Control Panel</div>
<div class="subtitle">Мониторинг трансляций и управление потоками</div>
</div>
</div>
<div class="header-center">
<div class="stats-panel">
<div class="status"><strong>Всего:</strong> <span id="totalCount">0</span></div>
<div class="status"><strong>Активно:</strong> <span id="activeCount">0</span></div>
</div>
</div>
<div class="header-right">
<button class="button-secondary" onclick="openLoginModal()">Пользователь</button>
<button class="button-secondary" onclick="openTelegramModal()">Telegram API</button>
<button class="button-primary" onclick="openStreamModal()">+ Добавить поток</button>
<button class="button-secondary" onclick="openAboutModal()">About</button>
</div>
</header>
<div class="container">
<div id="tiles" class="tile-grid"></div>
<div id="modal" class="modal">
<div class="modal-content" id="modalContent"></div>
</div>
<script>
let state = {};
function fetchState() {
  fetch('/api/state').then(r=>r.json()).then(data=>{state=data; render();});
}
function openModal(html) {
  document.getElementById('modalContent').innerHTML = html;
  document.getElementById('modalContent').className = 'modal-content';
  document.getElementById('modal').classList.add('active');
}
function closeModal() { document.getElementById('modal').classList.remove('active'); }
function render() {
  document.getElementById('totalCount').textContent = state.stream_count;
  document.getElementById('activeCount').textContent = state.active_count;
  const tiles = document.getElementById('tiles');
  tiles.innerHTML = '';
  state.streams.forEach(stream => {
    const tile = document.createElement('div');
    tile.className = 'tile' + (stream.active ? ' active' : '');
    tile.innerHTML = `
      <div class="top">
        <div>
          <div class="title">${stream.name || stream.id}</div>
          <div class="status-pill ${stream.active ? 'active' : 'stopped'}">${stream.active ? (stream.using_backup ? 'Backup' : 'Online') : 'Offline'}</div>
        </div>
        <div class="badge">${stream.cbr ? 'CBR' : 'VBR'}</div>
      </div>
      <div class="info">
        <div class="info-row"><strong>Вывод</strong><span>${(stream.output_type || 'udp').toUpperCase()} · ${stream.vlc_link || (stream.output_host + ':' + stream.output_port)}</span></div>
        <div class="info-row"><strong>Активный вход</strong><span>${stream.active_input_label || 'Основной'} · ${stream.active_input_uri || stream.input_uri || '—'}</span></div>
        <div class="info-row"><strong>Основной</strong><span>${stream.input_uri || '—'}</span></div>
        <div class="info-row"><strong>Резерв</strong><span>${stream.backup_input_uri || '—'}</span></div>
        <div class="info-row"><strong>SID</strong><span>${stream.service_id || '—'}</span></div>
        <div class="info-row"><strong>Bitrate In</strong><span>${stream.bitrate_in_kbps ? stream.bitrate_in_kbps + ' kbps' : '—'}</span></div>
        <div class="info-row"><strong>Bitrate Out</strong><span>${stream.bitrate_out_kbps ? stream.bitrate_out_kbps + ' kbps' : '—'}</span></div>
        <div class="info-row"><strong>Статус</strong><span>${stream.status}</span></div>
      </div>
      <div class="controls">
        <button class="${stream.active ? 'stop-button' : 'start-button'}" onclick="toggleStream('${stream.id}', ${stream.active})">${stream.active ? 'Стоп' : 'Старт'}</button>
        <button onclick="editStream('${stream.id}')">Ред.</button>
        <button class="quality-button" onclick="openQualityModal('${stream.id}')">График</button>
        <button class="copy-button" onclick="copyLink('${stream.vlc_link}', this)">URL</button>
      </div>`;
    tiles.appendChild(tile);
  });
}
function toggleStream(id, active) {
  const url = active ? '/api/stop-stream' : '/api/start-stream';
  const body = active ? {id} : state.streams.find(s=>s.id===id);
  fetch(url, {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(()=>{
      setTimeout(fetchState,500);
      setTimeout(fetchState,1500);
    });
}
function editStream(id) {
  const stream = state.streams.find(s=>s.id===id);
  if (!stream) return;
  openStreamForm(stream);
}
function openAboutModal() {
  openModal(`
    <h2>About</h2>
    <div class="about-list">
      <div class="about-row"><strong>Имя</strong><span>Лукомский Виталий</span></div>
      <div class="about-row"><strong>Страна</strong><span>Беларусь, г. Борисов</span></div>
      <div class="about-row"><strong>Email</strong><a href="mailto:monkipnet@gmail.com">monkipnet@gmail.com</a></div>
    </div>
    <div class="modal-actions">
      <button class="button-primary" onclick="closeModal()">Закрыть</button>
    </div>
  `);
}
function openLoginModal() {
  openModal(`
    <h2>Пользователь</h2>
    <div class="form-grid full">
      <div class="form-row"><label>Login</label><input id="login" value="${state.login||''}" /></div>
      <div class="form-row"><label>Новый пароль</label><input id="password" type="password" placeholder="Оставьте пустым, чтобы не менять" /></div>
      <div class="form-row"><label>Имя сервера</label><input id="serverName" value="${state.server_name||''}" /></div>
      <div class="form-row"><label>Порт web-интерфейса</label><input id="httpPort" type="number" min="1" max="65535" value="${state.http_port||9000}" /></div>
    </div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">Отмена</button>
      <button class="button-primary" onclick="saveSettings()">Сохранить</button>
    </div>
  `);
}
function openTelegramModal() {
  openModal(`
    <h2>Telegram API</h2>
    <div class="form-grid full">
      <div class="form-row"><label>Token</label><input id="telegramToken" value="${state.telegram_token||''}" /></div>
      <div class="form-row"><label>Chat ID</label><input id="telegramChatId" value="${state.telegram_chat_id||''}" /></div>
    </div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">Отмена</button>
      <button class="button-primary" onclick="saveSettings()">Сохранить</button>
    </div>
  `);
}
function openStreamModal() {
  openStreamForm({
    id: 'stream-' + Date.now(),
    name:'', input_uri:'', backup_input_uri:'', output_type:'udp', output_mode:'listener', output_host:'127.0.0.1', output_port:1234,
    interface_address:'', input_mode:'auto', auto_start:false, remap_enabled:false, cbr:true, target_bitrate:2000000,
    audio_pid:0, video_pid:0, service_id:1, service_name:'', service_provider:''
  });
}
function openStreamForm(stream) {
  const renderStreamForm = () => {
    const ifaceOptions = state.interfaces || [];
    const options = ifaceOptions.map(i=>`<option value="${i.address}" ${i.address===stream.interface_address?'selected':''}>${i.name} (${i.address})</option>`).join('');
    const outputType = stream.output_type || 'udp';
    openModal(`
      <h2>${stream.name ? 'Редактирование трансляции' : 'Настройка трансляции'}</h2>
      <div class="form-grid">
        <div class="form-row full"><label>Имя плитки</label><input class="compact" id="streamName" value="${stream.name||''}" placeholder="Belarus 5" /></div>
        <div class="form-row full"><label>Входной URL (Основной)</label><input class="compact" id="streamInput" value="${stream.input_uri||''}" placeholder="rtsp://camera/live, udp://127.0.0.1:9087, rtmp://camera/live/stream или https://host/live.m3u8" /></div>
        <div class="form-row full"><label>Входной URL (Резервный)</label><input class="compact" id="streamBackupInput" value="${stream.backup_input_uri||''}" placeholder="http://192.168.1.2/..." /></div>
        <div class="form-row full"><label>Интерфейс вывода</label><select class="compact" id="streamInterface" onchange="syncOutputHostWithInterface()"><option value="">Auto / все интерфейсы</option>${options}</select></div>
        <div class="form-row"><label>Режим входа</label><select class="compact" id="streamInputMode"><option value="auto" ${(!stream.input_mode || stream.input_mode==='auto')?'selected':''}>Auto</option><option value="hls" ${stream.input_mode==='hls'?'selected':''}>HLS</option><option value="caller" ${stream.input_mode==='caller'?'selected':''}>SRT Caller</option><option value="listener" ${stream.input_mode==='listener'?'selected':''}>SRT Listener</option></select></div>
        <div class="form-row"><label>Формат выхода</label><select class="compact" id="streamOutputType" onchange="updateOutputHints()"><option value="udp" ${outputType==='udp'?'selected':''}>UDP MPEG-TS</option><option value="srt" ${outputType==='srt'?'selected':''}>SRT</option><option value="http" ${outputType==='http'?'selected':''}>HTTP TS</option><option value="hls" ${outputType==='hls'?'selected':''}>HLS</option><option value="rtmp" ${outputType==='rtmp'?'selected':''}>RTMP Push</option><option value="youtube" ${outputType==='youtube'?'selected':''}>YouTube</option></select></div>
        <div class="form-row" id="streamOutputModeRow"><label>Режим SRT выхода</label><select class="compact" id="streamOutputMode" onchange="updateOutputHints()"><option value="listener" ${(!stream.output_mode || stream.output_mode==='listener')?'selected':''}>SRT Listener</option><option value="caller" ${stream.output_mode==='caller'?'selected':''}>SRT Caller</option></select></div>
        <div class="form-row"><label id="streamOutputHostLabel">Адрес выхода</label><input class="compact" id="streamOutputHost" value="${stream.output_host||'239.0.0.1'}" placeholder="239.0.0.1" /></div>
        <div class="form-row"><label id="streamOutputPortLabel">Порт</label><input class="compact" id="streamOutputPort" type="number" value="${stream.output_port||1234}" placeholder="1234" /></div>
        <div class="form-row full"><label>URL для плеера</label><input class="compact" id="streamPreviewUrl" readonly value="${stream.vlc_link||''}" placeholder="Ссылка появится после сохранения" /></div>
        <div class="form-row full"><label>V-PID / A-PID</label><div class="row-inline compact-row"><input class="compact" id="streamAudioPid" type="number" value="${stream.audio_pid||257}" placeholder="257" /><input class="compact" id="streamVideoPid" type="number" value="${stream.video_pid||258}" placeholder="258" /></div></div>
        <div class="form-row"><label>SID</label><input class="compact" id="streamServiceId" type="number" value="${stream.service_id||1}" placeholder="1" /></div>
        <div class="form-row full"><label>Имя Канала и Провайдер</label><div class="row-inline compact-row"><input class="compact" id="streamServiceName" value="${stream.service_name||''}" placeholder="Belarus 5" /><input class="compact" id="streamProvider" value="${stream.service_provider||''}" placeholder="BTRC" /></div></div>
        <div class="form-row full"><label>Target bitrate (кбит/с)</label><input id="streamBitrate" type="number" value="${Math.round((stream.target_bitrate||2000000)/1000)}" placeholder="2000" /></div>
        <div class="form-row full"><label>Автозапуск</label><div class="checkbox-inline"><input id="streamAutoStart" type="checkbox" ${stream.auto_start ? 'checked' : ''} /><span>Запускать после перезапуска программы</span></div></div>
        <div class="form-row full"><label>Включить CBR</label><div class="checkbox-inline"><input id="streamCbr" type="checkbox" ${stream.cbr ? 'checked' : ''} /><span>CBR</span></div></div>
        <div class="form-row full"><label>Включить Remap</label><div class="checkbox-inline"><input id="streamRemapEnabled" type="checkbox" ${stream.remap_enabled ? 'checked' : ''} /><span>Remap PID / Service</span></div></div>
      </div>
      <div class="modal-actions">
        <button class="button-secondary" onclick="closeModal()">Отмена</button>
        <button class="button-primary" onclick="saveStream('${stream.id}')">Сохранить</button>
      </div>
    `);
    document.getElementById('streamCbr').checked = stream.cbr;
    updateOutputHints();
  };

  if (!state.interfaces || !state.interfaces.length) {
    loadInterfaces().then(renderStreamForm);
  } else {
    renderStreamForm();
  }
}
function updateOutputHints() {
  const type = document.getElementById('streamOutputType')?.value || 'udp';
  const hostLabel = document.getElementById('streamOutputHostLabel');
  const portLabel = document.getElementById('streamOutputPortLabel');
  const host = document.getElementById('streamOutputHost');
  const port = document.getElementById('streamOutputPort');
  const outputModeRow = document.getElementById('streamOutputModeRow');
  const outputMode = document.getElementById('streamOutputMode')?.value || 'listener';
  if (!hostLabel || !portLabel || !host || !port) return;
  if (outputModeRow) outputModeRow.style.display = type === 'srt' ? '' : 'none';
  if (type === 'http' || type === 'hls') {
    hostLabel.textContent = 'Адрес для ссылки';
    portLabel.textContent = 'Порт панели';
    port.value = state.http_port || port.value || 9000;
    port.disabled = true;
    host.placeholder = 'IP интерфейса или DNS';
  } else if (type === 'srt') {
    hostLabel.textContent = outputMode === 'caller' ? 'SRT сервер' : 'SRT host для ссылки';
    portLabel.textContent = 'SRT порт';
    port.disabled = false;
    host.placeholder = outputMode === 'caller' ? 'server.example.com или IP' : '0.0.0.0 для listener';
    if (outputMode === 'listener' && (!host.value || host.value === '127.0.0.1' || host.value === '239.0.0.1')) {
      host.value = '0.0.0.0';
    } else if (outputMode === 'caller' && (!host.value || host.value === '0.0.0.0' || host.value === '239.0.0.1')) {
      host.value = '127.0.0.1';
    }
  } else if (type === 'youtube') {
    hostLabel.textContent = 'YouTube key / URL';
    portLabel.textContent = 'Порт';
    port.disabled = true;
    host.placeholder = 'xxxx-xxxx-xxxx-xxxx или rtmp://a.rtmp.youtube.com/live2/...';
  } else if (type === 'rtmp') {
    hostLabel.textContent = 'RTMP URL / host';
    portLabel.textContent = 'RTMP порт';
    port.disabled = false;
    host.placeholder = 'rtmp://server/app/key или server.example.com';
  } else {
    hostLabel.textContent = 'Мультикаст / UDP IP';
    portLabel.textContent = 'UDP порт';
    port.disabled = false;
    host.placeholder = '239.0.0.1';
  }
  syncOutputHostWithInterface();
}
function syncOutputHostWithInterface() {
  const type = document.getElementById('streamOutputType')?.value || 'udp';
  const iface = document.getElementById('streamInterface')?.value || '';
  const host = document.getElementById('streamOutputHost');
  if (!host || !iface || (type !== 'http' && type !== 'hls')) return;
  if (!host.value || host.value === '0.0.0.0' || host.value === '127.0.0.1') {
    host.value = iface;
  }
}
function saveSettings() {
  const httpPortInput = document.getElementById('httpPort');
  const httpPort = httpPortInput ? Number(httpPortInput.value || 9000) : state.http_port;
  const previousHttpPort = Number(state.http_port || window.location.port || 9000);
  const payload = {
    login: document.getElementById('login')?.value || state.login,
    server_name: document.getElementById('serverName')?.value || state.server_name,
    telegram_token: document.getElementById('telegramToken')?.value || state.telegram_token,
    telegram_chat_id: document.getElementById('telegramChatId')?.value || state.telegram_chat_id,
    http_port: httpPort,
    streams: state.streams
  };
  const password = document.getElementById('password')?.value;
  if (password) payload.password = password;
  fetch('/api/save-config', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
    .then(()=>{
      if (httpPortInput && httpPort && httpPort !== previousHttpPort) {
        const nextUrl = new URL(window.location.href);
        nextUrl.port = String(httpPort);
        setTimeout(() => { window.location.href = nextUrl.toString(); }, 400);
        return;
      }
      closeModal();
      fetchState();
    });
}
function saveStream(id) {
  const payload = {
    id: id,
    name: document.getElementById('streamName').value,
    input_uri: document.getElementById('streamInput').value,
    output_type: document.getElementById('streamOutputType').value,
    output_mode: document.getElementById('streamOutputMode')?.value || 'listener',
    output_host: document.getElementById('streamOutputHost').value,
    output_port: Number(document.getElementById('streamOutputPort').value),
    backup_input_uri: document.getElementById('streamBackupInput').value,
    interface_address: document.getElementById('streamInterface').value,
    input_mode: document.getElementById('streamInputMode').value,
    auto_start: document.getElementById('streamAutoStart').checked,
    remap_enabled: document.getElementById('streamRemapEnabled').checked,
    cbr: document.getElementById('streamCbr').checked,
    target_bitrate: Number(document.getElementById('streamBitrate').value) * 1000,
    audio_pid: Number(document.getElementById('streamAudioPid').value),
    video_pid: Number(document.getElementById('streamVideoPid').value),
    service_id: Number(document.getElementById('streamServiceId').value),
    service_name: document.getElementById('streamServiceName').value,
    service_provider: document.getElementById('streamProvider').value
  };
  const existingIndex = state.streams.findIndex(s=>s.id===id);
  if (existingIndex >= 0) {
    state.streams[existingIndex] = payload;
  } else {
    state.streams.push(payload);
  }
  const savePayload = {
    login: state.login,
    server_name: state.server_name,
    telegram_token: state.telegram_token,
    telegram_chat_id: state.telegram_chat_id,
    http_port: state.http_port,
    streams: state.streams
  };
  fetch('/api/save-config', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(savePayload)})
    .then(()=>{closeModal();fetchState();});
}
function setCopyButtonState(button, className) {
  if (!button) return;
  button.classList.remove('copied', 'copy-error');
  button.classList.add(className);
  clearTimeout(button.copyStateTimer);
  button.copyStateTimer = setTimeout(() => {
    button.classList.remove('copied', 'copy-error');
  }, 1400);
}
function fallbackCopyText(text) {
  const input = document.createElement('textarea');
  input.value = text;
  input.setAttribute('readonly', '');
  input.style.position = 'fixed';
  input.style.left = '-9999px';
  input.style.top = '0';
  document.body.appendChild(input);
  input.focus();
  input.select();
  let ok = false;
  try {
    ok = document.execCommand('copy');
  } finally {
    document.body.removeChild(input);
  }
  return ok;
}
function copyLink(text, button) {
  const onSuccess = () => setCopyButtonState(button, 'copied');
  const onError = () => {
    if (fallbackCopyText(text)) {
      onSuccess();
    } else {
      setCopyButtonState(button, 'copy-error');
    }
  };

  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).then(onSuccess).catch(onError);
  } else {
    onError();
  }
}
const qualityPeriods = [
  {label:'Месяц', seconds:2592000},
  {label:'Неделя', seconds:604800},
  {label:'День', seconds:86400},
  {label:'Пол дня', seconds:43200},
  {label:'5 часов', seconds:18000},
  {label:'1 час', seconds:3600},
  {label:'30 минут', seconds:1800},
  {label:'10 минут', seconds:600},
  {label:'Минута', seconds:60}
];
let qualityChart = {streamId:'', period:3600, samples:[], points:[]};
function qualityColor(level) {
  return {ok:'#17c261', warn:'#ffbd4a', error:'#ff5f5f', offline:'#7c879b'}[level] || '#9aa3b1';
}
function formatTime(ts, period) {
  const date = new Date(ts * 1000);
  if (period >= 86400) {
    return date.toLocaleDateString([], {day:'2-digit', month:'2-digit'}) + ' ' +
      date.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'});
  }
  return date.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second: period <= 600 ? '2-digit' : undefined});
}
function openQualityModal(id, periodSeconds=3600) {
  const stream = state.streams.find(s=>s.id===id);
  if (!stream) return;
  qualityChart.streamId = id;
  qualityChart.period = periodSeconds;
  document.getElementById('modalContent').className = 'modal-content quality-modal';
  const tabs = qualityPeriods.map(p=>`<button class="${p.seconds===periodSeconds?'active':''}" onclick="loadQualityHistory('${id}', ${p.seconds})">${p.label}</button>`).join('');
  document.getElementById('modalContent').innerHTML = `
    <div class="quality-head">
      <div class="quality-title">
        <h2>Качество потока</h2>
        <small>${stream.name || stream.id} · ${stream.output_host}:${stream.output_port}</small>
      </div>
      <div class="period-tabs">${tabs}</div>
    </div>
    <div class="quality-board">
      <canvas id="qualityCanvas" width="860" height="320"></canvas>
    </div>
    <div class="quality-legend">
      <span><i class="quality-line quality-input"></i>Входной битрейт</span>
      <span><i class="quality-line quality-output"></i>Исходящий битрейт</span>
      <span><i class="quality-line quality-cc"></i>CC-errors</span>
      <span>Клик по графику копирует измерение в буфер</span>
    </div>
    <div class="quality-decode">
      <strong>Расшифровка</strong>
      <span>Синий - входной битрейт MPEG-TS на входе приложения.</span>
      <span>Зеленый - исходящий битрейт после обработки и отправки.</span>
      <span>Красный - CC-errors: разрывы continuity counter во входном MPEG-TS, обычно означают потерю/перестановку TS-пакетов.</span>
    </div>
    <div id="qualityCopyNotice" class="quality-copy"></div>
    <div id="qualityDetails" class="quality-details"></div>
    <div id="qualityErrors" class="quality-errors"></div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">Закрыть</button>
    </div>
  `;
  document.getElementById('modal').classList.add('active');
  loadQualityHistory(id, periodSeconds);
}
function loadQualityHistory(id, periodSeconds) {
  qualityChart.period = periodSeconds;
  fetch(`/api/quality-history?id=${encodeURIComponent(id)}&period=${periodSeconds}`)
    .then(r=>r.json())
    .then(data=>{
      qualityChart.samples = data.samples || [];
      renderQualityTabs(periodSeconds);
      drawQualityChart(data);
    });
}
function renderQualityTabs(periodSeconds) {
  document.querySelectorAll('.period-tabs button').forEach((button, index) => {
    button.classList.toggle('active', qualityPeriods[index]?.seconds === periodSeconds);
  });
}
function drawQualityChart(data) {
  const canvas = document.getElementById('qualityCanvas');
  const details = document.getElementById('qualityDetails');
  const errors = document.getElementById('qualityErrors');
  if (!canvas || !details || !errors) return;
  const ctx = canvas.getContext('2d');
  const rect = canvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.max(640, Math.floor(rect.width * ratio));
  canvas.height = Math.floor(320 * ratio);
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  const width = canvas.width / ratio;
  const height = canvas.height / ratio;
  ctx.clearRect(0, 0, width, height);
  const samples = data.samples || [];
  if (!samples.length) {
    ctx.fillStyle = '#9aa3b1';
    ctx.textAlign = 'center';
    ctx.fillText('История пока пустая. Данные появятся после нескольких обновлений состояния.', width / 2, height / 2);
    details.innerHTML = '<div class="quality-card"><strong>Нет данных</strong>История собирается в памяти во время работы приложения.</div>';
    errors.innerHTML = '';
    qualityChart.points = [];
    return;
  }
  const left = 54, right = 46, top = 16, bottom = 34;
  const plotW = width - left - right;
  const plotH = height - top - bottom;
  const endTs = data.generated_at || Math.floor(Date.now()/1000);
  const startTs = endTs - (data.period_seconds || qualityChart.period);
  const maxBitrate = Math.max(1000, ...samples.map(s=>Math.max(s.input_kbps || 0, s.output_kbps || 0))) * 1.15;
  const maxCcErrors = Math.max(1, ...samples.map(s=>s.cc_errors || 0));
  ctx.strokeStyle = 'rgba(255,255,255,.09)';
  ctx.fillStyle = '#8e99aa';
  ctx.font = '9px Arial';
  ctx.textAlign = 'right';
  for (let i=0;i<=4;i++) {
    const y = top + plotH * i / 4;
    ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(width - right, y); ctx.stroke();
    const kbps = Math.round(maxBitrate * (1 - i / 4));
    ctx.fillText(kbps + 'k', left - 7, y + 4);
  }
  ctx.textAlign = 'left';
  ctx.fillStyle = '#ff9c9c';
  for (let i=0;i<=4;i++) {
    const y = top + plotH * i / 4;
    const value = Math.round(maxCcErrors * (1 - i / 4));
    ctx.fillText(value + ' cc', width - right + 8, y + 4);
  }
  ctx.textAlign = 'center';
  ctx.fillStyle = '#8e99aa';
  for (let i=0;i<=6;i++) {
    const x = left + plotW * i / 6;
    const ts = startTs + (endTs - startTs) * i / 6;
    ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, top + plotH); ctx.stroke();
    ctx.fillText(formatTime(ts, data.period_seconds), x, height - 10);
  }
  const xFor = ts => left + ((ts - startTs) / Math.max(1, endTs - startTs)) * plotW;
  const yFor = kbps => top + plotH - (Math.min(kbps, maxBitrate) / maxBitrate) * plotH;
  const ccYFor = value => top + plotH - (Math.min(value, maxCcErrors) / maxCcErrors) * plotH;
  const drawLine = (field, color) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.beginPath();
    let started = false;
    samples.forEach(s => {
      const value = s[field] || 0;
      const x = xFor(s.ts);
      const y = yFor(value);
      if (!started) { ctx.moveTo(x, y); started = true; } else { ctx.lineTo(x, y); }
    });
    ctx.stroke();
  };
  drawLine('input_kbps', '#58a6ff');
  drawLine('output_kbps', '#17c261');
  const drawCcErrors = () => {
    ctx.strokeStyle = '#ff5f5f';
    ctx.fillStyle = 'rgba(255,95,95,.28)';
    ctx.lineWidth = 2;
    ctx.beginPath();
    let started = false;
    samples.forEach(s => {
      const value = s.cc_errors || 0;
      const x = xFor(s.ts);
      const y = ccYFor(value);
      if (value > 0) {
        ctx.fillRect(x - 2, y, 4, top + plotH - y);
      }
      if (!started) { ctx.moveTo(x, y); started = true; } else { ctx.lineTo(x, y); }
    });
    ctx.stroke();
  };
  drawCcErrors();
  qualityChart.points = [];
  const lastSample = samples[samples.length - 1] || {};
  samples.forEach(s => {
    const x = xFor(s.ts);
    const y = (s.cc_errors || 0) > 0 ? ccYFor(s.cc_errors || 0) : yFor(s.output_kbps || s.input_kbps || 0);
    ctx.fillStyle = (s.cc_errors || 0) > 0 ? '#ff5f5f' : qualityColor(s.level);
    ctx.beginPath();
    ctx.arc(x, y, (s.cc_errors || 0) > 0 || s.level !== 'ok' ? 5 : 3, 0, Math.PI * 2);
    ctx.fill();
    qualityChart.points.push({x, y, sample:s});
  });
  const summary = data.summary || {};
  details.innerHTML = `
    <div class="quality-card"><strong>Период</strong>${formatTime(startTs, data.period_seconds)} — ${formatTime(endTs, data.period_seconds)}</div>
    <div class="quality-card"><strong>Сэмплы</strong>${samples.length}</div>
    <div class="quality-card"><strong>Вход / выход</strong>${Math.round(lastSample.input_kbps || 0)} / ${Math.round(lastSample.output_kbps || 0)} kbps</div>
    <div class="quality-card"><strong>CC-errors</strong>${summary.cc_errors || 0} за период</div>
  `;
  const bad = samples.filter(s=>s.level !== 'ok' || (s.cc_errors || 0) > 0).slice(-30).reverse();
  errors.innerHTML = bad.length
    ? bad.map(s=>`<div><span style="color:${(s.cc_errors || 0) > 0 ? '#ff5f5f' : qualityColor(s.level)}">●</span><span>${formatTime(s.ts, data.period_seconds)}</span><span>${s.message} · CC: ${s.cc_errors || 0}</span></div>`).join('')
    : '<div><span style="color:#17c261">●</span><span>За выбранный период CC-errors и других ошибок нет</span></div>';
  canvas.onclick = ev => copyQualityPoint(ev, canvas);
}
function copyQualityPoint(ev, canvas) {
  if (!qualityChart.points.length) return;
  const rect = canvas.getBoundingClientRect();
  const x = ev.clientX - rect.left;
  const y = ev.clientY - rect.top;
  let nearest = qualityChart.points[0];
  let best = Number.MAX_VALUE;
  qualityChart.points.forEach(point => {
    const dist = Math.hypot(point.x - x, point.y - y);
    if (dist < best) { best = dist; nearest = point; }
  });
  const s = nearest.sample;
  const text = [
    `Время: ${formatTime(s.ts, qualityChart.period)}`,
    `Уровень: ${s.level}`,
    `Вход: ${s.input_kbps} kbps`,
    `Выход: ${s.output_kbps} kbps`,
    `CC-errors: ${s.cc_errors || 0}`,
    `Статус: ${s.status}`,
    `Расшифровка: ${s.message}`
  ].join('\n');
  const notice = document.getElementById('qualityCopyNotice');
  const show = message => {
    if (!notice) return;
    notice.textContent = message;
    clearTimeout(notice.copyTimer);
    notice.copyTimer = setTimeout(()=>{ notice.textContent = ''; }, 1800);
  };
  const onSuccess = () => show('Измерение скопировано в буфер обмена');
  const onError = () => {
    if (fallbackCopyText(text)) {
      onSuccess();
    } else {
      show('Не удалось скопировать измерение');
    }
  };
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).then(onSuccess).catch(onError);
  } else {
    onError();
  }
}
function loadInterfaces() {
  return fetch('/api/interfaces')
    .then(r=>r.json())
    .then(data=>{ state.interfaces=data; return data; })
    .catch(() => { state.interfaces=[]; return []; });
}
window.onload = () => {
  fetchState();
  loadInterfaces();
  setInterval(fetchState, 2000);
};
</script>
</body>
</html>
)HTML";
    return html;
}
