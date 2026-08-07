#pragma once

#include "protocols/GstOutputProtocols.h"

namespace tvs::protocols::outputs {

bool appendRtpSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec);

} // namespace tvs::protocols::outputs
