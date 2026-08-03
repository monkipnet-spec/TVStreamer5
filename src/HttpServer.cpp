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
#include <vector>
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

int validPortOrDefault(int port, int defaultPort) {
    return port > 0 && port <= 65535 ? port : defaultPort;
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
    if (type != "udp-vbr" && type != "udp-cbr" &&
        type != "srt" && type != "http" && type != "hls" &&
        type != "rtmp" && type != "youtube") {
        return "udp-vbr";
    }
    return type;
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

std::vector<StreamConfig> streamOutputs(const StreamConfig& cfg) {
    std::vector<StreamConfig> outputs;
    outputs.push_back(configForOutput(cfg, primaryOutputConfig(cfg)));
    for (const auto& output : cfg.additionalOutputs) {
        outputs.push_back(configForOutput(cfg, output));
    }
    return outputs;
}

int streamHttpPort(const StreamConfig& cfg, int defaultPort) {
    const std::string type = normalizedOutputType(cfg);
    if (type == "http" || type == "hls") {
        return validPortOrDefault(cfg.outputPort, defaultPort);
    }
    return defaultPort;
}

std::string streamLink(const StreamConfig& cfg, int httpPort) {
    const std::string type = normalizedOutputType(cfg);
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
        return "http://" + advertisedHost(cfg) + ":" + std::to_string(streamHttpPort(cfg, httpPort)) + "/stream/" + cfg.id + ".ts";
    }
    if (type == "hls") {
        return "http://" + advertisedHost(cfg) + ":" + std::to_string(streamHttpPort(cfg, httpPort)) + "/hls/" + cfg.id + "/playlist.m3u8";
    }
    return "udp://@" + cfg.outputHost + ":" + std::to_string(cfg.outputPort);
}

} // namespace

std::string extractStreamIdFromTarget(const std::string& target) {
    const std::string tsPrefix = "/stream/";
    if (target.rfind(tsPrefix, 0) == 0) {
        const auto start = tsPrefix.size();
        if (target.size() <= start + 3 || target.substr(target.size() - 3) != ".ts") {
            return "";
        }
        const auto end = target.find('.', start);
        return cleanPathToken(target.substr(start, end == std::string::npos ? std::string::npos : end - start));
    }

    const std::string hlsPrefix = "/hls/";
    if (target.rfind(hlsPrefix, 0) == 0) {
        const auto slash = target.find('/', hlsPrefix.size());
        if (slash == std::string::npos) {
            return "";
        }
        return cleanPathToken(target.substr(hlsPrefix.size(), slash - hlsPrefix.size()));
    }

    return "";
}

HttpServer::HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm)
    : ioContext(ioc), configManager(cfg), streamManager(sm) {
}

bool HttpServer::start() {
    return bindHttpPorts(configuredHttpPorts());
}

void HttpServer::doAccept(std::shared_ptr<tcp::acceptor> listener, int port, uint64_t generation) {
    if (!listener || !listener->is_open()) {
        return;
    }

    listener->async_accept([this, listener, port, generation](boost::system::error_code ec, tcp::socket socket) {
        if (generation != acceptGeneration.load()) {
            return;
        }
        if (!ec) {
            std::thread(&HttpServer::handleSession, this, std::move(socket)).detach();
        } else if (ec != boost::asio::error::operation_aborted) {
            std::cerr << "HTTP accept failed on port " << port << ": " << ec.message() << std::endl;
        }
        if (listener->is_open()) {
            doAccept(listener, port, generation);
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
        res.set(http::field::cache_control, "no-store");
        res.set(http::field::pragma, "no-cache");
        res.keep_alive(req.keep_alive());

        const std::string target(req.target());
        if (requiresAuthentication(target) && !isAuthorized(req)) {
            writeUnauthorized(res);
            res.content_length(res.body().size());
            http::write(socket, res);
            return;
        }

        if (req.method() == http::verb::get) {
          if (target.rfind("/stream/", 0) == 0) {
            if (!isStreamClientAllowed(socket, target)) {
              res.result(http::status::forbidden);
              res.set(http::field::content_type, "text/plain");
              res.body() = "Stream access denied";
            } else if (handleHttpStream(socket, target)) {
              return;
            }
            } else if (target.rfind("/hls/", 0) == 0) {
                if (!isStreamClientAllowed(socket, target)) {
                    res.result(http::status::forbidden);
                    res.set(http::field::content_type, "text/plain");
                    res.body() = "Stream access denied";
                } else if (serveHlsFile(socket, target, res)) {
                    // serveHlsFile filled the response.
                }
            } else if (target == "/" || target == "/index.html") {
                res.body() = renderIndexPage();
            } else if (target == "/api/interfaces") {
                res.set(http::field::content_type, "application/json");
                res.body() = listInterfaces();
            } else if (target == "/api/system-metrics") {
              res.set(http::field::content_type, "application/json");
              res.body() = systemMetrics();
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
            } else if (target == "/api/delete-stream") {
              handleDeleteStream(req.body());
              res.set(http::field::content_type, "application/json");
              res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/save-subscribers") {
                handleSaveSubscribers(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/reset-subscriber") {
              handleResetSubscriber(req.body());
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

bool HttpServer::isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const {
  if (!configManager.subscribers.filteringEnabled) {
    return true;
  }
  const std::string normalizedClientIp = normalizeIpAddress(clientIp);
  if (streamId.empty() || normalizedClientIp.empty()) {
    return false;
  }
  for (const auto& subscriber : configManager.subscribers.subscribers) {
    const std::string primaryIp = normalizeIpAddress(subscriber.primaryIp);
    const std::string backupIp = normalizeIpAddress(subscriber.backupIp);
    const bool ipMatches = subscriber.enabled &&
      (normalizedClientIp == primaryIp || (!backupIp.empty() && normalizedClientIp == backupIp));
    const bool streamMatches = std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), streamId) != subscriber.streamIds.end();
    if (ipMatches && streamMatches) {
      return true;
    }
  }
  return false;
}

bool HttpServer::isStreamClientAllowed(const tcp::socket& socket, const std::string& target) const {
  boost::system::error_code ec;
  const std::string clientIp = socket.remote_endpoint(ec).address().to_string();
  if (ec) {
    return false;
  }
  const std::string streamId = extractStreamIdFromTarget(target);
  return isClientAllowedForStream(streamId, clientIp);
}

void HttpServer::writeUnauthorized(http::response<http::string_body>& res) const {
    res.result(http::status::unauthorized);
    res.set(http::field::www_authenticate, "Basic realm=\"TVStreamer5\"");
    res.set(http::field::content_type, "text/plain; charset=UTF-8");
    res.body() = "Unauthorized";
}

std::set<int> HttpServer::configuredHttpPorts() const {
    std::set<int> ports;
    if (configManager.config.httpPort > 0 && configManager.config.httpPort <= 65535) {
        ports.insert(configManager.config.httpPort);
    }
    const int defaultPort = configManager.config.httpPort;
    for (const auto& stream : configManager.config.streams) {
        for (const auto& output : streamOutputs(stream)) {
            const std::string type = normalizedOutputType(output);
            if ((type == "http" || type == "hls") && output.outputPort > 0 && output.outputPort <= 65535) {
                ports.insert(streamHttpPort(output, defaultPort));
            }
        }
    }
    if (ports.empty()) {
        ports.insert(9000);
    }
    return ports;
}

bool HttpServer::bindHttpPorts(const std::set<int>& ports) {
    boost::system::error_code ec;
    const uint64_t generation = acceptGeneration.fetch_add(1) + 1;

    for (auto& [port, listener] : acceptors) {
        (void)port;
        if (listener && listener->is_open()) {
            listener->cancel(ec);
            listener->close(ec);
        }
    }

    std::unordered_map<int, std::shared_ptr<tcp::acceptor>> nextAcceptors;
    for (int port : ports) {
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid HTTP port: " << port << std::endl;
            continue;
        }

        auto listener = std::make_shared<tcp::acceptor>(ioContext);
        tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port));
        listener->open(endpoint.protocol(), ec);
        if (ec) {
            std::cerr << "HTTP server failed to open port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "HTTP server failed to set reuse_address on port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->bind(endpoint, ec);
        if (ec) {
            std::cerr << "HTTP server failed to bind port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "HTTP server failed to listen on port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }

        doAccept(listener, port, generation);
        nextAcceptors[port] = std::move(listener);
        std::cerr << "HTTP server listening on port " << port << std::endl;
    }

    const bool primaryBound = nextAcceptors.count(configManager.config.httpPort) > 0;
    acceptors = std::move(nextAcceptors);
    if (!primaryBound) {
        std::cerr << "HTTP server primary port is not listening: " << configManager.config.httpPort << std::endl;
    }
    return primaryBound;
}

void HttpServer::refreshHttpPorts() {
    boost::asio::post(ioContext, [this]() {
        bindHttpPorts(configuredHttpPorts());
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

  std::string HttpServer::systemMetrics() {
    uint64_t cpuTotal = 0;
    uint64_t cpuIdle = 0;
    {
      std::ifstream statFile("/proc/stat");
      std::string label;
      uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
      if (statFile >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal && label == "cpu") {
        cpuIdle = idle + iowait;
        cpuTotal = user + nice + system + idle + iowait + irq + softirq + steal;
      }
    }

    uint64_t memoryTotal = 0;
    uint64_t memoryAvailable = 0;
    {
      std::ifstream meminfo("/proc/meminfo");
      std::string label;
      uint64_t value = 0;
      std::string unit;
      while (meminfo >> label >> value >> unit) {
        if (label == "MemTotal:") memoryTotal = value;
        if (label == "MemAvailable:") memoryAvailable = value;
      }
    }

    Json::Value root;
    std::lock_guard<std::mutex> lock(metricsMutex);
    double cpuPercent = 0.0;
    const auto now = std::chrono::steady_clock::now();
    if (previousCpuTotal > 0 && cpuTotal > previousCpuTotal && cpuIdle >= previousCpuIdle) {
      const uint64_t totalDelta = cpuTotal - previousCpuTotal;
      const uint64_t idleDelta = cpuIdle - previousCpuIdle;
      cpuPercent = 100.0 * static_cast<double>(totalDelta - std::min(totalDelta, idleDelta)) / totalDelta;
    }
    previousCpuTotal = cpuTotal;
    previousCpuIdle = cpuIdle;
    const double elapsedSeconds = previousMetricsSample.time_since_epoch().count() == 0
      ? 0.0
      : std::chrono::duration<double>(now - previousMetricsSample).count();
    previousMetricsSample = now;

    root["cpu_percent"] = cpuPercent;
    root["ram_percent"] = memoryTotal == 0
      ? 0.0
      : 100.0 * static_cast<double>(memoryTotal - std::min(memoryTotal, memoryAvailable)) / memoryTotal;
    Json::Value interfaces(Json::arrayValue);
    const auto interfaceAddresses = enumerateNetworkInterfaces();
    std::ifstream netdev("/proc/net/dev");
    std::string line;
    while (std::getline(netdev, line)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon);
      name.erase(0, name.find_first_not_of(" \t"));
      name.erase(name.find_last_not_of(" \t") + 1);
      if (name == "lo" || name.empty()) continue;
      std::istringstream values(line.substr(colon + 1));
      uint64_t rxBytes = 0, txBytes = 0;
      if (!(values >> rxBytes)) continue;
      for (int i = 0; i < 7; ++i) {
        uint64_t ignored = 0;
        values >> ignored;
      }
      if (!(values >> txBytes)) continue;
      double rxMbps = 0.0;
      double txMbps = 0.0;
      const auto previous = previousNetworkBytes.find(name);
      if (elapsedSeconds > 0.0 && previous != previousNetworkBytes.end()) {
        rxMbps = static_cast<double>(rxBytes - std::min(rxBytes, previous->second.first)) * 8.0 / elapsedSeconds / 1000000.0;
        txMbps = static_cast<double>(txBytes - std::min(txBytes, previous->second.second)) * 8.0 / elapsedSeconds / 1000000.0;
      }
      previousNetworkBytes[name] = {rxBytes, txBytes};
      Json::Value item;
      item["name"] = name;
      const auto address = std::find_if(interfaceAddresses.begin(), interfaceAddresses.end(), [&name](const NetworkInterface& iface) {
        return iface.name == name;
      });
      item["address"] = address == interfaceAddresses.end() ? "" : address->address;
      item["rx_mbps"] = rxMbps;
      item["tx_mbps"] = txMbps;
      interfaces.append(item);
    }
    root["interfaces"] = interfaces;
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
    root["subscriber_filtering_enabled"] = configManager.subscribers.filteringEnabled;
    Json::Value subscribers(Json::arrayValue);
    for (const auto& subscriber : configManager.subscribers.subscribers) {
      Json::Value item = subscriber.toJson();
      const size_t activeSessions = streamManager.activeSubscriberSessions(subscriber);
      item["active_sessions"] = Json::UInt64(activeSessions);
      item["session_active"] = activeSessions > 0;
      subscribers.append(item);
    }
    root["subscribers"] = subscribers;
    Json::Value streams(Json::arrayValue);
    auto snap = streamManager.snapshot();
    for (const auto& cfg : configManager.config.streams) {
        Json::Value item = cfg.toJson();
        if (snap.count(cfg.id)) {
            auto* streamState = snap.at(cfg.id);
            item["active"] = streamState->active.load();
            item["status"] = streamState->statusMessage;
            item["using_backup"] = streamState->usingBackup;
            item["active_input_uri"] = cfg.testPattern
                ? "test://bars"
                : (streamState->activeInputUri.empty() ? cfg.inputUri : streamState->activeInputUri);
            item["active_input_label"] = cfg.testPattern
                ? "Тест"
                : (streamState->usingBackup ? "Резерв" : "Основной");
            item["bitrate_in_kbps"] = Json::UInt64(streamState->inputBitrate.load() / 1000);
            item["bitrate_out_kbps"] = Json::UInt64(streamState->outputBitrate.load() / 1000);
            item["cc_errors"] = Json::UInt64(streamState->ccErrorsDelta.load());
            item["cc_errors_total"] = Json::UInt64(streamState->ccErrors.load());
        } else {
            item["active"] = false;
            item["status"] = "stopped";
            item["using_backup"] = false;
            item["active_input_uri"] = cfg.testPattern ? "test://bars" : cfg.inputUri;
            item["active_input_label"] = cfg.testPattern ? "Тест" : "Основной";
            item["bitrate_in_kbps"] = Json::UInt64(0);
            item["bitrate_out_kbps"] = Json::UInt64(0);
            item["cc_errors"] = Json::UInt64(0);
            item["cc_errors_total"] = Json::UInt64(0);
        }
        Json::Value links(Json::arrayValue);
        for (const auto& output : streamOutputs(cfg)) {
            Json::Value link;
            link["output_type"] = normalizedOutputType(output);
            link["output_mode"] = output.outputMode;
            link["output_host"] = output.outputHost;
            link["output_port"] = output.outputPort;
            link["url"] = streamLink(output, configManager.config.httpPort);
            links.append(link);
        }
        item["vlc_links"] = links;
        item["vlc_link"] = links.size() == 0 ? "" : links[0].get("url", "").asString();
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
    boost::system::error_code endpointError;
    const std::string clientIp = socket.remote_endpoint(endpointError).address().to_string();
    int fd = socket.release();
    streamManager.addHttpClient(id, fd, endpointError ? "" : clientIp);
    return true;
}

bool HttpServer::serveHlsFile(const tcp::socket& socket, const std::string& target, http::response<http::string_body>& res) {
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
    boost::system::error_code endpointError;
    const std::string clientIp = socket.remote_endpoint(endpointError).address().to_string();
    if (!endpointError && !clientIp.empty()) {
        streamManager.addStreamSession(id, clientIp, "hls");
    }
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
    refreshHttpPorts();
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

  void HttpServer::handleDeleteStream(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
      std::cerr << "Invalid delete-stream payload: " << errs << std::endl;
      return;
    }
    const std::string id = root.get("id", "").asString();
    if (id.empty()) return;
    streamManager.stopStream(id);
    auto& streams = configManager.config.streams;
    streams.erase(std::remove_if(streams.begin(), streams.end(), [&id](const StreamConfig& stream) {
      return stream.id == id;
    }), streams.end());
    configManager.save();
  }

  void HttpServer::handleSaveSubscribers(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
      std::cerr << "Invalid subscribers payload: " << errs << std::endl;
      return;
    }
    SubscriberListConfig next;
    next.filteringEnabled = root.get("filtering_enabled", false).asBool();
    if (root["subscribers"].isArray()) {
      for (const auto& item : root["subscribers"]) {
        auto subscriber = SubscriberConfig::fromJson(item);
        if (!subscriber.name.empty() && (!subscriber.primaryIp.empty() || !subscriber.backupIp.empty())) {
          next.subscribers.push_back(std::move(subscriber));
        }
      }
    }
    if (configManager.subscribers.toJson() == next.toJson()) {
      std::cerr << "Subscriber update unchanged; skipping session reset" << std::endl;
      return;
    }

    configManager.subscribers = std::move(next);
    configManager.saveSubscribers();
    const size_t reset = streamManager.enforceSubscriberAccess();
    if (reset > 0) {
      std::cerr << "Reset unauthorized stream sessions after subscriber update: " << reset << std::endl;
    }
  }

  void HttpServer::handleResetSubscriber(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) return;
    const std::string name = root.get("name", "").asString();
    for (const auto& subscriber : configManager.subscribers.subscribers) {
      if (subscriber.name != name) continue;
      size_t reset = 0;
      if (!subscriber.primaryIp.empty()) reset += streamManager.resetHttpSessions(subscriber.primaryIp);
      if (!subscriber.backupIp.empty() && subscriber.backupIp != subscriber.primaryIp) {
        reset += streamManager.resetHttpSessions(subscriber.backupIp);
      }
      const size_t restarted = streamManager.restartSrtOutputsForStreams(subscriber.streamIds);
      std::cerr << "Reset subscriber sessions: " << name << " (" << reset
                << "), restarted_srt_outputs=" << restarted << std::endl;
      return;
    }
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
.system-load{display:flex;align-items:center;gap:8px;padding:6px 10px;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.08);border-radius:12px;color:#d1d9ed;font-size:.76rem;white-space:nowrap}
.system-load strong{color:#fff;font-size:.76rem}
.system-load .metric{display:flex;align-items:center;gap:4px}
.system-load .metric span{color:#7dd1ff;font-weight:700;min-width:38px;text-align:right}
.network-button{padding:7px 10px;border:1px solid rgba(57,189,248,.28);border-radius:10px;color:#bdefff;background:rgba(57,189,248,.12);cursor:pointer;font-size:.78rem;white-space:nowrap}
.network-button:hover{background:rgba(57,189,248,.24)}
.button-primary{padding:8px 14px;border:none;border-radius:999px;color:#FFF;background:#1f8bff;cursor:pointer;font-size:0.88rem;transition:background .2s ease,color .2s ease,box-shadow .2s ease,opacity .2s ease}
.button-secondary{padding:7px 12px;border:1px solid rgba(255,255,255,.14);border-radius:999px;color:#EEE;background:rgba(255,255,255,.05);cursor:pointer;font-size:0.82rem;transition:background .2s ease,border-color .2s ease}
.button-primary:hover{background:#0f7ce7}
.button-primary.save-clean{background:rgba(255,255,255,.08);color:#9aa3b1;box-shadow:inset 0 0 0 1px rgba(255,255,255,.12)}
.button-primary.save-clean:hover{background:rgba(255,255,255,.08)}
.button-primary.save-dirty{background:#ffbd4a;color:#161b25;box-shadow:0 0 0 2px rgba(255,189,74,.18)}
.button-primary.save-dirty:hover{background:#ffc968}
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
.tile .delete-button{position:absolute;top:8px;right:8px;width:16px;height:16px;padding:0;border:0;border-radius:50%;background:#d9363e;color:#fff;font-size:12px;line-height:16px;cursor:pointer;box-shadow:0 3px 8px rgba(0,0,0,.24)}
.tile .delete-button:hover{background:#f0444d;transform:scale(1.08)}
.tile .title{font-size:11px;font-weight:700;line-height:1.2;color:#fff}
.tile .badge{position:absolute;left:50%;top:10px;transform:translateX(-50%);padding:2px 5px;background:rgba(20,161,255,.14);color:#7dd1ff;border-radius:999px;font-size:11px;text-transform:uppercase;letter-spacing:.08em}
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
.modal-content{position:relative;background:rgba(11,15,22,.985);padding:18px 18px;border-radius:22px;width:min(520px,100%);max-height:92%;overflow:auto;box-shadow:0 28px 70px rgba(0,0,0,.24);border:1px solid rgba(255,255,255,.08)}
.modal-close{position:absolute;top:10px;right:10px;width:28px;height:28px;padding:0;border:0;border-radius:8px;background:rgba(255,95,95,.18);color:#ffc2c2;font-size:18px;line-height:28px;cursor:pointer;z-index:2}
.modal-close:hover{background:rgba(255,95,95,.3);color:#fff}
.modal-content.quality-modal{width:min(920px,100%)}
.modal-content.network-modal{width:min(620px,100%)}
.modal-content.subscriber-modal{width:min(1280px,100%);max-height:98%}
.modal-content h2{margin-top:0;font-size:1.25rem;margin-bottom:14px;color:#fff}
.quality-head{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;flex-wrap:wrap;margin-bottom:10px}
.quality-title{display:flex;flex-direction:column;gap:4px}
.quality-title small{color:#9aa3b1}
.period-tabs{display:flex;gap:6px;flex-wrap:wrap}
.period-tabs button{padding:6px 8px;border:1px solid rgba(255,255,255,.1);background:rgba(255,255,255,.05);color:#d7deec;border-radius:8px;cursor:pointer;font-size:.72rem}
.period-tabs button.active{background:#1f8bff;color:#fff;border-color:#1f8bff}
.quality-board{position:relative;border:1px solid rgba(255,255,255,.08);background:#101722;border-radius:10px;padding:8px}
.quality-board canvas{display:block;width:100%;height:230px;cursor:copy}
.quality-board.cc-board{margin-top:10px}
.quality-board.cc-board canvas{height:150px;cursor:default}
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
.output-list{display:grid;gap:8px;width:100%}
.output-row{display:grid;grid-template-columns:minmax(120px,1.1fr) minmax(106px,.8fr) minmax(130px,1fr) 86px 30px;gap:6px;align-items:end;padding:8px;background:rgba(255,255,255,.035);border:1px solid rgba(255,255,255,.07);border-radius:8px}
.output-row .form-row{gap:5px}
.output-row input,.output-row select{box-sizing:border-box;max-width:none}
.output-row .remove-output{width:30px;height:30px;padding:0;border:0;border-radius:8px;background:rgba(255,95,95,.18);color:#ffc2c2;cursor:pointer}
.output-row .remove-output:disabled{opacity:.35;cursor:not-allowed}
@media (max-width:760px){.output-row{grid-template-columns:1fr 1fr}.output-row .remove-output{align-self:end}}
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
.about-donate{align-items:start}
.about-donate-content{display:grid;grid-template-columns:148px minmax(0,1fr);gap:12px;align-items:center}
.about-qr{width:148px;height:148px;display:block;background:#fff;border-radius:8px;padding:8px;box-sizing:border-box}
.about-donate-address{font-family:monospace;font-size:.82rem;line-height:1.45}
@media (max-width:560px){.about-donate-content{grid-template-columns:1fr}.about-qr{width:132px;height:132px}}
.network-table{width:100%;border-collapse:collapse;color:#d7deec;font-size:.85rem}
.network-table th,.network-table td{padding:9px 6px;text-align:right;border-bottom:1px solid rgba(255,255,255,.08)}
.network-table th:first-child,.network-table td:first-child{text-align:left}
.network-table th{color:#9aa3b1;font-weight:600}
.network-empty{padding:22px 0;text-align:center;color:#9aa3b1}
.subscriber-list{display:grid;gap:8px;min-width:1020px}
.subscriber-row{display:grid;grid-template-columns:minmax(190px,1.8fr) minmax(180px,1fr) minmax(180px,1fr) minmax(120px,auto) 86px minmax(145px,auto) 34px;gap:6px;align-items:center}
.subscriber-row input{width:100%;box-sizing:border-box;padding:7px 8px;background:#121825;border:1px solid rgba(255,255,255,.08);border-radius:8px;color:#EEE;font-size:.78rem}
.subscriber-row .remove-subscriber{width:30px;height:30px;padding:0;border:0;border-radius:8px;background:rgba(255,95,95,.18);color:#ffc2c2;cursor:pointer}
.subscriber-head{display:grid;grid-template-columns:minmax(190px,1.8fr) minmax(180px,1fr) minmax(180px,1fr) minmax(120px,auto) 86px minmax(145px,auto) 34px;gap:6px;color:#9aa3b1;font-size:.72rem;margin-bottom:4px;min-width:1020px}
.subscriber-streams{grid-column:1/-1;display:flex;gap:5px;flex-wrap:wrap;padding:4px 0 8px 4px;border-bottom:1px solid rgba(255,255,255,.08)}
.subscriber-streams label{display:flex;align-items:center;gap:4px;color:#cfd8ea;font-size:.72rem}
.subscriber-stream-picker{position:relative;min-width:0}
.subscriber-stream-picker summary{display:inline-flex;align-items:center;gap:6px;padding:6px 9px;border:1px solid rgba(255,255,255,.1);border-radius:8px;background:rgba(255,255,255,.05);color:#d7deec;font-size:.76rem;cursor:pointer;list-style:none}
.subscriber-stream-picker summary::-webkit-details-marker{display:none}
.subscriber-stream-picker summary:after{content:'▾';color:#9aa3b1}
.subscriber-stream-options{position:absolute;right:0;top:calc(100% + 4px);z-index:3;display:grid;gap:5px;min-width:180px;margin-top:0;padding:8px;background:#121825;border:1px solid rgba(255,255,255,.1);border-radius:8px;box-shadow:0 14px 30px rgba(0,0,0,.28)}
.subscriber-stream-options label{display:flex;align-items:center;gap:6px;color:#cfd8ea;font-size:.76rem}
.subscriber-enabled{display:flex;align-items:center;justify-content:center;gap:4px;color:#b6f7c2;font-size:.72rem;white-space:nowrap}
.subscriber-enabled input{width:15px;height:15px}
.subscriber-session{display:flex;align-items:center;gap:6px;white-space:nowrap;color:#9aa3b1;font-size:.72rem}
.subscriber-session.active{color:#b6f7c2}
.reset-session{padding:5px 7px;border:1px solid rgba(255,184,77,.25);border-radius:7px;background:rgba(255,184,77,.12);color:#ffe0a3;cursor:pointer;font-size:.7rem}
</style>
</head>
<body>
<header>
<div class="header-left">
<div>
<div class="title">Control Panel</div>
<div class="subtitle" data-i18n="subtitle">Broadcast monitoring and stream control</div>
</div>
</div>
<div class="header-center">
<div class="system-load">
<span class="metric"><strong>CPU</strong> <span id="cpuLoad">—%</span></span>
<span class="metric"><strong>RAM</strong> <span id="ramLoad">—%</span></span>
</div>
<div class="stats-panel">
<div class="status"><strong data-i18n="total">Total:</strong> <span id="totalCount">0</span></div>
<div class="status"><strong data-i18n="active">Active:</strong> <span id="activeCount">0</span></div>
</div>
<button class="network-button" onclick="openNetworkModal()" data-i18n="network">Network</button>
</div>
<div class="header-right">
<button class="button-secondary" onclick="toggleLanguage()" id="languageButton">RU</button>
<button class="button-secondary" onclick="openLoginModal()" data-i18n="user">User</button>
<button class="button-secondary" onclick="openTelegramModal()">Telegram API</button>
<button class="button-secondary" onclick="downloadVlcPlaylist()" data-i18n="playlist">VLC playlist</button>
<button class="button-secondary" onclick="openSubscribersModal()" data-i18n="subscribers">Subscribers</button>
<button class="button-primary" onclick="openStreamModal()" data-i18n="addStream">+ Add stream</button>
<button class="button-secondary" onclick="openAboutModal()">About</button>
</div>
</header>
<div class="container">
<div id="tiles" class="tile-grid"></div>
<div id="modal" class="modal">
<div class="modal-content" id="modalContent"></div>
</div>
<script>
const translations = {
  en: {
    subtitle:'Broadcast monitoring and stream control', total:'Total:', active:'Active:', network:'Network', user:'User', addStream:'+ Add stream',
    interfacesNotFound:'No interfaces found', output:'Output', activeInput:'Active input', primary:'Primary', backup:'Backup', sid:'SID', bitrateIn:'Bitrate In', bitrateOut:'Bitrate Out', status:'Status',
    online:'Online', backupOnline:'Backup', offline:'Offline', start:'Start', stop:'Stop', edit:'Edit', chart:'Chart', delete:'Delete stream', removeConfirm:'Delete stream',
    networkLoad:'Network interface load', interface:'Interface', incoming:'Incoming', outgoing:'Outgoing', close:'Close',
    about:'About', name:'Name', country:'Country', donate:'Donate', donateQr:'Donate QR code', cancel:'Cancel', save:'Save', userTitle:'User', telegram:'Telegram API', quality:'Stream quality', playlist:'VLC playlist', subscribers:'Subscribers', streams:'Streams', filtering:'Enable IP filtering', addSubscriber:'Add subscriber', primaryIp:'Primary IP', backupIp:'Backup IP', addedAt:'Added at', subscriberName:'Subscriber name', noSubscribers:'No subscribers added', noStreams:'No streams configured', enabled:'Enabled', disabled:'Disabled', exportSubscribers:'Export TXT', session:'Session', activeSession:'Online', offlineSession:'Offline', resetSession:'Reset'
  },
  ru: {
    subtitle:'Мониторинг трансляций и управление потоками', total:'Всего:', active:'Активно:', network:'Сеть', user:'Пользователь', addStream:'+ Добавить поток',
    interfacesNotFound:'Интерфейсы не найдены', output:'Вывод', activeInput:'Активный вход', primary:'Основной', backup:'Резерв', sid:'SID', bitrateIn:'Bitrate In', bitrateOut:'Bitrate Out', status:'Статус',
    online:'Онлайн', backupOnline:'Резерв', offline:'Офлайн', start:'Старт', stop:'Стоп', edit:'Ред.', chart:'График', delete:'Удалить поток', removeConfirm:'Удалить поток',
    networkLoad:'Загрузка сетевых интерфейсов', interface:'Интерфейс', incoming:'Входящий', outgoing:'Исходящий', close:'Закрыть',
    about:'About', name:'Имя', country:'Страна', donate:'Донат', donateQr:'QR-код доната', cancel:'Отмена', save:'Сохранить', userTitle:'Пользователь', telegram:'Telegram API', quality:'Качество потока', playlist:'Плейлист VLC', subscribers:'Абоненты', streams:'Потоки', filtering:'Включить фильтрацию по IP', addSubscriber:'Добавить абонента', primaryIp:'Основной IP', backupIp:'Резервный IP', addedAt:'Дата добавления', subscriberName:'Наименование абонента', noSubscribers:'Абоненты не добавлены', noStreams:'Потоки не настроены', enabled:'Включен', disabled:'Отключен', exportSubscribers:'Экспорт TXT', session:'Сессия', activeSession:'Онлайн', offlineSession:'Офлайн', resetSession:'Сбросить'
  }
};
let language = localStorage.getItem('tvstreamer-language') || 'en';
const donateAddress = 'UQD1uQn5WxhzKLXjL0KOVuJDcRU65pYzgt6pm_gzJM-vT-cN';
const donateQrPath = 'M4 4h7v1H4zM12 4h1v1H12zM14 4h3v1H14zM25 4h3v1H25zM30 4h7v1H30zM4 5h1v1H4zM10 5h1v1H10zM13 5h1v1H13zM15 5h1v1H15zM17 5h2v1H17zM20 5h3v1H20zM26 5h1v1H26zM28 5h1v1H28zM30 5h1v1H30zM36 5h1v1H36zM4 6h1v1H4zM6 6h3v1H6zM10 6h1v1H10zM12 6h3v1H12zM16 6h1v1H16zM18 6h2v1H18zM22 6h1v1H22zM25 6h2v1H25zM28 6h1v1H28zM30 6h1v1H30zM32 6h3v1H32zM36 6h1v1H36zM4 7h1v1H4zM6 7h3v1H6zM10 7h1v1H10zM14 7h1v1H14zM16 7h1v1H16zM18 7h1v1H18zM20 7h1v1H20zM22 7h1v1H22zM24 7h2v1H24zM27 7h2v1H27zM30 7h1v1H30zM32 7h3v1H32zM36 7h1v1H36zM4 8h1v1H4zM6 8h3v1H6zM10 8h1v1H10zM14 8h11v1H14zM26 8h2v1H26zM30 8h1v1H30zM32 8h3v1H32zM36 8h1v1H36zM4 9h1v1H4zM10 9h1v1H10zM12 9h1v1H12zM15 9h1v1H15zM20 9h1v1H20zM22 9h1v1H22zM24 9h1v1H24zM26 9h1v1H26zM30 9h1v1H30zM36 9h1v1H36zM4 10h7v1H4zM12 10h1v1H12zM14 10h1v1H14zM16 10h1v1H16zM18 10h1v1H18zM20 10h1v1H20zM22 10h1v1H22zM24 10h1v1H24zM26 10h1v1H26zM28 10h1v1H28zM30 10h7v1H30zM13 11h1v1H13zM16 11h1v1H16zM22 11h1v1H22zM27 11h2v1H27zM4 12h1v1H4zM6 12h1v1H6zM10 12h2v1H10zM14 12h2v1H14zM17 12h1v1H17zM19 12h1v1H19zM21 12h1v1H21zM23 12h1v1H23zM25 12h1v1H25zM27 12h2v1H27zM31 12h1v1H31zM34 12h1v1H34zM36 12h1v1H36zM4 13h2v1H4zM9 13h1v1H9zM11 13h3v1H11zM16 13h1v1H16zM18 13h2v1H18zM24 13h3v1H24zM28 13h3v1H28zM35 13h2v1H35zM5 14h2v1H5zM10 14h1v1H10zM15 14h1v1H15zM17 14h7v1H17zM25 14h1v1H25zM27 14h2v1H27zM30 14h2v1H30zM34 14h1v1H34zM36 14h1v1H36zM6 15h3v1H6zM11 15h1v1H11zM16 15h1v1H16zM18 15h3v1H18zM22 15h3v1H22zM26 15h2v1H26zM29 15h5v1H29zM35 15h2v1H35zM6 16h1v1H6zM8 16h1v1H8zM10 16h1v1H10zM17 16h6v1H17zM24 16h1v1H24zM30 16h2v1H30zM33 16h2v1H33zM36 16h1v1H36zM4 17h3v1H4zM8 17h2v1H8zM13 17h1v1H13zM15 17h1v1H15zM18 17h1v1H18zM20 17h3v1H20zM24 17h2v1H24zM27 17h5v1H27zM33 17h1v1H33zM35 17h1v1H35zM4 18h2v1H4zM8 18h3v1H8zM12 18h2v1H12zM15 18h2v1H15zM18 18h1v1H18zM20 18h1v1H20zM22 18h1v1H22zM24 18h4v1H24zM30 18h1v1H30zM33 18h2v1H33zM36 18h1v1H36zM5 19h1v1H5zM8 19h2v1H8zM11 19h1v1H11zM13 19h1v1H13zM18 19h3v1H18zM23 19h2v1H23zM28 19h2v1H28zM31 19h2v1H31zM35 19h1v1H35zM6 20h1v1H6zM9 20h2v1H9zM13 20h1v1H13zM15 20h3v1H15zM21 20h1v1H21zM24 20h1v1H24zM30 20h2v1H30zM36 20h1v1H36zM4 21h1v1H4zM7 21h3v1H7zM14 21h1v1H14zM16 21h2v1H16zM19 21h1v1H19zM21 21h1v1H21zM24 21h1v1H24zM27 21h3v1H27zM31 21h1v1H31zM33 21h1v1H33zM36 21h1v1H36zM8 22h1v1H8zM10 22h1v1H10zM13 22h1v1H13zM15 22h1v1H15zM17 22h1v1H17zM19 22h1v1H19zM23 22h1v1H23zM26 22h4v1H26zM32 22h3v1H32zM36 22h1v1H36zM6 23h1v1H6zM8 23h2v1H8zM14 23h2v1H14zM18 23h1v1H18zM20 23h2v1H20zM23 23h1v1H23zM26 23h1v1H26zM30 23h1v1H30zM36 23h1v1H36zM5 24h2v1H5zM8 24h1v1H8zM10 24h2v1H10zM13 24h2v1H13zM16 24h2v1H16zM19 24h1v1H19zM21 24h1v1H21zM25 24h1v1H25zM28 24h1v1H28zM30 24h1v1H30zM32 24h1v1H32zM35 24h1v1H35zM5 25h2v1H5zM8 25h1v1H8zM17 25h2v1H17zM25 25h1v1H25zM27 25h1v1H27zM31 25h1v1H31zM33 25h1v1H33zM35 25h1v1H35zM4 26h2v1H4zM9 26h2v1H9zM12 26h1v1H12zM14 26h1v1H14zM16 26h1v1H16zM18 26h2v1H18zM21 26h1v1H21zM23 26h1v1H23zM26 26h2v1H26zM30 26h1v1H30zM32 26h2v1H32zM36 26h1v1H36zM7 27h1v1H7zM11 27h1v1H11zM13 27h1v1H13zM15 27h2v1H15zM18 27h1v1H18zM28 27h6v1H28zM4 28h4v1H4zM10 28h2v1H10zM13 28h1v1H13zM17 28h3v1H17zM23 28h3v1H23zM28 28h5v1H28zM35 28h2v1H35zM12 29h3v1H12zM18 29h1v1H18zM20 29h1v1H20zM24 29h2v1H24zM28 29h1v1H28zM32 29h1v1H32zM35 29h2v1H35zM4 30h7v1H4zM12 30h1v1H12zM14 30h1v1H14zM16 30h2v1H16zM19 30h6v1H19zM28 30h1v1H28zM30 30h1v1H30zM32 30h1v1H32zM34 30h1v1H34zM36 30h1v1H36zM4 31h1v1H4zM10 31h1v1H10zM16 31h4v1H16zM22 31h1v1H22zM27 31h2v1H27zM32 31h2v1H32zM35 31h1v1H35zM4 32h1v1H4zM6 32h3v1H6zM10 32h1v1H10zM13 32h2v1H13zM16 32h1v1H16zM19 32h14v1H19zM35 32h1v1H35zM4 33h1v1H4zM6 33h3v1H6zM10 33h1v1H10zM15 33h1v1H15zM18 33h1v1H18zM20 33h1v1H20zM22 33h1v1H22zM24 33h2v1H24zM29 33h1v1H29zM31 33h1v1H31zM35 33h2v1H35zM4 34h1v1H4zM6 34h3v1H6zM10 34h1v1H10zM12 34h1v1H12zM15 34h3v1H15zM20 34h1v1H20zM22 34h1v1H22zM24 34h2v1H24zM27 34h1v1H27zM32 34h5v1H32zM4 35h1v1H4zM10 35h1v1H10zM13 35h1v1H13zM16 35h1v1H16zM19 35h2v1H19zM22 35h3v1H22zM30 35h2v1H30zM33 35h1v1H33zM4 36h7v1H4zM12 36h2v1H12zM15 36h1v1H15zM17 36h2v1H17zM21 36h1v1H21zM26 36h1v1H26zM28 36h2v1H28zM32 36h2v1H32zM36 36h1v1H36z';
function t(key, values={}) {
  let value = translations[language]?.[key] || translations.en[key] || key;
  Object.entries(values).forEach(([name, replacement]) => { value = value.replace(`{${name}}`, replacement); });
  return value;
}
function applyLanguage() {
  document.querySelectorAll('[data-i18n]').forEach(element => { element.textContent = t(element.dataset.i18n); });
  const button = document.getElementById('languageButton');
  if (button) button.textContent = language === 'en' ? 'RU' : 'EN';
}
function toggleLanguage() {
  language = language === 'en' ? 'ru' : 'en';
  localStorage.setItem('tvstreamer-language', language);
  applyLanguage();
  render();
}
let state = {};
let networkRefreshTimer = null;
let subscribersModalOpen = false;
let subscriberFormBaseline = '';
function fetchState() {
  Promise.all([fetch('/api/state', {cache:'no-store'}).then(r=>r.json()), fetch('/api/system-metrics', {cache:'no-store'}).then(r=>r.json())])
    .then(([data, metrics])=>{state=data; state.system_metrics=metrics; render(); updateSystemLoad(metrics); refreshSubscriberSessions();});
}
function updateSystemLoad(metrics) {
  document.getElementById('cpuLoad').textContent = `${Number(metrics.cpu_percent || 0).toFixed(1)}%`;
  document.getElementById('ramLoad').textContent = `${Number(metrics.ram_percent || 0).toFixed(1)}%`;
  const table = document.getElementById('networkTableBody');
  if (!table) return;
  const interfaces = metrics.interfaces || [];
  table.innerHTML = interfaces.length ? interfaces.map(iface => `
    <tr><td>${iface.name}${iface.address ? ` (${iface.address})` : ''}</td><td>${Number(iface.rx_mbps || 0).toFixed(2)} Mbps</td><td>${Number(iface.tx_mbps || 0).toFixed(2)} Mbps</td></tr>
  `).join('') : `<tr><td colspan="3" class="network-empty">${t('interfacesNotFound')}</td></tr>`;
}
function fetchSystemMetrics() {
  fetch('/api/system-metrics', {cache:'no-store'}).then(r=>r.json()).then(updateSystemLoad).catch(()=>{});
}
function downloadVlcPlaylist() {
  const entries = (state.streams || [])
    .flatMap(stream => {
      const links = streamLinks(stream);
      return links.map(link => {
        const suffix = links.length > 1 && link.output_type ? ` ${String(link.output_type).toUpperCase()}` : '';
        const name = String((stream.name || stream.id) + suffix).replace(/[\r\n]/g, ' ').trim();
        return `#EXTINF:-1,${name}\n${link.url}`;
      });
    });
  const content = `#EXTM3U\n${entries.join('\n')}\n`;
  const blob = new Blob([content], {type:'audio/x-mpegurl;charset=utf-8'});
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = 'tvstreamer5-playlist.m3u';
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}
function modalCloseButton() {
  return `<button class="modal-close" onclick="closeModal()" aria-label="${t('close')}">×</button>`;
}
function openModal(html) {
  subscribersModalOpen = false;
  document.getElementById('modalContent').innerHTML = modalCloseButton() + html;
  document.getElementById('modalContent').className = 'modal-content';
  document.getElementById('modal').classList.add('active');
}
function closeModal() {
  subscribersModalOpen = false;
  document.getElementById('modal').classList.remove('active');
}
function normalizedOutputType(stream) {
  const raw = String(stream.output_type || 'udp').toLowerCase();
  if (raw === 'udp') return stream.cbr ? 'udp-cbr' : 'udp-vbr';
  if (raw === 'udp_cbr' || raw === 'udpcbr') return 'udp-cbr';
  if (raw === 'udp_vbr' || raw === 'udpvbr') return 'udp-vbr';
  return raw;
}
function outputConfigsForStream(stream) {
  const makeOutput = output => ({
    output_type: output.output_type || 'udp-cbr',
    output_mode: output.output_mode || 'listener',
    output_host: output.output_host || '127.0.0.1',
    output_port: Number(output.output_port || 1234),
    cbr: stream.cbr
  });
  if (Array.isArray(stream.outputs) && stream.outputs.length) {
    return stream.outputs.map(makeOutput);
  }
  return [
    makeOutput(stream),
    ...(Array.isArray(stream.additional_outputs) ? stream.additional_outputs.map(makeOutput) : [])
  ];
}
function streamLinks(stream) {
  if (Array.isArray(stream.vlc_links) && stream.vlc_links.length) {
    return stream.vlc_links.filter(link => link.url);
  }
  return stream.vlc_link ? [{output_type: normalizedOutputType(stream), url: stream.vlc_link}] : [];
}
function outputBadgeText(stream) {
  const outputs = outputConfigsForStream(stream);
  return outputs.length > 1 ? `${outputs.length} OUT` : normalizedOutputType(outputs[0] || stream).toUpperCase();
}
function streamBitrateMode(stream) {
  const type = normalizedOutputType(stream);
  if (type === 'udp-cbr') return 'CBR';
  if (type === 'udp-vbr') return 'VBR';
  return stream.cbr ? 'CBR' : 'VBR';
}
function render() {
  document.getElementById('totalCount').textContent = state.stream_count;
  document.getElementById('activeCount').textContent = state.active_count;
  const tiles = document.getElementById('tiles');
  tiles.innerHTML = '';
  state.streams.forEach(stream => {
    const outputs = outputConfigsForStream(stream);
    const outputType = normalizedOutputType(outputs[0] || stream);
    const bitrateMode = streamBitrateMode(stream);
    const links = streamLinks(stream);
    const primaryLink = links[0]?.url || stream.vlc_link || `${stream.output_host || ''}:${stream.output_port || ''}`;
    const tile = document.createElement('div');
    tile.className = 'tile' + (stream.active ? ' active' : '');
    tile.innerHTML = `
      <div class="top">
        <div>
          <div class="title">${stream.name || stream.id}</div>
          <div class="status-pill ${stream.active ? 'active' : 'stopped'}">${stream.active ? (stream.using_backup ? 'Backup' : 'Online') : 'Offline'}</div>
        </div>
        <div class="badge">${outputs.length > 1 ? outputBadgeText(stream) : bitrateMode}</div>
      </div>
      <button class="delete-button" title="Удалить поток" aria-label="Удалить поток" onclick="deleteStream('${stream.id}')">×</button>
      <div class="info">
        <div class="info-row"><strong>${t('output')}</strong><span>${outputs.length > 1 ? outputBadgeText(stream) : outputType.toUpperCase()} · ${primaryLink}</span></div>
        <div class="info-row"><strong>${t('activeInput')}</strong><span>${stream.active_input_label || t('primary')} · ${stream.active_input_uri || stream.input_uri || '—'}</span></div>
        <div class="info-row"><strong>${t('primary')}</strong><span>${stream.input_uri || '—'}</span></div>
        <div class="info-row"><strong>${t('backup')}</strong><span>${stream.backup_input_uri || '—'}</span></div>
        <div class="info-row"><strong>${t('sid')}</strong><span>${stream.service_id || '—'}</span></div>
        <div class="info-row"><strong>${t('bitrateIn')}</strong><span>${stream.bitrate_in_kbps ? stream.bitrate_in_kbps + ' kbps' : '—'}</span></div>
        <div class="info-row"><strong>${t('bitrateOut')}</strong><span>${stream.bitrate_out_kbps ? stream.bitrate_out_kbps + ' kbps' : '—'}</span></div>
        <div class="info-row"><strong>${t('status')}</strong><span>${stream.status}</span></div>
      </div>
      <div class="controls">
        <button class="${stream.active ? 'stop-button' : 'start-button'}" onclick="toggleStream('${stream.id}', ${stream.active})">${stream.active ? t('stop') : t('start')}</button>
        <button onclick="editStream('${stream.id}')">${t('edit')}</button>
        <button class="quality-button" onclick="openQualityModal('${stream.id}')">${t('chart')}</button>
        <button class="copy-button" onclick="copyStreamLinks('${stream.id}', this)">${links.length > 1 ? 'URLs' : 'URL'}</button>
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
function deleteStream(id) {
  const stream = state.streams.find(s=>s.id===id);
  if (!stream || !window.confirm(`${t('removeConfirm')} «${stream.name || stream.id}»?`)) return;
  fetch('/api/delete-stream', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})})
    .then(()=>{ closeModal(); setTimeout(fetchState, 300); });
}
function openNetworkModal() {
  document.getElementById('modalContent').className = 'modal-content network-modal';
  document.getElementById('modalContent').innerHTML = modalCloseButton() + `
    <h2>${t('networkLoad')}</h2>
    <table class="network-table"><thead><tr><th>${t('interface')}</th><th>${t('incoming')}</th><th>${t('outgoing')}</th></tr></thead><tbody id="networkTableBody"></tbody></table>
    <div class="modal-actions"><button class="button-secondary" onclick="closeNetworkModal()">${t('close')}</button></div>
  `;
  document.getElementById('modal').classList.add('active');
  fetchSystemMetrics();
  clearInterval(networkRefreshTimer);
  networkRefreshTimer = setInterval(fetchSystemMetrics, 2000);
}
function closeNetworkModal() {
  clearInterval(networkRefreshTimer);
  networkRefreshTimer = null;
  closeModal();
}
function openSubscribersModal() {
  const renderSubscribers = () => {
    const subscribers = state.subscribers || [];
    const rows = subscribers.length ? subscribers.map((subscriber, index) => `
      <div class="subscriber-row" data-index="${index}" data-added-at="${subscriber.added_at || ''}">
        <input data-field="name" value="${subscriber.name || ''}" placeholder="${t('subscriberName')}" />
        <input data-field="primary_ip" value="${subscriber.primary_ip || ''}" placeholder="${t('primaryIp')}" />
        <input data-field="backup_ip" value="${subscriber.backup_ip || ''}" placeholder="${t('backupIp')}" />
        <details class="subscriber-stream-picker">
          <summary id="subscriberStreamsSummary-${index}">${t('streams')} (${(subscriber.stream_ids || []).length})</summary>
          <div class="subscriber-stream-options">
            ${(state.streams || []).map(stream => `<label><input type="checkbox" data-stream-id="${stream.id}" onchange="updateSubscriberStreamSummary(${index})" ${(subscriber.stream_ids || []).includes(stream.id) ? 'checked' : ''} />${stream.name || stream.id}</label>`).join('') || `<span>${t('noStreams')}</span>`}
          </div>
        </details>
        <label class="subscriber-enabled"><input data-field="enabled" type="checkbox" onchange="updateSubscriberStatus(this)" ${subscriber.enabled !== false ? 'checked' : ''} /><span>${subscriber.enabled !== false ? t('enabled') : t('disabled')}</span></label>
        <div class="subscriber-session ${subscriber.session_active ? 'active' : ''}"><span>${subscriber.session_active ? `${t('activeSession')} (${subscriber.active_sessions || 0})` : t('offlineSession')}</span><button class="reset-session" onclick="resetSubscriberSession('${String(subscriber.name || '').replace(/'/g, "\\'")}')">${t('resetSession')}</button></div>
        <button class="remove-subscriber" onclick="removeSubscriber(${index})" aria-label="Remove">×</button>
      </div>
    `).join('') : `<div class="network-empty">${t('noSubscribers')}</div>`;
    openModal(`
      <h2>${t('subscribers')}</h2>
      <div class="checkbox-inline"><input id="subscriberFiltering" type="checkbox" ${state.subscriber_filtering_enabled ? 'checked' : ''} /><span>${t('filtering')}</span></div>
      <div class="subscriber-head"><span>${t('subscriberName')}</span><span>${t('primaryIp')}</span><span>${t('backupIp')}</span><span>${t('streams')}</span><span>${t('enabled')}</span><span>${t('session')}</span><span></span></div>
      <div id="subscriberList" class="subscriber-list">${rows}</div>
      <div class="modal-actions">
        <button class="button-secondary" onclick="addSubscriber()">+ ${t('addSubscriber')}</button>
        <button class="button-secondary" onclick="exportSubscribers()">${t('exportSubscribers')}</button>
        <button class="button-secondary" onclick="closeModal()">${t('cancel')}</button>
        <button id="saveSubscribersButton" class="button-primary save-clean" onclick="saveSubscribers()">${t('save')}</button>
      </div>
    `);
    document.getElementById('modalContent').className = 'modal-content subscriber-modal';
    subscribersModalOpen = true;
    if (!subscriberFormBaseline) {
      subscriberFormBaseline = serializeSubscriberPayload(collectSubscriberPayload());
    }
    wireSubscriberDirtyTracking();
    updateSubscribersSaveButton();
    refreshSubscriberSessions();
  };
  subscriberFormBaseline = '';
  renderSubscribers();
}
function serializeSubscriberPayload(payload) {
  return JSON.stringify(payload);
}
function collectSubscriberPayload() {
  const rows = [...document.querySelectorAll('.subscriber-row')];
  const subscribers = rows.map(row => {
    const value = field => row.querySelector(`[data-field="${field}"]`)?.value.trim() || '';
    return {
      name:value('name'), primary_ip:value('primary_ip'), backup_ip:value('backup_ip'), added_at:row.dataset.addedAt || '',
      enabled:row.querySelector('[data-field="enabled"]')?.checked !== false,
      stream_ids:[...row.querySelectorAll('[data-stream-id]:checked')].map(input=>input.dataset.streamId)
    };
  });
  return {filtering_enabled:document.getElementById('subscriberFiltering')?.checked === true, subscribers};
}
function wireSubscriberDirtyTracking() {
  const list = document.getElementById('subscriberList');
  list?.addEventListener('input', updateSubscribersSaveButton);
  list?.addEventListener('change', updateSubscribersSaveButton);
  document.getElementById('subscriberFiltering')?.addEventListener('change', updateSubscribersSaveButton);
}
function updateSubscribersSaveButton() {
  const button = document.getElementById('saveSubscribersButton');
  if (!button) return;
  const dirty = serializeSubscriberPayload(collectSubscriberPayload()) !== subscriberFormBaseline;
  button.classList.toggle('save-dirty', dirty);
  button.classList.toggle('save-clean', !dirty);
}
function refreshSubscriberSessions() {
  if (!subscribersModalOpen) return;
  const list = document.getElementById('subscriberList');
  if (!list) return;
  (state.subscribers || []).forEach((subscriber, index) => {
    const row = list.querySelector(`.subscriber-row[data-index="${index}"]`);
    const session = row?.querySelector('.subscriber-session');
    const text = session?.querySelector('span');
    if (!session || !text) return;
    const active = !!subscriber.session_active;
    session.classList.toggle('active', active);
    text.textContent = active ? `${t('activeSession')} (${subscriber.active_sessions || 0})` : t('offlineSession');
  });
}
function addSubscriber() {
  const payload = collectSubscriberPayload();
  payload.subscribers.push({name:'', primary_ip:'', backup_ip:'', added_at:new Date().toISOString().slice(0, 10), enabled:true, stream_ids:[]});
  state.subscribers = payload.subscribers;
  state.subscriber_filtering_enabled = payload.filtering_enabled;
  const baseline = subscriberFormBaseline;
  openSubscribersModal();
  subscriberFormBaseline = baseline;
  updateSubscribersSaveButton();
}
function removeSubscriber(index) {
  const payload = collectSubscriberPayload();
  payload.subscribers.splice(index, 1);
  state.subscribers = payload.subscribers;
  state.subscriber_filtering_enabled = payload.filtering_enabled;
  const baseline = subscriberFormBaseline;
  openSubscribersModal();
  subscriberFormBaseline = baseline;
  updateSubscribersSaveButton();
}
function updateSubscriberStreamSummary(index) {
  const row = document.querySelector(`.subscriber-row[data-index="${index}"]`);
  const summary = document.getElementById(`subscriberStreamsSummary-${index}`);
  if (!row || !summary) return;
  summary.textContent = `${t('streams')} (${row.querySelectorAll('[data-stream-id]:checked').length})`;
  updateSubscribersSaveButton();
}
function updateSubscriberStatus(input) {
  const label = input.closest('.subscriber-enabled');
  const text = label?.querySelector('span');
  if (text) text.textContent = input.checked ? t('enabled') : t('disabled');
  updateSubscribersSaveButton();
}
function resetSubscriberSession(name) {
  fetch('/api/reset-subscriber', {
    method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({name})
  }).then(()=>{ state.subscribers = (state.subscribers || []).map(subscriber => subscriber.name === name ? {...subscriber, session_active:false, active_sessions:0} : subscriber); refreshSubscriberSessions(); fetchState(); });
}
function saveSubscribers() {
  const payload = collectSubscriberPayload();
  const serialized = serializeSubscriberPayload(payload);
  if (serialized === subscriberFormBaseline) return;
  fetch('/api/save-subscribers', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body:JSON.stringify(payload)
  }).then(()=>{
    state.subscribers=payload.subscribers;
    state.subscriber_filtering_enabled=payload.filtering_enabled;
    subscriberFormBaseline = serialized;
    updateSubscribersSaveButton();
    refreshSubscriberSessions();
    fetchState();
  });
}
function exportSubscribers() {
  const rows = [...document.querySelectorAll('.subscriber-row')];
  const lines = [t('subscribers')];
  rows.forEach((row, index) => {
    const value = field => row.querySelector(`[data-field="${field}"]`)?.value.trim() || '—';
    const enabled = row.querySelector('[data-field="enabled"]')?.checked !== false;
    const streams = [...row.querySelectorAll('[data-stream-id]:checked')].map(input => {
      const label = input.closest('label');
      return label ? label.textContent.trim() : input.dataset.streamId;
    });
    lines.push('', `${index + 1}. ${value('name')}`,
      `IP: ${value('primary_ip')}`,
      `Backup IP: ${value('backup_ip')}`,
      `${t('addedAt')}: ${row.dataset.addedAt || '—'}`,
      `${t('status')}: ${enabled ? t('enabled') : t('disabled')}`,
      `${t('streams')}: ${streams.length ? streams.join(', ') : '—'}`);
  });
  const blob = new Blob([lines.join('\n') + '\n'], {type:'text/plain;charset=utf-8'});
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = 'tvstreamer5-subscribers.txt';
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}
function editStream(id) {
  const stream = state.streams.find(s=>s.id===id);
  if (!stream) return;
  openStreamForm(stream);
}
function openAboutModal() {
  openModal(`
    <h2>${t('about')}</h2>
    <div class="about-list">
      <div class="about-row"><strong>${t('name')}</strong><span>Лукомский Виталий</span></div>
      <div class="about-row"><strong>${t('country')}</strong><span>Беларусь, г. Борисов</span></div>
      <div class="about-row"><strong>Email</strong><a href="mailto:monkipnet@gmail.com">monkipnet@gmail.com</a></div>
      <div class="about-row about-donate"><strong>${t('donate')}</strong><div class="about-donate-content">
        <svg class="about-qr" viewBox="0 0 41 41" role="img" aria-label="${t('donateQr')}" shape-rendering="crispEdges">
          <rect width="41" height="41" fill="#fff"></rect>
          <path d="${donateQrPath}" fill="#111"></path>
        </svg>
        <span class="about-donate-address">${donateAddress}</span>
      </div></div>
    </div>
    <div class="modal-actions">
      <button class="button-primary" onclick="closeModal()">${t('close')}</button>
    </div>
  `);
}
function openLoginModal() {
  openModal(`
    <h2>${t('userTitle')}</h2>
    <div class="form-grid full">
      <div class="form-row"><label>Login</label><input id="login" value="${state.login||''}" /></div>
      <div class="form-row"><label>Новый пароль</label><input id="password" type="password" placeholder="Оставьте пустым, чтобы не менять" /></div>
      <div class="form-row"><label>Имя сервера</label><input id="serverName" value="${state.server_name||''}" /></div>
      <div class="form-row"><label>Порт web-интерфейса</label><input id="httpPort" type="number" min="1" max="65535" value="${state.http_port||9000}" /></div>
    </div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">${t('cancel')}</button>
      <button class="button-primary" onclick="saveSettings()">${t('save')}</button>
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
    name:'', input_uri:'', backup_input_uri:'', output_type:'udp-cbr', output_mode:'listener', output_host:'127.0.0.1', output_port:1234,
    interface_address:'', input_mode:'auto', test_pattern:false, auto_start:false, remap_enabled:false, cbr:true, target_bitrate:2000000,
    audio_pid:0, video_pid:0, service_id:1, service_name:'', service_provider:'', additional_outputs:[]
  });
}
function outputTypeOptions(selected) {
  const options = [
    ['udp-vbr', 'UDP MPEG-TS VBR'],
    ['udp-cbr', 'UDP MPEG-TS CBR'],
    ['srt', 'SRT'],
    ['http', 'HTTP TS'],
    ['hls', 'HLS'],
    ['rtmp', 'RTMP Push'],
    ['youtube', 'YouTube']
  ];
  return options.map(([value, label]) => `<option value="${value}" ${selected===value?'selected':''}>${label}</option>`).join('');
}
function renderOutputRows(outputs, links=[], startIndex=0) {
  return outputs.map((output, offset) => {
    const index = startIndex + offset;
    const type = normalizedOutputType(output);
    const link = links[index]?.url || '';
    return `
      <div class="output-row" data-output-index="${index}">
        <div class="form-row"><label>${index === 0 ? 'Основной формат' : 'Доп. формат'}</label><select data-output-field="output_type" onchange="updateOutputHints()">${outputTypeOptions(type)}</select></div>
        <div class="form-row"><label>SRT режим</label><select data-output-field="output_mode" onchange="updateOutputHints()"><option value="listener" ${(!output.output_mode || output.output_mode==='listener')?'selected':''}>Listener</option><option value="caller" ${output.output_mode==='caller'?'selected':''}>Caller</option></select></div>
        <div class="form-row"><label data-output-host-label>Адрес выхода</label><input data-output-field="output_host" value="${output.output_host||'239.0.0.1'}" placeholder="239.0.0.1" /></div>
        <div class="form-row"><label data-output-port-label>Порт</label><input data-output-field="output_port" type="number" min="1" max="65535" value="${output.output_port||1234}" placeholder="1234" /></div>
        <button class="remove-output" type="button" onclick="removeStreamOutput(this)" ${index === 0 ? 'disabled' : ''}>×</button>
        <div class="form-row full" style="grid-column:1/-1"><label>URL для плеера</label><input readonly value="${link}" placeholder="Ссылка появится после сохранения" /></div>
      </div>
    `;
  }).join('');
}
function renumberOutputRows() {
  document.querySelectorAll('.output-row').forEach((row, index) => {
    row.dataset.outputIndex = index;
    const label = row.querySelector('label');
    if (label) label.textContent = index === 0 ? 'Основной формат' : 'Доп. формат';
    const remove = row.querySelector('.remove-output');
    if (remove) remove.disabled = index === 0;
  });
}
function addStreamOutput() {
  const list = document.getElementById('streamOutputs');
  if (!list) return;
  const index = list.querySelectorAll('.output-row').length;
  const iface = document.getElementById('streamInterface')?.value || '127.0.0.1';
  list.insertAdjacentHTML('beforeend', renderOutputRows([{output_type:'hls', output_mode:'listener', output_host:iface, output_port:state.http_port || 9000, cbr:document.getElementById('streamCbr')?.checked}], [], index));
  updateOutputHints();
}
function removeStreamOutput(button) {
  const row = button.closest('.output-row');
  if (!row || Number(row.dataset.outputIndex || 0) === 0) return;
  row.remove();
  renumberOutputRows();
  updateOutputHints();
}
function collectOutputRows() {
  const rows = [...document.querySelectorAll('.output-row')];
  return rows.map(row => {
    const value = field => row.querySelector(`[data-output-field="${field}"]`)?.value || '';
    return {
      output_type: value('output_type') || 'udp-cbr',
      output_mode: value('output_mode') || 'listener',
      output_host: value('output_host') || '127.0.0.1',
      output_port: Number(value('output_port') || 1234)
    };
  });
}
function openStreamForm(stream) {
  const renderStreamForm = () => {
    const ifaceOptions = state.interfaces || [];
    const options = ifaceOptions.map(i=>`<option value="${i.address}" ${i.address===stream.interface_address?'selected':''}>${i.name} (${i.address})</option>`).join('');
    const outputs = outputConfigsForStream(stream);
    const outputType = normalizedOutputType(outputs[0] || stream);
    const links = Array.isArray(stream.vlc_links) ? stream.vlc_links : [];
    openModal(`
      <h2>${stream.name ? 'Редактирование трансляции' : 'Настройка трансляции'}</h2>
      <div class="form-grid">
        <div class="form-row full"><label>Имя плитки</label><input class="compact" id="streamName" value="${stream.name||''}" placeholder="Belarus 5" /></div>
        <div class="form-row full"><label>Входной URL (Основной)</label><input class="compact" id="streamInput" value="${stream.input_uri||''}" placeholder="rtsp://camera/live, udp://127.0.0.1:9087, rtmp://camera/live/stream или https://host/live.m3u8" /></div>
        <div class="form-row full"><label>Входной URL (Резервный)</label><input class="compact" id="streamBackupInput" value="${stream.backup_input_uri||''}" placeholder="http://192.168.1.2/..." /></div>
        <div class="form-row full"><label>Тестовая таблица</label><div class="checkbox-inline"><input id="streamTestPattern" type="checkbox" ${stream.test_pattern ? 'checked' : ''} /><span>Использовать вместо входных потоков</span></div></div>
        <div class="form-row full"><label>Интерфейс вывода</label><select class="compact" id="streamInterface" onchange="syncOutputHostWithInterface()"><option value="">Auto / все интерфейсы</option>${options}</select></div>
        <div class="form-row"><label>Режим входа</label><select class="compact" id="streamInputMode"><option value="auto" ${(!stream.input_mode || stream.input_mode==='auto')?'selected':''}>Auto</option><option value="hls" ${stream.input_mode==='hls'?'selected':''}>HLS</option><option value="caller" ${stream.input_mode==='caller'?'selected':''}>SRT Caller</option><option value="listener" ${stream.input_mode==='listener'?'selected':''}>SRT Listener</option></select></div>
        <div class="form-row full"><label>Выходные форматы</label><div id="streamOutputs" class="output-list">${renderOutputRows(outputs, links)}</div><button class="button-secondary" type="button" onclick="addStreamOutput()">+ Добавить формат</button></div>
        <div class="form-row full"><label>V-PID / A-PID</label><div class="row-inline compact-row"><input class="compact" id="streamAudioPid" type="number" value="${stream.audio_pid||257}" placeholder="257" /><input class="compact" id="streamVideoPid" type="number" value="${stream.video_pid||258}" placeholder="258" /></div></div>
        <div class="form-row"><label>SID</label><input class="compact" id="streamServiceId" type="number" value="${stream.service_id||1}" placeholder="1" /></div>
        <div class="form-row full"><label>Имя Канала и Провайдер</label><div class="row-inline compact-row"><input class="compact" id="streamServiceName" value="${stream.service_name||''}" placeholder="Belarus 5" /><input class="compact" id="streamProvider" value="${stream.service_provider||''}" placeholder="BTRC" /></div></div>
        <div class="form-row full"><label>Target bitrate (кбит/с)</label><input id="streamBitrate" type="number" value="${Math.round((stream.target_bitrate||2000000)/1000)}" placeholder="2000" /></div>
        <div class="form-row full"><label>Автозапуск</label><div class="checkbox-inline"><input id="streamAutoStart" type="checkbox" ${stream.auto_start ? 'checked' : ''} /><span>Запускать после перезапуска программы</span></div></div>
        <div class="form-row full" id="streamCbrRow"><label>Включить CBR</label><div class="checkbox-inline"><input id="streamCbr" type="checkbox" ${stream.cbr ? 'checked' : ''} /><span>CBR</span></div></div>
        <div class="form-row full"><label>Включить Remap</label><div class="checkbox-inline"><input id="streamRemapEnabled" type="checkbox" ${stream.remap_enabled ? 'checked' : ''} /><span>Remap PID / Service</span></div></div>
      </div>
      <div class="modal-actions">
        <button class="button-secondary" onclick="closeModal()">Отмена</button>
        <button class="button-primary" onclick="saveStream('${stream.id}')">Сохранить</button>
      </div>
    `);
    document.getElementById('streamCbr').checked = outputType === 'udp-cbr' || (outputType !== 'udp-vbr' && stream.cbr);
    updateOutputHints();
  };

  if (!state.interfaces || !state.interfaces.length) {
    loadInterfaces().then(renderStreamForm);
  } else {
    renderStreamForm();
  }
}
function updateOutputHints() {
  const rows = [...document.querySelectorAll('.output-row')];
  const cbrRow = document.getElementById('streamCbrRow');
  const cbrInput = document.getElementById('streamCbr');
  rows.forEach(row => {
    const type = row.querySelector('[data-output-field="output_type"]')?.value || 'udp-cbr';
    const outputMode = row.querySelector('[data-output-field="output_mode"]')?.value || 'listener';
    const hostLabel = row.querySelector('[data-output-host-label]');
    const portLabel = row.querySelector('[data-output-port-label]');
    const host = row.querySelector('[data-output-field="output_host"]');
    const port = row.querySelector('[data-output-field="output_port"]');
    const modeRow = row.querySelector('[data-output-field="output_mode"]')?.closest('.form-row');
    if (!hostLabel || !portLabel || !host || !port) return;
    if (modeRow) modeRow.style.display = type === 'srt' ? '' : 'none';
    if (type === 'http' || type === 'hls') {
      hostLabel.textContent = 'Адрес для ссылки';
      portLabel.textContent = type === 'hls' ? 'HLS порт' : 'HTTP порт';
      port.disabled = false;
      port.placeholder = String(state.http_port || 9000);
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
  });
  if (cbrInput && cbrRow) {
    const primaryType = rows[0]?.querySelector('[data-output-field="output_type"]')?.value || 'udp-cbr';
    const udpMode = primaryType === 'udp-cbr' || primaryType === 'udp-vbr';
    cbrInput.checked = primaryType === 'udp-cbr' || (!udpMode && cbrInput.checked);
    cbrInput.disabled = udpMode;
    cbrRow.style.display = udpMode ? 'none' : '';
  }
  syncOutputHostWithInterface();
}
function syncOutputHostWithInterface() {
  const iface = document.getElementById('streamInterface')?.value || '';
  if (!iface) return;
  document.querySelectorAll('.output-row').forEach(row => {
    const type = row.querySelector('[data-output-field="output_type"]')?.value || 'udp-cbr';
    const host = row.querySelector('[data-output-field="output_host"]');
    if (!host || (type !== 'http' && type !== 'hls')) return;
    if (!host.value || host.value === '0.0.0.0' || host.value === '127.0.0.1') {
      host.value = iface;
    }
  });
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
  const outputs = collectOutputRows();
  const primaryOutput = outputs[0] || {output_type:'udp-cbr', output_mode:'listener', output_host:'127.0.0.1', output_port:1234};
  const selectedOutputType = primaryOutput.output_type;
  const selectedCbr = selectedOutputType === 'udp-cbr'
    ? true
    : (selectedOutputType === 'udp-vbr' ? false : document.getElementById('streamCbr').checked);
  const payload = {
    id: id,
    name: document.getElementById('streamName').value,
    input_uri: document.getElementById('streamInput').value,
    output_type: selectedOutputType,
    output_mode: primaryOutput.output_mode,
    output_host: primaryOutput.output_host,
    output_port: primaryOutput.output_port,
    additional_outputs: outputs.slice(1),
    backup_input_uri: document.getElementById('streamBackupInput').value,
    interface_address: document.getElementById('streamInterface').value,
    input_mode: document.getElementById('streamInputMode').value,
    test_pattern: document.getElementById('streamTestPattern').checked,
    auto_start: document.getElementById('streamAutoStart').checked,
    remap_enabled: document.getElementById('streamRemapEnabled').checked,
    cbr: selectedCbr,
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
function copyStreamLinks(id, button) {
  const stream = (state.streams || []).find(item => item.id === id);
  if (!stream) return;
  const text = streamLinks(stream).map(link => link.url).join('\n') || stream.vlc_link || '';
  copyLink(text, button);
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
  const outputs = outputConfigsForStream(stream);
  qualityChart.streamId = id;
  qualityChart.period = periodSeconds;
  document.getElementById('modalContent').className = 'modal-content quality-modal';
  const tabs = qualityPeriods.map(p=>`<button class="${p.seconds===periodSeconds?'active':''}" onclick="loadQualityHistory('${id}', ${p.seconds})">${p.label}</button>`).join('');
  document.getElementById('modalContent').innerHTML = modalCloseButton() + `
    <div class="quality-head">
      <div class="quality-title">
        <h2>Качество потока</h2>
        <small>${stream.name || stream.id} · ${outputs.map(output => `${normalizedOutputType(output).toUpperCase()} ${output.output_host}:${output.output_port}`).join(' · ')}</small>
      </div>
      <div class="period-tabs">${tabs}</div>
    </div>
    <div class="quality-board">
      <canvas id="qualityCanvas" width="860" height="230"></canvas>
    </div>
    <div class="quality-board cc-board">
      <canvas id="ccCanvas" width="860" height="150"></canvas>
    </div>
    <div class="quality-legend">
      <span><i class="quality-line quality-input"></i>Входной битрейт</span>
      <span><i class="quality-line quality-output"></i>Исходящий битрейт</span>
      <span><i class="quality-line quality-cc"></i>CC-errors на отдельном графике</span>
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
  const ccCanvas = document.getElementById('ccCanvas');
  const details = document.getElementById('qualityDetails');
  const errors = document.getElementById('qualityErrors');
  if (!canvas || !ccCanvas || !details || !errors) return;
  const setupCanvas = (target, height) => {
    const targetRect = target.getBoundingClientRect();
    const targetRatio = window.devicePixelRatio || 1;
    target.width = Math.max(640, Math.floor(targetRect.width * targetRatio));
    target.height = Math.floor(height * targetRatio);
    const targetContext = target.getContext('2d');
    targetContext.setTransform(targetRatio, 0, 0, targetRatio, 0, 0);
    return {ctx: targetContext, width: target.width / targetRatio, height};
  };
  const chart = setupCanvas(canvas, 230);
  const ccChart = setupCanvas(ccCanvas, 150);
  const ctx = chart.ctx;
  const ccCtx = ccChart.ctx;
  const width = chart.width;
  const height = chart.height;
  const ccWidth = ccChart.width;
  const ccHeight = ccChart.height;
  ctx.clearRect(0, 0, width, height);
  ccCtx.clearRect(0, 0, ccWidth, ccHeight);
  const samples = data.samples || [];
  if (!samples.length) {
    ctx.fillStyle = '#9aa3b1';
    ctx.textAlign = 'center';
    ctx.fillText('История пока пустая. Данные появятся после нескольких обновлений состояния.', width / 2, height / 2);
    ccCtx.fillStyle = '#9aa3b1';
    ccCtx.textAlign = 'center';
    ccCtx.fillText('Нет данных CC-errors', ccWidth / 2, ccHeight / 2);
    details.innerHTML = '<div class="quality-card"><strong>Нет данных</strong>История собирается в памяти во время работы приложения.</div>';
    errors.innerHTML = '';
    qualityChart.points = [];
    return;
  }
  const left = 54, right = 46, top = 16, bottom = 34;
  const plotW = width - left - right;
  const plotH = height - top - bottom;
  const ccLeft = 54, ccRight = 46, ccTop = 16, ccBottom = 28;
  const ccPlotW = ccWidth - ccLeft - ccRight;
  const ccPlotH = ccHeight - ccTop - ccBottom;
  const endTs = data.generated_at || Math.floor(Date.now()/1000);
  const startTs = endTs - (data.period_seconds || qualityChart.period);
  const maxBitrate = Math.max(1000, ...samples.map(s=>Math.max(s.input_kbps || 0, s.output_kbps || 0))) * 1.15;
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
  qualityChart.points = [];
  const lastSample = samples[samples.length - 1] || {};
  samples.forEach(s => {
    const x = xFor(s.ts);
    const y = yFor(s.output_kbps || s.input_kbps || 0);
    ctx.fillStyle = qualityColor(s.level);
    ctx.beginPath();
    ctx.arc(x, y, s.level !== 'ok' ? 5 : 3, 0, Math.PI * 2);
    ctx.fill();
    qualityChart.points.push({x, y, sample:s});
  });
  const maxCcErrors = Math.max(1, ...samples.map(s=>s.cc_errors || 0));
  const ccYFor = value => ccTop + ccPlotH - (Math.min(value, maxCcErrors) / maxCcErrors) * ccPlotH;
  ccCtx.strokeStyle = 'rgba(255,255,255,.09)';
  ccCtx.fillStyle = '#ff9c9c';
  ccCtx.font = '9px Arial';
  ccCtx.textAlign = 'right';
  for (let i=0;i<=4;i++) {
    const y = ccTop + ccPlotH * i / 4;
    ccCtx.beginPath(); ccCtx.moveTo(ccLeft, y); ccCtx.lineTo(ccWidth - ccRight, y); ccCtx.stroke();
    ccCtx.fillText(Math.round(maxCcErrors * (1 - i / 4)) + ' cc', ccLeft - 7, y + 4);
  }
  ccCtx.textAlign = 'center';
  ccCtx.fillStyle = '#8e99aa';
  for (let i=0;i<=6;i++) {
    const x = ccLeft + ccPlotW * i / 6;
    const ts = startTs + (endTs - startTs) * i / 6;
    ccCtx.beginPath(); ccCtx.moveTo(x, ccTop); ccCtx.lineTo(x, ccTop + ccPlotH); ccCtx.stroke();
    ccCtx.fillText(formatTime(ts, data.period_seconds), x, ccHeight - 8);
  }
  const ccXFor = ts => ccLeft + ((ts - startTs) / Math.max(1, endTs - startTs)) * ccPlotW;
  ccCtx.strokeStyle = '#ff5f5f';
  ccCtx.fillStyle = 'rgba(255,95,95,.28)';
  ccCtx.lineWidth = 2;
  ccCtx.beginPath();
  let ccStarted = false;
  samples.forEach(s => {
    const x = ccXFor(s.ts);
    const y = ccYFor(s.cc_errors || 0);
    if ((s.cc_errors || 0) > 0) ccCtx.fillRect(x - 2, y, 4, ccTop + ccPlotH - y);
    if (!ccStarted) { ccCtx.moveTo(x, y); ccStarted = true; } else { ccCtx.lineTo(x, y); }
  });
  ccCtx.stroke();
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
  applyLanguage();
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
