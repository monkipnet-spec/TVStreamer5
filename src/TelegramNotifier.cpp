#include "TelegramNotifier.h"

#include <curl/curl.h>
#include <iostream>
#include <sstream>

TelegramNotifier::TelegramNotifier(ConfigManager& cfg)
    : manager(cfg) {
}

void TelegramNotifier::sendMessage(const std::string& text) {
    const auto& config = manager.config;
    if (config.telegramToken.empty() || config.telegramChatId.empty()) {
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return;
    }

    std::ostringstream url;
    url << "https://api.telegram.org/bot"
        << curl_easy_escape(curl, config.telegramToken.c_str(), 0)
        << "/sendMessage?chat_id="
        << curl_easy_escape(curl, config.telegramChatId.c_str(), 0)
        << "&text=" << curl_easy_escape(curl, text.c_str(), 0);

    curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "Telegram send error: " << curl_easy_strerror(res) << std::endl;
    }
    curl_easy_cleanup(curl);
}
