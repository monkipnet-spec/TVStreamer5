#include "ConfigManager.h"

#include <fstream>
#include <iostream>
#include <sstream>

StreamConfig StreamConfig::fromJson(const Json::Value& root) {
    StreamConfig config;
    config.id = root.get("id", "").asString();
    config.name = root.get("name", "").asString();
    config.inputUri = root.get("input_uri", "").asString();
    config.backupInputUri = root.get("backup_input_uri", "").asString();
    config.outputType = root.get("output_type", "udp").asString();
    config.outputMode = root.get("output_mode", "listener").asString();
    config.outputHost = root.get("output_host", "127.0.0.1").asString();
    config.outputPort = root.get("output_port", 1234).asInt();
    config.interfaceAddress = root.get("interface_address", "").asString();
    config.inputMode = root.get("input_mode", "auto").asString();
    config.autoStart = root.get("auto_start", false).asBool();
    config.remapEnabled = root.get("remap_enabled", false).asBool();
    config.cbr = root.get("cbr", true).asBool();
    config.targetBitrate = root.get("target_bitrate", Json::UInt64(8000000)).asUInt64();
    config.audioPid = root.get("audio_pid", 0).asUInt();
    config.videoPid = root.get("video_pid", 0).asUInt();
    config.serviceId = root.get("service_id", 1).asUInt();
    config.serviceName = root.get("service_name", "").asString();
    config.serviceProvider = root.get("service_provider", "").asString();
    return config;
}

Json::Value StreamConfig::toJson() const {
    Json::Value root;
    root["id"] = id;
    root["name"] = name;
    root["input_uri"] = inputUri;
    root["backup_input_uri"] = backupInputUri;
    root["output_type"] = outputType;
    root["output_mode"] = outputMode;
    root["output_host"] = outputHost;
    root["output_port"] = outputPort;
    root["interface_address"] = interfaceAddress;
    root["input_mode"] = inputMode;
    root["auto_start"] = autoStart;
    root["remap_enabled"] = remapEnabled;
    root["cbr"] = cbr;
    root["target_bitrate"] = Json::UInt64(targetBitrate);
    root["audio_pid"] = audioPid;
    root["video_pid"] = videoPid;
    root["service_id"] = serviceId;
    root["service_name"] = serviceName;
    root["service_provider"] = serviceProvider;
    return root;
}

Json::Value AppConfig::toJson() const {
    Json::Value root;
    root["login"] = login;
    root["password"] = password;
    root["server_name"] = serverName;
    root["http_port"] = httpPort;
    root["telegram_token"] = telegramToken;
    root["telegram_chat_id"] = telegramChatId;
    Json::Value list(Json::arrayValue);
    for (const auto& stream : streams) {
        list.append(stream.toJson());
    }
    root["streams"] = list;
    return root;
}

AppConfig AppConfig::fromJson(const Json::Value& root) {
    AppConfig config;
    config.login = root.get("login", "admin").asString();
    config.password = root.get("password", "admin").asString();
    config.serverName = root.get("server_name", "TVStreamer5").asString();
    config.httpPort = root.get("http_port", 9000).asInt();
    config.telegramToken = root.get("telegram_token", "").asString();
    config.telegramChatId = root.get("telegram_chat_id", "").asString();
    if (root.isMember("streams") && root["streams"].isArray()) {
        for (const auto& item : root["streams"]) {
            config.streams.push_back(StreamConfig::fromJson(item));
        }
    }
    return config;
}

ConfigManager::ConfigManager() {
    configPath = std::filesystem::current_path() / "tvstreamer5-config.json";
}

bool ConfigManager::load() {
    if (!std::filesystem::exists(configPath)) {
        std::cerr << "Config file not found, creating default configuration: " << configPath << std::endl;
        AppConfig defaultConfig;
        {
            std::lock_guard<std::mutex> lock(fileMutex);
            config = defaultConfig;
        }
        return save();
    }

    std::lock_guard<std::mutex> lock(fileMutex);
    std::ifstream input(configPath);
    if (!input.is_open()) {
        return false;
    }
    Json::Value root;
    Json::CharReaderBuilder readerBuilder;
    std::string errs;
    bool ok = Json::parseFromStream(readerBuilder, input, &root, &errs);
    if (!ok) {
        std::cerr << "Failed to parse config: " << errs << std::endl;
        return false;
    }
    config = AppConfig::fromJson(root);
    return true;
}

bool ConfigManager::save() {
    std::lock_guard<std::mutex> lock(fileMutex);
    std::ofstream output(configPath);
    if (!output.is_open()) {
        std::cerr << "Unable to open config file for writing: " << configPath << std::endl;
        return false;
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    std::string str = Json::writeString(writer, config.toJson());
    output << str;
    return true;
}
