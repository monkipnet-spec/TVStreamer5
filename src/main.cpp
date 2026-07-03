#include <iostream>
#include <boost/asio.hpp>

#include "ConfigManager.h"
#include "TelegramNotifier.h"
#include "StreamManager.h"
#include "HttpServer.h"

int main() {
    std::cerr << "main() entered" << std::endl;

    ConfigManager configManager;
    if (!configManager.load()) {
        std::cerr << "Unable to load or create configuration." << std::endl;
        return 1;
    }

    std::cerr << "Config loaded: http_port=" << configManager.config.httpPort
              << " login=" << configManager.config.login << std::endl;

    TelegramNotifier notifier(configManager);
    StreamManager streamManager(configManager, notifier);

    boost::asio::io_context ioc;
    HttpServer server(ioc, configManager, streamManager);
    if (!server.start()) {
        std::cerr << "HTTP server start failed" << std::endl;
        return 1;
    }

    std::cerr << "HTTP server started" << std::endl;
    std::cout << "TVStreamer5 running on port " << configManager.config.httpPort << std::endl;
    std::cerr << "Calling ioc.run()" << std::endl;

    ioc.run();
    return 0;
}
