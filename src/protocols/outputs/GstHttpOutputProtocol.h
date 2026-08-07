#pragma once
#include "protocols/GstOutputProtocols.h"
namespace tvs::protocols::outputs {
bool appendHttpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec);
}
