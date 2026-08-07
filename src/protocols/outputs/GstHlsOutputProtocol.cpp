#include "protocols/outputs/GstHlsOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"
namespace tvs::protocols::outputs {
bool appendHlsSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    appendOutputQueue(args, "transcode_hls_output_queue", true);
    const std::string dir = hlsDirectory(cfg);
    args.insert(args.end(), {
        "hlssink",
        "playlist-location=" + dir + "/playlist.m3u8",
        "location=" + dir + "/segment%05d.ts",
        "target-duration=3",
        "max-files=10"
    });
    assignTsPads(cfg, spec);
    spec.description = "hls@" + dir + "/playlist.m3u8";
    return true;
}
}
