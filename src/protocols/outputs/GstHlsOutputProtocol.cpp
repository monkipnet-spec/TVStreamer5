#include "protocols/outputs/GstHlsOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"
namespace tvs::protocols::outputs {
bool appendHlsSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    appendTsSmoother(args, "transcode_hls_ts_smoother", 250000);
    appendOutputQueue(args, "transcode_hls_output_queue", false);
    const std::string dir = hlsDirectory(cfg);
    args.insert(args.end(), {
        "hlssink",
        "playlist-location=" + dir + "/playlist.m3u8",
        "location=" + dir + "/segment%05d.ts",
        "target-duration=4",
        "max-files=10",
        "playlist-length=6"
    });
    assignTsPads(cfg, spec);
    spec.description = "hls@" + dir + "/playlist.m3u8";
    return true;
}
}
