#pragma once

#include <string>

#include "ConfigManager.h"

class TelegramNotifier {
public:
    explicit TelegramNotifier(ConfigManager& cfg);
    void sendMessage(const std::string& text);

private:
    ConfigManager& manager;
};
