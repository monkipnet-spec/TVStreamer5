#include <gst/gst.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/algorithm/string.hpp>
#include <json/json.h>
#include <curl/curl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <vector>
#include <string>
#include <chrono>
#include <filesystem>
#include <iomanip>

using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

static std::string toLower(const std::string& value) {
    std::string lower = value;
    boost::algorithm::to_lower(lower);
    return lower;
}

static std::string gstQuote(const std::string& value) {
    std::string result = "'";
    for (char c : value) {
        if (c == '\'' ) {
            result += "\\'";
        } else if (c == '\\') {
            result += "\\\\";
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

struct NetworkInterface {
    std::string name;
    std::string address;
};

static std::vector<NetworkInterface> enumerateNetworkInterfaces() {
    std::vector<NetworkInterface> list;
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        return list;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        char host[NI_MAXHOST] = {0};
        int result = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST,
                                 nullptr, 0, NI_NUMERICHOST);
        if (result != 0) continue;
        std::string name(ifa->ifa_name);
        if (name == "lo") continue;
        list.push_back({name, std::string(host)});
    }

    freeifaddrs(ifaddr);
    return list;
}

struct StreamConfig {
    std::string id;
    std::string name;
    std::string inputUri;
    std::string outputHost = "127.0.0.1";
    int outputPort = 1234;
    std::string interfaceAddress;
    bool cbr = true;
    uint64_t targetBitrate = 2000000;
    uint32_t audioPid = 0;
    uint32_t videoPid = 0;
    std::string serviceName;
    std::string serviceProvider;

    Json::Value toJson() const {
        Json::Value root;
        root["id"] = id;
        root["name"] = name;
        root["input_uri"] = inputUri;
        root["output_host"] = outputHost;
        root["output_port"] = outputPort;
        root["interface_address"] = interfaceAddress;
        root["cbr"] = cbr;
        root["target_bitrate"] = Json::UInt64(targetBitrate);
        root["audio_pid"] = audioPid;
        root["video_pid"] = videoPid;
        root["service_name"] = serviceName;
        root["service_provider"] = serviceProvider;
        return root;
    }

    static StreamConfig fromJson(const Json::Value& root) {
        StreamConfig config;
        config.id = root.get("id", "").asString();
        config.name = root.get("name", "").asString();
        config.inputUri = root.get("input_uri", "").asString();
        config.outputHost = root.get("output_host", "127.0.0.1").asString();
        config.outputPort = root.get("output_port", 1234).asInt();
        config.interfaceAddress = root.get("interface_address", "").asString();
        config.cbr = root.get("cbr", true).asBool();
        config.targetBitrate = root.get("target_bitrate", Json::UInt64(2000000)).asUInt64();
        config.audioPid = root.get("audio_pid", 0).asUInt();
        config.videoPid = root.get("video_pid", 0).asUInt();
        config.serviceName = root.get("service_name", "").asString();
        config.serviceProvider = root.get("service_provider", "").asString();
        return config;
    }
};

struct AppConfig {
    std::string login = "admin";
    std::string password = "admin";
    int httpPort = 9000;
    std::string telegramToken;
    std::string telegramChatId;
    std::vector<StreamConfig> streams;

    Json::Value toJson() const {
        Json::Value root;
        root["login"] = login;
        root["password"] = password;
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

    static AppConfig fromJson(const Json::Value& root) {
        AppConfig config;
        config.login = root.get("login", "admin").asString();
        config.password = root.get("password", "admin").asString();
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
};

class ConfigManager {
public:
    ConfigManager() {
        configPath = std::filesystem::current_path() / "tvstreamer5-config.json";
    }

    bool load() {
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

    bool save() {
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

    AppConfig config;

private:
    std::filesystem::path configPath;
    std::mutex fileMutex;
};

class TelegramNotifier {
public:
    explicit TelegramNotifier(ConfigManager& cfg) : manager(cfg) {}

    void sendMessage(const std::string& text) {
        const auto& config = manager.config;
        if (config.telegramToken.empty() || config.telegramChatId.empty()) {
            return;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            return;
        }

        std::ostringstream url;
        url << "https://api.telegram.org/bot" << curl_easy_escape(curl, config.telegramToken.c_str(), 0)
            << "/sendMessage?chat_id=" << curl_easy_escape(curl, config.telegramChatId.c_str(), 0)
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

private:
    ConfigManager& manager;
};

struct StreamState {
    std::atomic<bool> active{false};
    std::atomic<bool> running{false};
    std::string statusMessage = "stopped";
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::thread busThread;
};

class StreamManager {
public:
    StreamManager(ConfigManager& cfg, TelegramNotifier& notifier)
        : configManager(cfg), telegramNotifier(notifier), gstreamerInitialized(false) {
        std::cerr << "StreamManager constructed" << std::endl;
    }

    ~StreamManager() {
        stopAll();
    }

    bool startStream(const StreamConfig& streamConfig) {
        std::lock_guard<std::mutex> lock(managerMutex);
        if (streams.count(streamConfig.id)) {
            return false;
        }

        if (!gstreamerInitialized) {
            gst_init(nullptr, nullptr);
            gstreamerInitialized = true;
        }

        std::string pipelineDescription = buildPipelineDescription(streamConfig);
        GError* error = nullptr;
        GstElement* pipeline = gst_parse_launch(pipelineDescription.c_str(), &error);
        if (!pipeline) {
            std::cerr << "Failed to create pipeline: " << (error ? error->message : "unknown") << std::endl;
            if (error) g_error_free(error);
            return false;
        }

        auto state = std::make_unique<StreamState>();
        state->pipeline = pipeline;
        state->bus = gst_element_get_bus(pipeline);
        state->running = true;
        state->active = true;
        state->statusMessage = "starting";
        state->busThread = std::thread(&StreamManager::monitorBus, this, streamConfig.id);

        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        streams[streamConfig.id] = std::move(state);
        telegramNotifier.sendMessage("Stream started: " + streamConfig.name);
        return true;
    }

    bool stopStream(const std::string& id) {
        std::lock_guard<std::mutex> lock(managerMutex);
        if (!streams.count(id)) {
            return false;
        }
        auto& state = *streams[id];
        state.running = false;
        if (state.pipeline) {
            gst_element_set_state(state.pipeline, GST_STATE_NULL);
            state.active = false;
            state.statusMessage = "stopped";
        }
        if (state.busThread.joinable()) {
            state.busThread.join();
        }
        if (state.bus) {
            gst_object_unref(state.bus);
            state.bus = nullptr;
        }
        if (state.pipeline) {
            gst_object_unref(state.pipeline);
            state.pipeline = nullptr;
        }
        streams.erase(id);
        telegramNotifier.sendMessage("Stream stopped: " + id);
        return true;
    }

    void stopAll() {
        std::lock_guard<std::mutex> lock(managerMutex);
        for (auto& [id, statePtr] : streams) {
            auto& state = *statePtr;
            state.running = false;
            if (state.pipeline) {
                gst_element_set_state(state.pipeline, GST_STATE_NULL);
            }
            if (state.busThread.joinable()) {
                state.busThread.join();
            }
            if (state.bus) {
                gst_object_unref(state.bus);
            }
            if (state.pipeline) {
                gst_object_unref(state.pipeline);
            }
        }
        streams.clear();
    }

    std::vector<std::string> activeStreams() {
        std::lock_guard<std::mutex> lock(managerMutex);
        std::vector<std::string> result;
        for (auto& [id, statePtr] : streams) {
            if (statePtr->active.load()) {
                result.push_back(id);
            }
        }
        return result;
    }

    std::map<std::string, StreamState*> snapshot() {
        std::lock_guard<std::mutex> lock(managerMutex);
        std::map<std::string, StreamState*> result;
        for (auto& [id, statePtr] : streams) {
            result[id] = statePtr.get();
        }
        return result;
    }

private:
    bool gstreamerInitialized;

    std::string buildPipelineDescription(const StreamConfig& cfg) {
        std::ostringstream desc;
        std::string src;
        std::string absIntf = cfg.interfaceAddress.empty() ? "0.0.0.0" : cfg.interfaceAddress;

        if (toLower(cfg.inputUri).rfind("http://", 0) == 0 ||
            toLower(cfg.inputUri).rfind("https://", 0) == 0 ||
            toLower(cfg.inputUri).rfind("srt://", 0) == 0 ||
            toLower(cfg.inputUri).rfind("rtp://", 0) == 0 ||
            toLower(cfg.inputUri).rfind("udp://", 0) == 0) {
            desc << "urisourcebin uri=" << gstQuote(cfg.inputUri) << " name=src ! ";
        }

        desc << "tsdemux name=demux demux. ! queue ! h264parse ! mpegtsmux name=mux "
             << "alignment=7 bitrate=" << (cfg.cbr ? cfg.targetBitrate : 0) << " ";

        if (!cfg.serviceName.empty()) {
            desc << "service-name=" << gstQuote(cfg.serviceName) << " ";
        }
        if (!cfg.serviceProvider.empty()) {
            desc << "service-provider=" << gstQuote(cfg.serviceProvider) << " ";
        }

        desc << "! udpsink host=" << gstQuote(cfg.outputHost) << " port=" << cfg.outputPort
             << " bind-address=" << gstQuote(absIntf) << " async=false sync=false";

        return desc.str();
    }

    void monitorBus(const std::string& id) {
        auto found = streams.find(id);
        if (found == streams.end()) {
            return;
        }
        StreamState* state = found->second.get();
        GstBus* bus = state->bus;
        while (state->running.load()) {
            GstMessage* msg = gst_bus_timed_pop(bus, 1000000000LL);
            if (!msg) continue;
            switch (GST_MESSAGE_TYPE(msg)) {
                case GST_MESSAGE_ERROR: {
                    GError* err = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_error(msg, &err, &dbg);
                    std::string message = err ? err->message : "unknown";
                    g_error_free(err);
                    g_free(dbg);
                    state->statusMessage = "error: " + message;
                    state->active = false;
                    telegramNotifier.sendMessage("Stream error: " + found->first + " -> " + message);
                    gst_message_unref(msg);
                    return;
                }
                case GST_MESSAGE_EOS:
                    state->statusMessage = "ended";
                    state->active = false;
                    telegramNotifier.sendMessage("Stream ended: " + found->first);
                    gst_message_unref(msg);
                    return;
                default:
                    gst_message_unref(msg);
                    break;
            }
        }
    }

    ConfigManager& configManager;
    TelegramNotifier& telegramNotifier;
    std::map<std::string, std::unique_ptr<StreamState>> streams;
    std::mutex managerMutex;
};

class HttpServer {
public:
    HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm)
        : acceptor(ioc), configManager(cfg), streamManager(sm) {
    }

    bool start() {
        try {
            tcp::endpoint endpoint(tcp::v4(), configManager.config.httpPort);
            acceptor.open(endpoint.protocol());
            acceptor.set_option(boost::asio::socket_base::reuse_address(true));
            acceptor.bind(endpoint);
            acceptor.listen();
            doAccept();
            return true;
        } catch (const std::exception& ex) {
            std::cerr << "HTTP server failed to start: " << ex.what() << std::endl;
            return false;
        }
    }

private:
    void doAccept() {
        acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::thread(&HttpServer::handleSession, this, std::move(socket)).detach();
            }
            doAccept();
        });
    }

    void handleSession(tcp::socket socket) {
        try {
            boost::beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(socket, buffer, req);
            http::response<http::string_body> res{http::status::ok, req.version()};
            res.set(http::field::server, "TVStreamer5");
            res.set(http::field::content_type, "text/html; charset=UTF-8");
            res.keep_alive(req.keep_alive());

            if (req.method() == http::verb::get) {
                if (req.target() == "/" || req.target() == "/index.html") {
                    res.body() = renderIndexPage();
                } else if (req.target() == "/api/interfaces") {
                    res.set(http::field::content_type, "application/json");
                    res.body() = listInterfaces();
                } else if (req.target() == "/api/state") {
                    res.set(http::field::content_type, "application/json");
                    res.body() = currentState();
                } else {
                    res.result(http::status::not_found);
                    res.body() = "Not Found";
                }
            } else if (req.method() == http::verb::post) {
                if (req.target() == "/api/save-config") {
                    handleSaveConfig(req.body());
                    res.set(http::field::content_type, "application/json");
                    res.body() = "{\"result\": \"ok\"}";
                } else if (req.target() == "/api/start-stream") {
                    handleStartStream(req.body());
                    res.set(http::field::content_type, "application/json");
                    res.body() = "{\"result\": \"ok\"}";
                } else if (req.target() == "/api/stop-stream") {
                    handleStopStream(req.body());
                    res.set(http::field::content_type, "application/json");
                    res.body() = "{\"result\": \"ok\"}";
                } else {
                    res.result(http::status::not_found);
                    res.body() = "Not Found";
                }
            } else {
                res.result(http::status::method_not_allowed);
                res.body() = "Method Not Allowed";
            }

            res.content_length(res.body().size());
            http::write(socket, res);
        } catch (const std::exception& ex) {
            std::cerr << "HTTP session failed: " << ex.what() << std::endl;
        }
    }

    std::string listInterfaces() {
        Json::Value root;
        auto interfaces = enumerateNetworkInterfaces();
        for (auto& iface : interfaces) {
            Json::Value item;
            item["name"] = iface.name;
            item["address"] = iface.address;
            root.append(item);
        }
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, root);
    }

    std::string currentState() {
        Json::Value root;
        root["login"] = configManager.config.login;
        root["http_port"] = configManager.config.httpPort;
        root["telegram_token"] = configManager.config.telegramToken;
        root["telegram_chat_id"] = configManager.config.telegramChatId;
        root["stream_count"] = Json::UInt(configManager.config.streams.size());
        root["active_count"] = Json::UInt(streamManager.activeStreams().size());
        Json::Value streams(Json::arrayValue);
        for (const auto& cfg : configManager.config.streams) {
            Json::Value item = cfg.toJson();
            auto snap = streamManager.snapshot();
            if (snap.count(cfg.id)) {
                item["active"] = true;
                item["status"] = snap.at(cfg.id)->statusMessage;
            } else {
                item["active"] = false;
                item["status"] = "stopped";
            }
            item["vlc_link"] = "udp://@" + cfg.outputHost + ":" + std::to_string(cfg.outputPort);
            streams.append(item);
        }
        root["streams"] = streams;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, root);
    }

    void handleSaveConfig(const std::string& body) {
        Json::CharReaderBuilder readerBuilder;
        Json::Value root;
        std::string errs;
        std::istringstream ss(body);
        if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
            std::cerr << "Invalid config payload: " << errs << std::endl;
            return;
        }
        configManager.config = AppConfig::fromJson(root);
        configManager.save();
    }

    void handleStartStream(const std::string& body) {
        Json::CharReaderBuilder readerBuilder;
        Json::Value root;
        std::string errs;
        std::istringstream ss(body);
        if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
            std::cerr << "Invalid start-stream payload: " << errs << std::endl;
            return;
        }
        auto cfg = StreamConfig::fromJson(root);
        streamManager.startStream(cfg);
    }

    void handleStopStream(const std::string& body) {
        Json::CharReaderBuilder readerBuilder;
        Json::Value root;
        std::string errs;
        std::istringstream ss(body);
        if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
            std::cerr << "Invalid stop-stream payload: " << errs << std::endl;
            return;
        }
        std::string id = root.get("id", "").asString();
        streamManager.stopStream(id);
    }

    std::string renderIndexPage() {
        static const std::string html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="X-UA-Compatible" content="IE=edge">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TVStreamer5</title>
<style>
body{font-family:Arial,Helvetica,sans-serif;background:#121212;color:#EEE;margin:0;padding:0}
header{display:flex;align-items:center;justify-content:space-between;padding:12px;background:#1F1F1F;flex-wrap:wrap}
button{padding:8px 12px;margin:0 4px 8px 0;border:none;border-radius:8px;color:#FFF;background:#3A3A3A;cursor:pointer;font-size:0.95rem}
button:hover{background:#505050}
.container{padding:12px}
.stats{display:flex;gap:10px;margin-bottom:12px;flex-wrap:wrap}
.tile-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}
.tile{background:#202020;padding:12px;border-radius:12px;border:1px solid #333;display:flex;flex-direction:column;gap:10px;min-height:150px}
.tile.active{border-color:#2E7D32;background:#24321D}
.tile.error{border-color:#B71C1C;background:#3D1F1F}
.tile h2{margin:0 0 8px;font-size:1rem}
.tile .item{display:flex;justify-content:space-between;font-size:0.88rem}
.modal{position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.75);display:none;align-items:center;justify-content:center;padding:10px}
.modal.active{display:flex}
.modal-content{background:#181818;padding:16px;border-radius:14px;width:min(600px,100%);max-height:90%;overflow:auto}
.modal-content h2{margin-top:0;font-size:1.1rem}
input,select{width:100%;padding:8px;margin-top:6px;margin-bottom:12px;background:#111;border:1px solid #333;border-radius:8px;color:#EEE;font-size:0.95rem}
textarea{width:100%;padding:8px;background:#111;border:1px solid #333;border-radius:8px;color:#EEE;font-size:0.95rem}
</style>
</head>
<body>
<header>
<div>
<button onclick="openLoginModal()">Login/Password</button>
<button onclick="openTelegramModal()">Telegram</button>
<button onclick="openStreamModal()">New Stream</button>
</div>
<div>
<span id="totalCount">Streams: 0</span>
<span id="activeCount" style="margin-left:16px">Active: 0</span>
</div>
</header>
<div class="container">
<div id="tiles" class="tile-grid"></div>
</div>
<div id="modal" class="modal">
<div class="modal-content" id="modalContent"></div>
</div>
<script>
let state = {};
function fetchState() {
  fetch('/api/state').then(r=>r.json()).then(data=>{
    state=data; render();
  });
}
function openModal(html) {
  document.getElementById('modalContent').innerHTML = html;
  document.getElementById('modal').classList.add('active');
}
function closeModal() { document.getElementById('modal').classList.remove('active'); }
function render() {
  document.getElementById('totalCount').textContent = 'Streams: ' + state.stream_count;
  document.getElementById('activeCount').textContent = 'Active: ' + state.active_count;
  const tiles = document.getElementById('tiles');
  tiles.innerHTML = '';
  state.streams.forEach(stream => {
    const tile = document.createElement('div');
    tile.className = 'tile' + (stream.active ? ' active' : '');
    tile.innerHTML = `
      <h2>${stream.name || stream.id}</h2>
      <div class="item"><strong>Status:</strong><span>${stream.status}</span></div>
      <div class="item"><strong>Input:</strong><span>${stream.input_uri}</span></div>
      <div class="item"><strong>Output:</strong><span>${stream.output_host}:${stream.output_port}</span></div>
      <div class="item"><strong>VLC:</strong><span>${stream.vlc_link}</span></div>
      <div style="display:flex;gap:10px;flex-wrap:wrap">
        <button onclick="toggleStream('${stream.id}', ${stream.active})">${stream.active ? 'Stop' : 'Start'}</button>
        <button onclick="editStream('${stream.id}')">Edit</button>
        <button onclick="copyLink('${stream.vlc_link}')">Copy Link</button>
      </div>`;
    tiles.appendChild(tile);
  });
}
function toggleStream(id, active) {
  const url = active ? '/api/stop-stream' : '/api/start-stream';
  const body = active ? {id} : state.streams.find(s=>s.id===id);
  fetch(url, {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(()=>setTimeout(fetchState,500));
}
function editStream(id) {
  const stream = state.streams.find(s=>s.id===id);
  if (!stream) return;
  openStreamForm(stream);
}
function openLoginModal() {
  openModal(`
    <h2>Login / Password</h2>
    <label>Login<input id="login" value="${state.login||''}" /></label>
    <label>Password<input id="password" type="password" value="${state.password||''}" /></label>
    <button onclick="saveSettings()">Save</button>
    <button onclick="closeModal()">Cancel</button>
  `);
}
function openTelegramModal() {
  openModal(`
    <h2>Telegram Settings</h2>
    <label>Token<input id="telegramToken" value="${state.telegram_token||''}" /></label>
    <label>Chat ID<input id="telegramChatId" value="${state.telegram_chat_id||''}" /></label>
    <button onclick="saveSettings()">Save</button>
    <button onclick="closeModal()">Cancel</button>
  `);
}
function openStreamModal() {
  openStreamForm({
    id: 'stream-' + Date.now(),
    name:'', input_uri:'', output_host:'127.0.0.1', output_port:1234,
    interface_address:'', cbr:true, target_bitrate:2000000,
    audio_pid:0, video_pid:0, service_name:'', service_provider:''
  });
}
function openStreamForm(stream) {
  const ifaceOptions = state.interfaces || [];
  const options = ifaceOptions.map(i=>`<option value="${i.address}" ${i.address===stream.interface_address?'selected':''}>${i.name} (${i.address})</option>`).join('');
  openModal(`
    <h2>${stream.name ? 'Edit Stream' : 'New Stream'}</h2>
    <label>Name<input id="streamName" value="${stream.name||''}" /></label>
    <label>Input URI<input id="streamInput" value="${stream.input_uri||''}" /></label>
    <label>Output host<input id="streamOutputHost" value="${stream.output_host||'127.0.0.1'}" /></label>
    <label>Output port<input id="streamOutputPort" type="number" value="${stream.output_port||1234}" /></label>
    <label>Interface<select id="streamInterface"><option value=""></option>${options}</select></label>
    <label>CBR<select id="streamCbr"><option value="true">CBR</option><option value="false">VBR</option></select></label>
    <label>Target Bitrate<input id="streamBitrate" type="number" value="${stream.target_bitrate||2000000}" /></label>
    <label>Audio PID<input id="streamAudioPid" type="number" value="${stream.audio_pid||0}" /></label>
    <label>Video PID<input id="streamVideoPid" type="number" value="${stream.video_pid||0}" /></label>
    <label>Service Name<input id="streamServiceName" value="${stream.service_name||''}" /></label>
    <label>Service Provider<input id="streamProvider" value="${stream.service_provider||''}" /></label>
    <button onclick="saveStream('${stream.id}')">Save</button>
    <button onclick="closeModal()">Cancel</button>
  `);
  document.getElementById('streamCbr').value = stream.cbr ? 'true' : 'false';
}
function saveSettings() {
  const payload = {
    login: document.getElementById('login')?.value || state.login,
    password: document.getElementById('password')?.value || state.password,
    telegram_token: document.getElementById('telegramToken')?.value || state.telegram_token,
    telegram_chat_id: document.getElementById('telegramChatId')?.value || state.telegram_chat_id,
    http_port: state.http_port,
    streams: state.streams
  };
  fetch('/api/save-config', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
    .then(()=>{closeModal();fetchState();});
}
function saveStream(id) {
  const payload = {
    id: id,
    name: document.getElementById('streamName').value,
    input_uri: document.getElementById('streamInput').value,
    output_host: document.getElementById('streamOutputHost').value,
    output_port: Number(document.getElementById('streamOutputPort').value),
    interface_address: document.getElementById('streamInterface').value,
    cbr: document.getElementById('streamCbr').value === 'true',
    target_bitrate: Number(document.getElementById('streamBitrate').value),
    audio_pid: Number(document.getElementById('streamAudioPid').value),
    video_pid: Number(document.getElementById('streamVideoPid').value),
    service_name: document.getElementById('streamServiceName').value,
    service_provider: document.getElementById('streamProvider').value
  };
  const existingIndex = state.streams.findIndex(s=>s.id===id);
  if (existingIndex >= 0) {
    state.streams[existingIndex] = payload;
  } else {
    state.streams.push(payload);
  }
  const savePayload = {
    login: state.login,
    password: state.password,
    telegram_token: state.telegram_token,
    telegram_chat_id: state.telegram_chat_id,
    http_port: state.http_port,
    streams: state.streams
  };
  fetch('/api/save-config', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(savePayload)})
    .then(()=>{closeModal();fetchState();});
}
function copyLink(text) {
  navigator.clipboard.writeText(text);
}
function loadInterfaces() {
  fetch('/api/interfaces').then(r=>r.json()).then(data=>{state.interfaces=data;});
}
window.onload = () => { fetchState(); loadInterfaces(); };
window.onclick = e => { if (e.target.id === 'modal') closeModal(); };
</script>
</body>
</html>
)HTML";
        return html;
    }

    tcp::acceptor acceptor;
    ConfigManager& configManager;
    StreamManager& streamManager;
};

int main() {
    std::cerr << "main() entered" << std::endl;
    ConfigManager configManager;
    if (!configManager.load()) {
        std::cerr << "Unable to load or create configuration." << std::endl;
        return 1;
    }

    std::cerr << "Config loaded: http_port=" << configManager.config.httpPort << " login=" << configManager.config.login << std::endl;

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
