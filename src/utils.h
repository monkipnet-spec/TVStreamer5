#pragma once

#include <string>
#include <vector>

struct NetworkInterface {
    std::string name;
    std::string address;
};

std::string toLower(const std::string& value);
std::string gstQuote(const std::string& value);
std::vector<NetworkInterface> enumerateNetworkInterfaces();
