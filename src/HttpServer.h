#pragma once

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <jsoncpp/json/json.h>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <functional>
#include <unordered_map>

#include "ConfigManager.h"
#include "StreamManager.h"

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

class HttpServer {
public:
    HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm);
    bool start();
    void addEndpoint(const std::string& path, std::function<void(const boost::asio::ip::tcp::socket&)> handler);

private:
    struct QualitySample {
        int64_t timestamp = 0;
        bool active = false;
        uint64_t inputKbps = 0;
        uint64_t outputKbps = 0;
        uint64_t targetKbps = 0;
        std::string status;
        std::string level;
        std::string message;
    };

    void doAccept();
    void handleSession(tcp::socket socket);
    bool requiresAuthentication(const std::string& target) const;
    bool isAuthorized(const http::request<http::string_body>& req) const;
    void writeUnauthorized(http::response<http::string_body>& res) const;
    std::string listInterfaces();
    std::string currentState();
    std::string qualityHistory(const std::string& target);
    bool handleHttpStream(tcp::socket& socket, const std::string& target);
    bool serveHlsFile(const std::string& target, http::response<http::string_body>& res);
    void handleSaveConfig(const std::string& body);
    void handleStartStream(const std::string& body);
    void handleStopStream(const std::string& body);
    std::string renderIndexPage();
    void recordQualitySample(const StreamConfig& cfg, const Json::Value& state);

    tcp::acceptor acceptor;
    ConfigManager& configManager;
    StreamManager& streamManager;
    std::mutex qualityMutex;
    std::unordered_map<std::string, std::deque<QualitySample>> qualitySamples;
    std::unordered_map<std::string, std::function<void(const boost::asio::ip::tcp::socket&)>> endpointHandlers;
};
