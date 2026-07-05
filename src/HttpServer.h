#pragma once

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <jsoncpp/json/json.h>
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
    void doAccept();
    void handleSession(tcp::socket socket);
    std::string listInterfaces();
    std::string currentState();
    void handleSaveConfig(const std::string& body);
    void handleStartStream(const std::string& body);
    void handleStopStream(const std::string& body);
    std::string renderIndexPage();

    tcp::acceptor acceptor;
    ConfigManager& configManager;
    StreamManager& streamManager;
    std::unordered_map<std::string, std::function<void(const boost::asio::ip::tcp::socket&)>> endpointHandlers;
};
