#include "HttpServer.h"

#include "utils.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/algorithm/string.hpp>
#include <iostream>
#include <sstream>

HttpServer::HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm)
    : acceptor(ioc), configManager(cfg), streamManager(sm) {
}

bool HttpServer::start() {
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

void HttpServer::doAccept() {
    acceptor.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::thread(&HttpServer::handleSession, this, std::move(socket)).detach();
        }
        doAccept();
    });
}

void HttpServer::handleSession(tcp::socket socket) {
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

std::string HttpServer::listInterfaces() {
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

std::string HttpServer::currentState() {
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
            item["bitrate_in_kbps"] = Json::UInt64(snap.at(cfg.id)->currentBitrate.load() / 1000);
            item["jitter_ms"] = Json::UInt64(snap.at(cfg.id)->currentJitterMs.load());
        } else {
            item["active"] = false;
            item["status"] = "stopped";
            item["bitrate_in_kbps"] = Json::UInt64(0);
            item["jitter_ms"] = Json::UInt64(0);
        }
        item["vlc_link"] = "udp://@" + cfg.outputHost + ":" + std::to_string(cfg.outputPort);
        streams.append(item);
    }
    root["streams"] = streams;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

void HttpServer::handleSaveConfig(const std::string& body) {
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

void HttpServer::handleStartStream(const std::string& body) {
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

void HttpServer::handleStopStream(const std::string& body) {
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

std::string HttpServer::renderIndexPage() {
    static const std::string html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="X-UA-Compatible" content="IE=edge">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TVStreamer5</title>
<style>
body{font-family:Arial,Helvetica,sans-serif;background:#0f1218;color:#EEE;margin:0;padding:0;min-height:100vh}
body:before{content:'';position:fixed;inset:0;background:radial-gradient(circle at top left,rgba(40,160,255,.18),transparent 28%),radial-gradient(circle at top right,rgba(120,90,255,.15),transparent 22%),linear-gradient(180deg,#10131a 0%,#090c12 100%);pointer-events:none;z-index:-1}
header{display:flex;align-items:center;justify-content:space-between;padding:8px 10px;background:rgba(19,23,31,.95);backdrop-filter:blur(10px);border-bottom:1px solid rgba(255,255,255,.06);gap:12px;flex-wrap:wrap}
.header-left{display:flex;align-items:flex-start;gap:10px}
.header-left .title{font-size:1.05rem;font-weight:700;letter-spacing:.02em;color:#fff}
.header-left .subtitle{font-size:.78rem;color:#9aa3b1;margin-top:2px}
.header-right{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.header-center{display:flex;align-items:center;justify-content:center;gap:12px}
.button-primary{padding:8px 14px;border:none;border-radius:999px;color:#FFF;background:#1f8bff;cursor:pointer;font-size:0.88rem;transition:background .2s ease}
.button-secondary{padding:7px 12px;border:1px solid rgba(255,255,255,.14);border-radius:999px;color:#EEE;background:rgba(255,255,255,.05);cursor:pointer;font-size:0.82rem;transition:background .2s ease,border-color .2s ease}
.button-primary:hover{background:#0f7ce7}
.button-secondary:hover{background:rgba(255,255,255,.1);border-color:rgba(255,255,255,.24)}
.container{padding:10px 12px 12px;max-width:1180px;margin:0 auto}
.stats-panel{display:grid;grid-template-columns:repeat(2,minmax(100px,1fr));gap:10px;padding:8px 12px;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.08);border-radius:16px}
.stats-panel .status{display:flex;flex-direction:column;gap:3px;font-size:.78rem;color:#d1d9ed}
.stats-panel .status strong{color:#fff;font-size:.78rem}
.stats-panel .status span{font-size:1rem;font-weight:700;color:#fff}
.tile-grid{display:grid;grid-template-columns:repeat(8,180px);gap:12px 1ch;justify-content:start}
.tile{position:relative;background:rgba(22,27,37,.94);padding:10px 10px 10px 16px;border-radius:18px;border:1px solid rgba(255,255,255,.06);display:flex;flex-direction:column;gap:6px;min-height:130px;width:100%;max-width:180px;box-sizing:border-box;box-shadow:0 18px 42px rgba(0,0,0,.14);transition:transform .2s ease,border-color .2s ease}
.tile:before{content:'';position:absolute;left:0;top:12px;bottom:12px;width:4px;border-radius:999px;background:linear-gradient(180deg,#3fc8ff,#1d69ff)}
.tile:hover{transform:translateY(-1px);border-color:rgba(31,136,255,.3)}
.tile.active{border-color:#17c261}
.tile.error{border-color:#fb5f5f}
.tile .top{display:flex;align-items:center;justify-content:space-between;gap:6px}
.tile .title{font-size:.84rem;font-weight:700;line-height:1.2;color:#fff}
.tile .badge{padding:2px 5px;background:rgba(20,161,255,.14);color:#7dd1ff;border-radius:999px;font-size:.64rem;text-transform:uppercase;letter-spacing:.08em}
.tile .status-pill{padding:2px 6px;background:rgba(255,255,255,.06);color:#c9d2e4;border-radius:999px;font-size:.64rem;text-transform:uppercase;letter-spacing:.08em}
.tile .status-pill.active{background:rgba(23,194,97,.15);color:#b6f7c2}
.tile .status-pill.stopped{background:rgba(255,95,95,.14);color:#ffb3b3}
.tile .info{display:grid;grid-template-columns:1fr;gap:5px;font-size:.78rem;color:#b3b8c6}
.tile .info-row{display:flex;justify-content:space-between;gap:8px;align-items:center}
.tile .info-row strong{color:#fff;font-size:.78rem}
.tile .info-row span{max-width:140px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;text-align:right}
.tile .controls{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:6px}
.tile .controls button{padding:7px 8px;border:none;border-radius:10px;background:rgba(255,255,255,.06);color:#EEE;font-size:.78rem;cursor:pointer;transition:background .2s ease}
.tile .controls button:hover{background:rgba(255,255,255,.12)}
.modal{position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(8,10,15,.78);display:none;align-items:center;justify-content:center;padding:12px;z-index:20}
.modal.active{display:flex}
.modal-content{background:rgba(11,15,22,.985);padding:18px 18px;border-radius:22px;width:min(520px,100%);max-height:92%;overflow:auto;box-shadow:0 28px 70px rgba(0,0,0,.24);border:1px solid rgba(255,255,255,.08)}
.modal-content h2{margin-top:0;font-size:1.25rem;margin-bottom:14px;color:#fff}
.form-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.form-grid.full{grid-template-columns:1fr}
.form-row.full{grid-column:1/-1}
.form-row{display:flex;flex-direction:column;gap:8px;align-items:flex-start}
.form-row label{font-size:.78rem;color:#9aa3b1}
.form-row input,.form-row select{width:100%;max-width:210px;padding:7px 9px;background:#121825;border:1px solid rgba(255,255,255,.08);border-radius:8px;color:#EEE;font-size:.8rem}
.form-row input.compact,.form-row select.compact{max-width:150px}
.row-inline{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.row-inline.compact-row input{width:100%;padding:7px 9px}
.form-row-inline small-field input{width:calc(100% - 8px)}
.form-row .checkbox-inline{display:flex;align-items:center;gap:10px;margin-top:8px}
.form-row .checkbox-inline input{width:16px;height:16px}
.modal-actions{display:flex;justify-content:flex-end;gap:10px;margin-top:16px}
.modal-actions button{min-width:100px;padding:8px 12px}
</style>
</head>
<body>
<header>
<div class="header-left">
<div>
<div class="title">Control Panel</div>
<div class="subtitle">Мониторинг трансляций и управление потоками</div>
</div>
</div>
<div class="header-center">
<div class="stats-panel">
<div class="status"><strong>Всего:</strong> <span id="totalCount">0</span></div>
<div class="status"><strong>Активно:</strong> <span id="activeCount">0</span></div>
</div>
</div>
<div class="header-right">
<button class="button-secondary" onclick="openLoginModal()">Пользователь</button>
<button class="button-secondary" onclick="openTelegramModal()">Telegram API</button>
<button class="button-primary" onclick="openStreamModal()">+ Добавить поток</button>
</div>
</header>
<div class="container">
<div id="tiles" class="tile-grid"></div>
<div id="modal" class="modal">
<div class="modal-content" id="modalContent"></div>
</div>
<script>
let state = {};
function fetchState() {
  fetch('/api/state').then(r=>r.json()).then(data=>{state=data; render();});
}
function openModal(html) {
  document.getElementById('modalContent').innerHTML = html;
  document.getElementById('modal').classList.add('active');
}
function closeModal() { document.getElementById('modal').classList.remove('active'); }
function render() {
  document.getElementById('totalCount').textContent = state.stream_count;
  document.getElementById('activeCount').textContent = state.active_count;
  const tiles = document.getElementById('tiles');
  tiles.innerHTML = '';
  state.streams.forEach(stream => {
    const tile = document.createElement('div');
    tile.className = 'tile' + (stream.active ? ' active' : '');
    tile.innerHTML = `
      <div class="top">
        <div>
          <div class="title">${stream.name || stream.id}</div>
          <div class="status-pill ${stream.active ? 'active' : 'stopped'}">${stream.active ? 'Online' : 'Offline'}</div>
        </div>
        <div class="badge">${stream.cbr ? 'CBR' : 'VBR'}</div>
      </div>
      <div class="info">
        <div class="info-row"><strong>Вывод</strong><span>${stream.output_host}:${stream.output_port}</span></div>
        <div class="info-row"><strong>Вход</strong><span>${stream.input_uri || '—'}</span></div>
        <div class="info-row"><strong>Резерв</strong><span>${stream.backup_input_uri || '—'}</span></div>
        <div class="info-row"><strong>SID</strong><span>${stream.service_id || '—'}</span></div>
        <div class="info-row"><strong>Битрейт</strong><span>${stream.bitrate_in_kbps ? stream.bitrate_in_kbps + ' kbps' : '—'}</span></div>
        <div class="info-row"><strong>Jitter</strong><span>${stream.jitter_ms ? stream.jitter_ms + ' ms' : '—'}</span></div>
        <div class="info-row"><strong>Статус</strong><span>${stream.status}</span></div>
        <div class="info-row"><strong>VLC</strong><span>${stream.vlc_link}</span></div>
      </div>
      <div class="controls">
        <button onclick="toggleStream('${stream.id}', ${stream.active})">${stream.active ? 'Стоп' : 'Старт'}</button>
        <button onclick="editStream('${stream.id}')">Ред.</button>
        <button onclick="copyLink('${stream.vlc_link}')">Копия</button>
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
    <h2>Пользователь</h2>
    <div class="form-grid full">
      <div class="form-row"><label>Login</label><input id="login" value="${state.login||''}" /></div>
      <div class="form-row"><label>Password</label><input id="password" type="password" value="${state.password||''}" /></div>
    </div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">Отмена</button>
      <button class="button-primary" onclick="saveSettings()">Сохранить</button>
    </div>
  `);
}
function openTelegramModal() {
  openModal(`
    <h2>Telegram API</h2>
    <div class="form-grid full">
      <div class="form-row"><label>Token</label><input id="telegramToken" value="${state.telegram_token||''}" /></div>
      <div class="form-row"><label>Chat ID</label><input id="telegramChatId" value="${state.telegram_chat_id||''}" /></div>
    </div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">Отмена</button>
      <button class="button-primary" onclick="saveSettings()">Сохранить</button>
    </div>
  `);
}
function openStreamModal() {
  openStreamForm({
    id: 'stream-' + Date.now(),
    name:'', input_uri:'', backup_input_uri:'', output_host:'127.0.0.1', output_port:1234,
    interface_address:'', cbr:true, target_bitrate:2000000,
    audio_pid:0, video_pid:0, service_id:1, service_name:'', service_provider:''
  });
}
function openStreamForm(stream) {
  const renderStreamForm = () => {
    const ifaceOptions = state.interfaces || [];
    const options = ifaceOptions.map(i=>`<option value="${i.address}" ${i.address===stream.interface_address?'selected':''}>${i.name} (${i.address})</option>`).join('');
    openModal(`
      <h2>${stream.name ? 'Редактирование трансляции' : 'Настройка трансляции'}</h2>
      <div class="form-grid">
        <div class="form-row full"><label>Имя плитки</label><input class="compact" id="streamName" value="${stream.name||''}" placeholder="Belarus 5" /></div>
        <div class="form-row full"><label>Входной URL (Основной)</label><input class="compact" id="streamInput" value="${stream.input_uri||''}" placeholder="udp://127.0.0.1:9087" /></div>
        <div class="form-row full"><label>Входной URL (Резервный)</label><input class="compact" id="streamBackupInput" value="${stream.backup_input_uri||''}" placeholder="http://192.168.1.2/..." /></div>
        <div class="form-row full"><label>Интерфейс вывода (UDP)</label><select class="compact" id="streamInterface"><option value=""></option>${options}</select></div>
        <div class="form-row"><label>Мультикаст IP</label><input class="compact" id="streamOutputHost" value="${stream.output_host||'239.0.0.1'}" placeholder="239.0.0.1" /></div>
        <div class="form-row"><label>Порт</label><input class="compact" id="streamOutputPort" type="number" value="${stream.output_port||1234}" placeholder="1234" /></div>
        <div class="form-row full"><label>V-PID / A-PID</label><div class="row-inline compact-row"><input class="compact" id="streamAudioPid" type="number" value="${stream.audio_pid||257}" placeholder="257" /><input class="compact" id="streamVideoPid" type="number" value="${stream.video_pid||258}" placeholder="258" /></div></div>
        <div class="form-row"><label>SID</label><input class="compact" id="streamServiceId" type="number" value="${stream.service_id||1}" placeholder="1" /></div>
        <div class="form-row full"><label>Имя Канала и Провайдер</label><div class="row-inline compact-row"><input class="compact" id="streamServiceName" value="${stream.service_name||''}" placeholder="Belarus 5" /><input class="compact" id="streamProvider" value="${stream.service_provider||''}" placeholder="BTRC" /></div></div>
        <div class="form-row full"><label>Target bitrate (кбит/с)</label><input id="streamBitrate" type="number" value="${Math.round((stream.target_bitrate||2000000)/1000)}" placeholder="2000" /></div>
        <div class="form-row full"><label>Включить CBR</label><div class="checkbox-inline"><input id="streamCbr" type="checkbox" ${stream.cbr ? 'checked' : ''} /><span>CBR</span></div></div>
      </div>
      <div class="modal-actions">
        <button class="button-secondary" onclick="closeModal()">Отмена</button>
        <button class="button-primary" onclick="saveStream('${stream.id}')">Сохранить</button>
      </div>
    `);
    document.getElementById('streamCbr').checked = stream.cbr;
  };

  if (!state.interfaces || !state.interfaces.length) {
    loadInterfaces().then(renderStreamForm);
  } else {
    renderStreamForm();
  }
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
    backup_input_uri: document.getElementById('streamBackupInput').value,
    interface_address: document.getElementById('streamInterface').value,
    cbr: document.getElementById('streamCbr').checked,
    target_bitrate: Number(document.getElementById('streamBitrate').value) * 1000,
    audio_pid: Number(document.getElementById('streamAudioPid').value),
    video_pid: Number(document.getElementById('streamVideoPid').value),
    service_id: Number(document.getElementById('streamServiceId').value),
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
  return fetch('/api/interfaces')
    .then(r=>r.json())
    .then(data=>{ state.interfaces=data; return data; })
    .catch(() => { state.interfaces=[]; return []; });
}
window.onload = () => { fetchState(); loadInterfaces(); };
window.onclick = e => { if (e.target.id === 'modal') closeModal(); };
</script>
</body>
</html>
)HTML";
    return html;
}
