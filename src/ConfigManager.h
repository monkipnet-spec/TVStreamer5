#pragma once

#include <jsoncpp/json/json.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct StreamConfig {
    std::string id;
    std::string name;
    std::string inputUri;
    std::string backupInputUri;
    std::string outputType = "udp";
    std::string outputMode = "listener";
    std::string outputHost = "127.0.0.1";
    int outputPort = 1234;
    std::string interfaceAddress;
    std::string inputMode = "auto";
    bool autoStart = false;
    bool remapEnabled = false;
    bool cbr = true;
    uint64_t targetBitrate = 8000000;
    uint32_t audioPid = 0;
    uint32_t videoPid = 0;
    uint32_t serviceId = 1;
    std::string serviceName;
    std::string serviceProvider;

    Json::Value toJson() const;
    static StreamConfig fromJson(const Json::Value& root);
};

struct AppConfig {
    std::string login = "admin";
    std::string password = "admin";
    std::string serverName = "TVStreamer5";
    int httpPort = 9000;
    std::string telegramToken;
    std::string telegramChatId;
    std::vector<StreamConfig> streams;

    Json::Value toJson() const;
    static AppConfig fromJson(const Json::Value& root);
};

class ConfigManager {
public:
    ConfigManager();
    bool load();
    bool save();

    AppConfig config;

private:
    std::filesystem::path configPath;
    std::mutex fileMutex;
};
