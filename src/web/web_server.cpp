#include "oamr/web/web_server.hpp"

#include "oamr/audio/audio_backend.hpp"
#include "oamr/audio/backend_factory.hpp"
#include "oamr/pairing/pairing_service.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace oamr::web {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void close_socket(Socket socket) { close(socket); }
#endif

std::string json_escape(const std::string& value) {
    std::string result;
    for (const unsigned char ch : value) {
        if (ch == '"') result += "\\\"";
        else if (ch == '\\') result += "\\\\";
        else if (ch < 0x20) result += ' ';
        else result += static_cast<char>(ch);
    }
    return result;
}

std::string url_decode(const std::string& value) {
    std::string result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+' ) result += ' ';
        else if (value[index] == '%' && index + 2 < value.size()) {
            const auto hex = value.substr(index + 1, 2);
            result += static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16));
            index += 2;
        } else result += value[index];
    }
    return result;
}

std::unordered_map<std::string, std::string> query_params(const std::string& target) {
    std::unordered_map<std::string, std::string> result;
    const auto start = target.find('?');
    if (start == std::string::npos) return result;
    std::stringstream stream(target.substr(start + 1));
    std::string item;
    while (std::getline(stream, item, '&')) {
        const auto delimiter = item.find('=');
        result[url_decode(item.substr(0, delimiter))] = url_decode(delimiter == std::string::npos ? "" : item.substr(delimiter + 1));
    }
    return result;
}

std::string content_type_for(const std::filesystem::path& path) {
    const auto extension = path.extension().string();
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".js") return "text/javascript; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".ico") return "image/x-icon";
    return "application/octet-stream";
}

std::optional<std::string> static_asset(const std::string& target, std::string& type) {
    const auto query = target.find('?');
    const std::string request_path = target.substr(0, query);
    if (request_path != "/" && request_path.rfind("/assets/", 0) != 0) return std::nullopt;

    const std::filesystem::path relative = request_path == "/" ? "index.html" : request_path.substr(1);
    const auto normalized = relative.lexically_normal();
    if (normalized.empty() || normalized.is_absolute() || normalized.string().starts_with("..")) return std::nullopt;

    const auto file = std::filesystem::current_path() / "web" / normalized;
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) return std::nullopt;
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    type = content_type_for(file);
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

const char* kPairControlPage = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>OAMR</title>
<style>:root{color-scheme:dark}*{box-sizing:border-box}body{font:15px system-ui;margin:0;background:#101218;color:#eef1f7}.wrap{max-width:1260px;margin:auto;padding:30px 22px}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px}.wide{grid-column:1/-1}.card{background:#191d27;border:1px solid #303746;border-radius:12px;padding:20px}.muted{color:#aab3c3;margin:.4em 0 1em}h1,h2{margin:0 0 8px}h2{font-size:1.18rem}label{display:block;margin:10px 0 4px}input,select,button{font:inherit;padding:10px;border-radius:7px;border:1px solid #465066;background:#10131a;color:#eef1f7;width:100%}button{border:0;background:#6d5dfc;font-weight:700;cursor:pointer;margin-top:14px}.secondary{background:#303746}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.matrix{overflow:auto}table{border-collapse:collapse;min-width:720px;width:100%}th,td{border:1px solid #3b4352;padding:9px;text-align:center}th{background:#222836}td.disabled{background:#151821;color:#697386}input[type=checkbox]{width:20px;height:20px}.expose{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;max-height:210px;overflow:auto}.expose label{margin:0;padding:8px;background:#131720;border-radius:6px}.expose input{width:auto;margin-right:7px}code{font-size:1.35rem;letter-spacing:.18em;background:#10131a;padding:7px 10px;border-radius:6px}#status{white-space:pre-wrap;min-height:42px}.peer{border-top:1px solid #303746;padding:10px 0}.badge{display:inline-block;background:#29314a;border-radius:99px;padding:4px 9px;font-size:.82em}.editable{cursor:pointer;text-decoration:underline;text-decoration-style:dotted}@media(max-width:760px){.grid,.row,.expose{grid-template-columns:1fr}.wide{grid-column:auto}}</style>
<main class=wrap><h1>Open Audio Matrix Router</h1><p class=muted><span class=badge>仅本机 Web UI</span> 配对控制端口为 TCP 8791；网页不会暴露到局域网。</p><div class=grid>
<section class="card wide"><h2>路由表</h2><p class=muted>每条线路独立运行；网络线路的音质、最大延迟和模式只在这里修改。</p><div id=routeTable class=muted>暂无路由。</div><button class=secondary onclick=stopRoute()>停止并清空所有路由</button></section>
<section class=card><h2>直接发送到局域网</h2><p class=muted>用于未配对设备；配对设备优先在矩阵中创建。</p><label>音频来源</label><select id=networkSource></select><div class=row><input id=sendHost value=127.0.0.1 placeholder="IP / 主机名"><input id=sendPort type=number value=5004 placeholder="UDP 端口"></div><button onclick=networkStart('send')>创建发送线路</button></section>
<section class=card><h2>直接从局域网接收</h2><p class=muted>用于未配对设备；配对设备优先在矩阵中创建。</p><label>播放到</label><select id=receiveSink></select><input id=receivePort type=number value=5004 placeholder="UDP 端口"><button onclick=networkStart('receive')>创建接收线路</button></section>
<section class="card wide"><h2>运行日志</h2><pre id=status class=muted>正在加载设备…</pre></section>
<section class="card wide"><h2>设备配对</h2><p class=muted>先在要被配对的机器生成一次性代码；再在本机填入那台机器的 IP、TCP 端口和代码。代码十分钟有效，使用一次即失效。</p><div class=row><div><label>本机别名</label><input id=localAlias></div><div><label>配对控制端口</label><input value=8791 disabled></div></div><label>允许已配对机器看到的本机设备</label><div id=exposure class=expose></div><button onclick=savePairProfile()>保存本机开放设备</button><div class=row><div><label>一次性配对代码</label><code id=pairCode>------</code><button class=secondary onclick=newCode()>生成新代码</button></div><div><label>配对另一台机器</label><input id=peerHost placeholder="192.168.31.100"><div class=row><input id=peerPort type=number value=8791><input id=peerAlias placeholder="别名，例如 客厅电脑"></div><input id=peerCode placeholder="对方显示的六位代码"><button onclick=pairRemote()>开始配对</button></div></div></section>
<section class="card wide"><h2>已配对设备与传输遥测</h2><p class=muted>目录与遥测会在保存开放设备、改别名、开始或停止网络流后同步；此区域也每五秒刷新一次。</p><div id=peers class=muted>尚未配对设备。</div></section>
<section class="card wide"><h2>音频矩阵</h2><p class=muted>勾选本机或已配对设备之间的格子，即可创建本地或 RTP 网络线路；网络属性稍后在路由表中设置。</p><div id=matrix class=matrix></div><button onclick=applyMatrix()>添加勾选的矩阵线路</button></section>
</div></main><script>
let data,peerData=[];const status=x=>document.querySelector('#status').textContent=x,esc=x=>String(x).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function opt(x){return '<option value="'+encodeURIComponent(x.id)+'">'+esc(x.name)+'</option>'}function srcopt(x){return '<option data-loopback="'+x.renderLoopback+'" value="'+encodeURIComponent(x.id)+'">'+esc(x.name)+'</option>'}
async function load(){data=await fetch('/api/devices').then(x=>x.json());let local=await fetch('/api/pair/local').then(x=>x.json());localAlias.value=local.alias;networkSource.innerHTML=data.sources.map(srcopt).join('');receiveSink.innerHTML=data.sinks.map(opt).join('');exposure.innerHTML=data.sources.map(x=>'<label><input type=checkbox data-e="S" value="'+encodeURIComponent(x.id)+'">来源 · '+esc(x.name)+'</label>').join('')+data.sinks.map(x=>'<label><input type=checkbox data-e="K" value="'+encodeURIComponent(x.id)+'">输出 · '+esc(x.name)+'</label>').join('');await loadPeers();await loadRoutes();status('设备已就绪。')}
function renderMatrix(){let rs=peerData.flatMap(p=>p.endpoints.filter(e=>e.direction==='source').map(e=>({id:e.id,name:p.alias+' · '+e.name,remote:true,peer:p.nodeId}))),rk=peerData.flatMap(p=>p.endpoints.filter(e=>e.direction==='sink').map(e=>({id:e.id,name:p.alias+' · '+e.name,remote:true,peer:p.nodeId}))),sources=[...data.sources,...rs],sinks=[...data.sinks,...rk];let h='<table><tr><th>来源 \\ 播放目标</th>'+sinks.map(x=>'<th>'+esc(x.name)+(x.remote?' <small>网络</small>':'')+'</th>').join('')+'</tr>';h+=sources.map(a=>'<tr><th>'+esc(a.name)+(a.remote?' <small>网络</small>':'')+'</th>'+sinks.map(b=>a.remote&&b.remote?'<td class=disabled>跨远端待后续</td>':a.remote?'<td><input type=checkbox data-kind="receive" data-peer="'+a.peer+'" data-remote="'+encodeURIComponent(a.id)+'" data-local="'+encodeURIComponent(b.id)+'"></td>':b.remote?'<td><input type=checkbox data-kind="send" data-peer="'+b.peer+'" data-remote="'+encodeURIComponent(b.id)+'" data-local="'+encodeURIComponent(a.id)+'" data-loopback="'+a.renderLoopback+'"></td>':a.renderLoopback&&a.endpoint===b.endpoint?'<td class=disabled>禁用</td>':'<td><input type=checkbox data-source="'+encodeURIComponent(a.id)+'" data-loopback="'+a.renderLoopback+'" data-sink="'+encodeURIComponent(b.id)+'"></td>').join('')+'</tr>').join('')+'</table>';matrix.innerHTML=h}
async function post(path){const t=await fetch(path,{method:'POST'}).then(x=>x.text());status(t);await loadRoutes();return t}function mirror(){let p=decodeURIComponent(playing.value),t=decodeURIComponent(target.value);if(!p||!t||p===t)return status('请选择不同的播放目标。');post('/api/mirror?source='+encodeURIComponent(p)+'&sink='+encodeURIComponent(t))}
function networkStart(kind){let q='quality=medium&max-latency-ms=100&mode=auto';if(kind==='send'){let s=networkSource.selectedOptions[0];q+='&source='+encodeURIComponent(decodeURIComponent(s.value))+'&loopback='+s.dataset.loopback+'&host='+encodeURIComponent(sendHost.value)+'&port='+sendPort.value}else q+='&sink='+encodeURIComponent(decodeURIComponent(receiveSink.value))+'&port='+receivePort.value;post('/api/network/'+kind+'?'+q)}
async function applyMatrix(){let checked=[...matrix.querySelectorAll('input:checked')],local=checked.filter(x=>x.dataset.source),remote=checked.filter(x=>x.dataset.kind);if(!local.length&&!remote.length)return status('请至少选择一条矩阵线路。');if(local.length)await post('/api/matrix?routes='+encodeURIComponent(local.map(x=>decodeURIComponent(x.dataset.source)+'\\t'+x.dataset.loopback+'\\t'+decodeURIComponent(x.dataset.sink)).join('\\n')));for(let x of remote){let q='peer='+encodeURIComponent(x.dataset.peer)+'&kind='+x.dataset.kind+'&local='+encodeURIComponent(decodeURIComponent(x.dataset.local))+'&remote='+encodeURIComponent(decodeURIComponent(x.dataset.remote))+'&loopback='+(x.dataset.loopback||false)+'&quality=medium&max-latency-ms=100&mode=auto';await post('/api/paired/route?'+q)}}function stopRoute(){post('/api/stop')}
async function savePairProfile(){let e=[...exposure.querySelectorAll('input:checked')].map(x=>x.dataset.e+'\\t'+decodeURIComponent(x.value)+'\\t'+x.parentElement.textContent.trim()).join('\\n');await post('/api/pair/config?alias='+encodeURIComponent(localAlias.value.trim()||'This computer')+'&endpoints='+encodeURIComponent(e));await loadPeers()}async function newCode(){pairCode.textContent=await fetch('/api/pair/code').then(x=>x.text());status('一次性配对代码已生成；十分钟内有效。')}async function pairRemote(){let r=await post('/api/pair/connect?host='+encodeURIComponent(peerHost.value.trim())+'&port='+encodeURIComponent(peerPort.value)+'&alias='+encodeURIComponent(peerAlias.value.trim()||peerHost.value.trim())+'&code='+encodeURIComponent(peerCode.value.trim()));if(r.startsWith('Pairing succeeded'))await loadPeers()}async function editPeer(element){let alias=prompt('远程设备别名',element.dataset.alias);if(!alias)return;let host=prompt('远程 IP / 主机名',element.dataset.host);if(!host)return;let port=prompt('配对控制端口',element.dataset.port);if(!port)return;await post('/api/pair/alias?node='+encodeURIComponent(element.dataset.node)+'&alias='+encodeURIComponent(alias));await post('/api/pair/endpoint?node='+encodeURIComponent(element.dataset.node)+'&host='+encodeURIComponent(host)+'&port='+encodeURIComponent(port));await loadPeers()}async function routeAction(button){await post('/api/routes/'+button.dataset.id+'/'+button.dataset.action+(button.dataset.action==='toggle'?'?enabled='+button.dataset.enabled:''))}async function saveRouteProfile(button){let id=button.dataset.id,q=document.querySelector('#rq'+id).value,l=document.querySelector('#rl'+id).value,m=document.querySelector('#rm'+id).value;await post('/api/routes/'+id+'/profile?quality='+q+'&max-latency-ms='+l+'&mode='+m)}async function loadPeers(){peerData=await fetch('/api/pair/peers').then(x=>x.json());document.querySelector('#peers').innerHTML=peerData.length?peerData.map(p=>{let t=p.telemetry||{},line=t.deviceName?'音质 '+esc(t.quality)+' · 目标延迟 '+t.latencyMs+' ms · 丢包 '+t.packetLossPercent+'% · 设备 '+esc(t.deviceName):'当前未传输音频';return '<div class=peer><span class="editable" data-node="'+esc(p.nodeId)+'" data-alias="'+esc(p.alias)+'" data-host="'+esc(p.host)+'" data-port="'+p.port+'" onclick="editPeer(this)"><b>'+esc(p.alias)+'</b> · '+esc(p.host)+':'+p.port+'</span><br><span class=badge>'+line+'</span><br>'+ (p.endpoints.length?p.endpoints.map(e=>'<span class=badge>'+ (e.direction==='source'?'来源':'输出')+' · '+esc(e.name)+'</span>').join(' '):'<span class=muted>对方未开放设备</span>')+'</div>'}).join(''):'尚未配对设备。';renderMatrix()}async function loadRoutes(){let r=await fetch('/api/routes').then(x=>x.json());routeTable.innerHTML=r.length?r.map(x=>'<div class=peer><b>'+esc(x.label)+'</b> <span class=badge>'+ (x.enabled?'运行中':'已暂停')+'</span>'+ (x.network?'<div class=row><select id=rq'+x.id+'><option '+(x.quality==='low'?'selected':'')+' value=low>低</option><option '+(x.quality==='medium'?'selected':'')+' value=medium>中</option><option '+(x.quality==='high'?'selected':'')+' value=high>高</option></select><select id=rl'+x.id+'><option '+(x.latency===40?'selected':'')+'>40</option><option '+(x.latency===60?'selected':'')+'>60</option><option '+(x.latency===100?'selected':'')+'>100</option><option '+(x.latency===150?'selected':'')+'>150</option></select><select id=rm'+x.id+'><option '+(x.mode==='stable'?'selected':'')+' value=stable>稳定</option><option '+(x.mode==='auto'?'selected':'')+' value=auto>自动</option><option '+(x.mode==='low-latency'?'selected':'')+' value=low-latency>低延迟</option></select><button class=secondary data-id="'+x.id+'" onclick="saveRouteProfile(this)">应用属性</button></div>':'')+'<button class=secondary data-id="'+x.id+'" data-action="toggle" data-enabled="'+(!x.enabled)+'" onclick="routeAction(this)">'+(x.enabled?'暂停':'恢复')+'</button> <button class=secondary data-id="'+x.id+'" data-action="delete" onclick="routeAction(this)">删除</button></div>').join(''):'暂无路由。'}
load().then(()=>setInterval(()=>{loadPeers().catch(()=>{});loadRoutes().catch(()=>{})},5000)).catch(e=>status('Error: '+e));
</script></html>)HTML";

bool parse_udp_port(const std::string& text, std::uint16_t& result) {
    unsigned value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || pointer != text.data() + text.size() || value == 0 || value > 65535) return false;
    result = static_cast<std::uint16_t>(value);
    return true;
}

bool network_profile_from(const std::unordered_map<std::string, std::string>& params,
                          audio::NetworkProfile& profile) {
    const auto quality = params.find("quality"), mode = params.find("mode"), latency = params.find("max-latency-ms");
    if (quality == params.end() || mode == params.end() || latency == params.end()) return false;
    if (quality->second == "low") profile.quality = audio::AudioQuality::Low;
    else if (quality->second == "medium") profile.quality = audio::AudioQuality::Medium;
    else if (quality->second == "high") profile.quality = audio::AudioQuality::High;
    else return false;
    if (mode->second == "stable") profile.mode = audio::LatencyMode::Stable;
    else if (mode->second == "auto") profile.mode = audio::LatencyMode::Auto;
    else if (mode->second == "low-latency") profile.mode = audio::LatencyMode::LowLatency;
    else return false;
    return parse_udp_port(latency->second, profile.max_latency_ms) && audio::valid_max_latency(profile.max_latency_ms);
}

std::string quality_name(audio::AudioQuality quality) {
    switch (quality) {
        case audio::AudioQuality::Low: return "low";
        case audio::AudioQuality::Medium: return "medium";
        case audio::AudioQuality::High: return "high";
    }
    return "unknown";
}

std::string mode_name(audio::LatencyMode mode) {
    switch (mode) {
        case audio::LatencyMode::Stable: return "stable";
        case audio::LatencyMode::Auto: return "auto";
        case audio::LatencyMode::LowLatency: return "low-latency";
    }
    return "unknown";
}

/** Settings of one active route; kept so a route can be rebuilt on resume or profile change. */
using RouteSettings = std::variant<audio::LoopbackSettings, audio::FanoutSettings,
                                   audio::MatrixSettings, audio::SenderSettings,
                                   audio::ReceiverSettings>;

} // namespace
class WebServer::Impl {
public:
    struct ActiveRoute {
        std::size_t id{};
        std::string label;
        bool enabled{true};
        RouteSettings settings;
        std::optional<audio::NetworkProfile> network_profile;
        std::unique_ptr<audio::AudioRoute> route;
    };

    std::atomic_bool stopping{false};
    std::string error;
    std::vector<ActiveRoute> routes;
    std::size_t next_route_id{1};
    pairing::PairingService pairing;
    std::unique_ptr<audio::AudioBackend> backend;
    std::chrono::steady_clock::time_point last_telemetry_sync{};

    Impl() : backend(audio::create_audio_backend()) {}

    std::unique_ptr<audio::AudioRoute> start_route(const RouteSettings& settings) {
        if (!backend) return nullptr;
        return std::visit([this](const auto& value) -> std::unique_ptr<audio::AudioRoute> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, audio::LoopbackSettings>) return backend->create_loopback(value);
            else if constexpr (std::is_same_v<T, audio::FanoutSettings>) return backend->create_fanout(value);
            else if constexpr (std::is_same_v<T, audio::MatrixSettings>) return backend->create_matrix(value);
            else if constexpr (std::is_same_v<T, audio::SenderSettings>) return backend->create_sender(value);
            else return backend->create_receiver(value);
        }, settings);
    }

    bool add_route(std::string label, RouteSettings settings) {
        if (!backend) { error = "No audio backend is available."; return false; }
        auto route = start_route(settings);
        if (!route) { error = backend->last_error(); return false; }
        routes.push_back({next_route_id++, std::move(label), true, std::move(settings), std::nullopt, std::move(route)});
        return true;
    }

    bool add_network_route(std::string label, const audio::NetworkProfile& profile, RouteSettings settings) {
        if (!backend) { error = "No audio backend is available."; return false; }
        auto route = start_route(settings);
        if (!route) { error = backend->last_error(); return false; }
        routes.push_back({next_route_id++, std::move(label), true, std::move(settings), profile, std::move(route)});
        return true;
    }

    bool set_route_enabled(std::size_t id, bool enable) {
        for (auto& item : routes) {
            if (item.id != id) continue;
            if (enable && !item.enabled) {
                auto route = start_route(item.settings);
                if (!route) { error = backend ? backend->last_error() : "No audio backend is available."; return false; }
                item.route = std::move(route);
            } else if (!enable && item.enabled && item.route) {
                item.route->stop();
            }
            item.enabled = enable;
            return true;
        }
        return false;
    }

    bool update_network_route(std::size_t id, const audio::NetworkProfile& profile) {
        for (auto& item : routes) {
            if (item.id != id || !item.network_profile) continue;
            item.network_profile = profile;
            std::visit([&profile](auto& settings) {
                using T = std::decay_t<decltype(settings)>;
                if constexpr (std::is_same_v<T, audio::SenderSettings> || std::is_same_v<T, audio::ReceiverSettings>)
                    settings.network = profile;
            }, item.settings);
            if (!item.enabled) return true;
            item.route.reset();
            auto route = start_route(item.settings);
            if (!route) {
                error = backend ? backend->last_error() : "No audio backend is available.";
                item.enabled = false;
                return false;
            }
            item.route = std::move(route);
            return true;
        }
        return false;
    }

    bool erase_route(std::size_t id) {
        const auto old_size = routes.size();
        routes.erase(std::remove_if(routes.begin(), routes.end(), [id](ActiveRoute& item) {
            if (item.id != id) return false;
            if (item.route) item.route->stop();
            return true;
        }), routes.end());
        return routes.size() != old_size;
    }

    void stop_all_routes() {
        for (auto& item : routes) if (item.route) item.route->stop();
        routes.clear();
    }

    void poll_routes_and_sync_telemetry() {
        for (auto& item : routes) {
            if (item.enabled && item.route && !item.route->poll()) {
                error = item.route->last_error();
                item.route->stop();
                item.enabled = false;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_telemetry_sync < std::chrono::seconds(2)) return;
        last_telemetry_sync = now;
        for (const auto& item : routes) {
            if (!item.enabled || !item.route || !item.network_profile) continue;
            const auto live = item.route->transport_telemetry();
            if (!live) continue;
            pairing::AudioTelemetry telemetry;
            telemetry.quality = quality_name(item.network_profile->quality);
            telemetry.target_latency_ms = item.network_profile->max_latency_ms;
            telemetry.packet_loss_percent = live->packet_loss_percent;
            telemetry.device_name = item.label;
            pairing.set_telemetry(std::move(telemetry));
            pairing.announce();
            return;
        }
    }

    bool start_remote_command(const pairing::RemoteRouteRequest& request, std::string& route_error) {
        audio::NetworkProfile profile;
        std::unordered_map<std::string, std::string> values{{"quality", request.quality},
                                                            {"mode", request.mode},
                                                            {"max-latency-ms", std::to_string(request.max_latency_ms)}};
        if (!network_profile_from(values, profile)) { route_error = "Invalid remote network profile."; return false; }
        if (request.kind == pairing::RemoteRouteKind::Send) {
            audio::SenderSettings settings;
            settings.host = request.host;
            settings.port = request.port;
            settings.source_device = request.device_id;
            settings.capture_render_device = request.render_loopback;
            settings.network = profile;
            if (!add_network_route("Paired send: " + request.device_id + " → " + request.host + ":" + std::to_string(request.port), profile, settings)) { route_error = error; return false; }
        } else {
            audio::ReceiverSettings settings;
            settings.port = request.port;
            settings.sink_device = request.device_id;
            settings.network = profile;
            if (!add_network_route("Paired receive: :" + std::to_string(request.port) + " → " + request.device_id, profile, settings)) { route_error = error; return false; }
        }
        return true;
    }

    std::string devices_json() {
        std::ostringstream json;
        const auto sources = backend ? backend->list_sources() : std::vector<audio::DeviceInfo>{};
        const auto sinks = backend ? backend->list_sinks() : std::vector<audio::DeviceInfo>{};
        json << "{\"sources\":[";
        bool first = true;
        for (const auto& device : sources) {
            if (device.is_default) continue;
            if (!first) json << ','; first = false;
            const bool render_loopback = !device.loopback_of.empty();
            const std::string endpoint = audio::selector_device_id(render_loopback ? device.loopback_of : device.id);
            json << "{\"id\":\"" << json_escape(device.id) << "\",\"name\":\"";
            if (render_loopback) json << "\\u7cfb\\u7edf\\u64ad\\u653e \\u00b7 ";
            json << json_escape(device.name) << "\",\"renderLoopback\":" << (render_loopback ? "true" : "false")
                 << ",\"endpoint\":\"" << json_escape(endpoint) << "\"}";
        }
        json << "],\"renderSources\":[";
        first = true;
        for (const auto& device : sources) {
            if (device.is_default || device.loopback_of.empty()) continue;
            if (!first) json << ','; first = false;
            json << "{\"id\":\"" << json_escape(device.id) << "\",\"name\":\"" << json_escape(device.name) << "\"}";
        }
        json << "],\"sinks\":[";
        first = true;
        for (const auto& device : sinks) {
            if (device.is_default) continue;
            if (!first) json << ','; first = false;
            json << "{\"id\":\"" << json_escape(device.id) << "\",\"name\":\"" << json_escape(device.name)
                 << "\",\"endpoint\":\"" << json_escape(audio::selector_device_id(device.id)) << "\"}";
        }
        return json << "]}", json.str();
    }

    std::string handle(const std::string& method, const std::string& target, std::string& type) {
        type = "text/plain; charset=utf-8";
        if (method == "GET") {
            if (const auto asset = static_asset(target, type)) return *asset;
        }
        if (method == "GET" && target.rfind("/api/devices", 0) == 0) { type = "application/json; charset=utf-8"; return devices_json(); }
        if (method == "GET" && target.rfind("/api/routes", 0) == 0) {
            type = "application/json; charset=utf-8"; std::ostringstream out; out << '['; bool first = true;
            for (const auto& item : routes) { if (!first) out << ','; first = false; out << "{\"id\":" << item.id << ",\"label\":\"" << json_escape(item.label) << "\",\"enabled\":" << (item.enabled ? "true" : "false") << ",\"network\":" << (item.network_profile ? "true" : "false"); if (item.network_profile) out << ",\"quality\":\"" << quality_name(item.network_profile->quality) << "\",\"latency\":" << item.network_profile->max_latency_ms << ",\"mode\":\"" << mode_name(item.network_profile->mode) << "\""; out << "}"; }
            return out << ']', out.str();
        }
        if (method == "GET" && target.rfind("/api/pair/code", 0) == 0) return pairing.create_pair_code();
        if (method == "GET" && target.rfind("/api/pair/local", 0) == 0) { type = "application/json; charset=utf-8"; return "{\"alias\":\"" + json_escape(pairing.local_alias()) + "\"}"; }
        if (method == "GET" && target.rfind("/api/discovery", 0) == 0) {
            type = "application/json; charset=utf-8"; std::ostringstream out;
            out << "{\"enabled\":" << (pairing.discovery_enabled() ? "true" : "false") << ",\"devices\":["; bool first = true;
            for (const auto& device : pairing.discovered_peers()) { if (!first) out << ','; first = false; out << "{\"nodeId\":\"" << json_escape(device.node_id) << "\",\"alias\":\"" << json_escape(device.alias) << "\",\"host\":\"" << json_escape(device.host) << "\",\"port\":" << device.port << '}'; }
            return out << "]}", out.str();
        }
        if (method == "GET" && target.rfind("/api/pair/peers", 0) == 0) {
            type = "application/json; charset=utf-8"; std::ostringstream out; out << '['; bool first = true;
            for (const auto& peer : pairing.peers()) { if (!first) out << ','; first = false; out << "{\"nodeId\":\"" << json_escape(peer.node_id) << "\",\"alias\":\"" << json_escape(peer.alias) << "\",\"host\":\"" << json_escape(peer.host) << "\",\"port\":" << peer.port << ",\"telemetry\":{\"quality\":\"" << json_escape(peer.telemetry.quality) << "\",\"latencyMs\":" << peer.telemetry.target_latency_ms << ",\"packetLossPercent\":" << peer.telemetry.packet_loss_percent << ",\"deviceName\":\"" << json_escape(peer.telemetry.device_name) << "\"},\"endpoints\":["; bool endpoint_first = true; for (const auto& endpoint : peer.endpoints) { if (!endpoint_first) out << ','; endpoint_first = false; out << "{\"id\":\"" << json_escape(endpoint.backend_id) << "\",\"name\":\"" << json_escape(endpoint.name) << "\",\"direction\":\"" << (endpoint.direction == pairing::EndpointDirection::Source ? "source" : "sink") << "\"}"; } out << "]}"; } return out << ']', out.str();
        }
        if (method == "POST" && target.rfind("/api/pair/config", 0) == 0) {
            const auto query = query_params(target); const auto alias = query.find("alias"), endpoints = query.find("endpoints"); if (alias == query.end()) return "Missing local alias.";
            pairing.set_local_alias(alias->second); std::vector<pairing::ExposedEndpoint> exposed;
            if (endpoints != query.end()) {
                // The compact embedded UI serializes separators as JavaScript
                // escapes. Accept those literals as well as real tabs/newlines
                // so exposed-device updates cannot silently become empty.
                std::string endpoint_text = endpoints->second;
                for (std::size_t position = 0; (position = endpoint_text.find("\\n", position)) != std::string::npos; ++position) endpoint_text.replace(position, 2, "\n");
                for (std::size_t position = 0; (position = endpoint_text.find("\\t", position)) != std::string::npos; ++position) endpoint_text.replace(position, 2, "\t");
                std::stringstream rows(endpoint_text); std::string row; while (std::getline(rows, row, '\n')) {
                    const auto first_tab = row.find('\t'); const auto second_tab = row.find('\t', first_tab + 1);
                    if (first_tab == std::string::npos || row.empty()) continue;
                    const auto id = row.substr(first_tab + 1, second_tab == std::string::npos ? std::string::npos : second_tab - first_tab - 1);
                    const auto name = second_tab == std::string::npos ? id : row.substr(second_tab + 1);
                    exposed.push_back({id, name, row[0] == 'S' ? pairing::EndpointDirection::Source : pairing::EndpointDirection::Sink});
                }
            }
            pairing.set_exposed_endpoints(std::move(exposed)); pairing.announce(); return "Pairing profile saved and synchronized.";
        }
        if (method == "POST" && target.rfind("/api/discovery", 0) == 0) {
            const auto query = query_params(target); const auto enabled = query.find("enabled");
            if (enabled == query.end()) return "Missing discovery state.";
            if (enabled->second != "true" && enabled->second != "false") return "Invalid discovery state.";
            if (!pairing.set_discovery_enabled(enabled->second == "true")) return "Could not enable LAN discovery: " + pairing.last_error();
            return enabled->second == "true" ? "LAN discovery enabled." : "LAN discovery disabled.";
        }
        if (method == "POST" && target.rfind("/api/pair/connect", 0) == 0) {
            const auto query = query_params(target); const auto host = query.find("host"), port = query.find("port"), alias = query.find("alias"), code = query.find("code"); std::uint16_t pairing_port{};
            if (host == query.end() || port == query.end() || alias == query.end() || code == query.end() || !parse_udp_port(port->second, pairing_port)) return "Invalid pairing request.";
            if (pairing.pair_remote(host->second, pairing_port, alias->second, code->second)) return "Pairing succeeded.";
            return "Pairing failed: " + pairing.last_error();
        }
        if (method == "POST" && target.rfind("/api/pair/alias", 0) == 0) {
            const auto query = query_params(target); const auto node = query.find("node"), alias = query.find("alias");
            if (node == query.end() || alias == query.end() || !pairing.set_peer_alias(node->second, alias->second)) return "Could not rename paired device.";
            return "Paired device alias saved.";
        }
        if (method == "POST" && target.rfind("/api/pair/endpoint", 0) == 0) {
            const auto query = query_params(target); const auto node = query.find("node"), host = query.find("host"), port = query.find("port"); std::uint16_t pairing_port{};
            if (node == query.end() || host == query.end() || port == query.end() || !parse_udp_port(port->second, pairing_port) || !pairing.set_peer_endpoint(node->second, host->second, pairing_port)) return "Could not update paired address.";
            return "Paired address saved.";
        }
        if (method == "POST" && target.rfind("/api/pair/delete", 0) == 0) {
            const auto query = query_params(target); const auto node = query.find("node");
            if (node == query.end() || !pairing.remove_peer(node->second)) return "Could not remove paired device.";
            return "Paired device removed.";
        }
        if (method == "POST" && target.rfind("/api/paired/route", 0) == 0) {
            const auto query = query_params(target); const auto peer_id = query.find("peer"), kind = query.find("kind"), local_device = query.find("local"), remote_device = query.find("remote"), loopback = query.find("loopback");
            if (peer_id == query.end() || kind == query.end() || local_device == query.end() || remote_device == query.end()) return "Invalid paired matrix route.";
            audio::NetworkProfile profile; if (!network_profile_from(query, profile)) return "Invalid paired route profile.";
            const auto peer_list = pairing.peers(); const auto peer = std::find_if(peer_list.begin(), peer_list.end(), [&](const auto& value) { return value.node_id == peer_id->second; }); if (peer == peer_list.end()) return "Paired device not found.";
            const std::uint16_t media_port = static_cast<std::uint16_t>(52000 + (next_route_id % 1000));
            pairing::RemoteRouteRequest remote; remote.device_id = remote_device->second; remote.port = media_port; remote.quality = quality_name(profile.quality); remote.max_latency_ms = profile.max_latency_ms; remote.mode = mode_name(profile.mode);
            if (kind->second == "send") {
                remote.kind = pairing::RemoteRouteKind::Receive;
                if (!pairing.request_remote_route(peer_id->second, remote)) return "Remote receiver failed: " + pairing.last_error();
                audio::SenderSettings settings; settings.host = peer->host; settings.port = media_port; settings.source_device = local_device->second; settings.capture_render_device = loopback != query.end() && loopback->second == "true"; settings.network = profile;
                if (!add_network_route("Matrix send → " + peer->alias + ": " + remote.device_id, profile, settings)) return "Local sender failed: " + error;
            } else if (kind->second == "receive") {
                // Ask the source computer first. This makes the operation
                // transactional from the UI's perspective: no local table
                // entry is retained when the paired computer rejects it.
                remote.kind = pairing::RemoteRouteKind::Send;
                if (!pairing.request_remote_route(peer_id->second, remote)) return "Remote sender failed: " + pairing.last_error();
                audio::ReceiverSettings settings; settings.port = media_port; settings.sink_device = local_device->second; settings.network = profile;
                if (!add_network_route("Matrix receive → " + peer->alias + ": " + remote.device_id, profile, settings)) return "Local receiver failed: " + error;
            } else return "Invalid paired matrix direction.";
            return "Paired matrix route added.";
        }
        if (method == "POST" && target.rfind("/api/loopback", 0) == 0) {
            const auto params = query_params(target);
            const auto source = params.find("source"), sink = params.find("sink");
            if (source == params.end() || sink == params.end()) return "Missing Source or Sink.";
            audio::LoopbackSettings settings;
            settings.source_device = source->second;
            settings.sink_device = sink->second;
            settings.capture_render_device = true;
            if (add_route("Local: " + source->second + " → " + sink->second, settings)) return "Local route added.";
            return "Start failed: " + error;
        }
        if (method == "POST" && target.rfind("/api/mirror", 0) == 0) {
            const auto params = query_params(target);
            const auto source = params.find("source"), sink = params.find("sink");
            if (source == params.end() || sink == params.end()) return "Missing playback source or target.";
            if (audio::selector_device_id(source->second) == audio::selector_device_id(sink->second)) return "Choose a different playback target to avoid feedback.";
            audio::LoopbackSettings settings;
            settings.source_device = source->second;
            settings.sink_device = sink->second;
            settings.capture_render_device = true;
            if (add_route("Mirror: " + source->second + " → " + sink->second, settings)) return "Playback mirroring route added.";
            return "Start failed: " + error;
        }
        if (method == "POST" && target.rfind("/api/network/send", 0) == 0) {
            const auto params = query_params(target);
            const auto host = params.find("host"), port = params.find("port"), source = params.find("source");
            std::uint16_t udp_port{}; audio::NetworkProfile profile;
            if (host == params.end() || source == params.end() || port == params.end() || !parse_udp_port(port->second, udp_port)
                || !network_profile_from(params, profile)) return "Invalid network sender settings.";
            audio::SenderSettings settings;
            settings.host = host->second; settings.port = udp_port; settings.source_device = source->second;
            settings.capture_render_device = params.contains("loopback") && params.at("loopback") == "true";
            settings.network = profile;
            if (add_network_route("Send: " + source->second + " → " + settings.host + ":" + std::to_string(settings.port), profile, settings)) { pairing.set_telemetry({quality_name(profile.quality), profile.max_latency_ms, -1.0, source->second}); pairing.announce(); return "Network sender route added and telemetry synchronized."; }
            return "Sender failed: " + error;
        }
        if (method == "POST" && target.rfind("/api/network/receive", 0) == 0) {
            const auto params = query_params(target);
            const auto port = params.find("port"), sink = params.find("sink");
            std::uint16_t udp_port{}; audio::NetworkProfile profile;
            if (sink == params.end() || port == params.end() || !parse_udp_port(port->second, udp_port)
                || !network_profile_from(params, profile)) return "Invalid network receiver settings.";
            audio::ReceiverSettings settings;
            settings.port = udp_port; settings.sink_device = sink->second; settings.network = profile;
            if (add_network_route("Receive: :" + std::to_string(settings.port) + " → " + sink->second, profile, settings)) { pairing.set_telemetry({quality_name(profile.quality), profile.max_latency_ms, -1.0, sink->second}); pairing.announce(); return "Network receiver route added and telemetry synchronized."; }
            return "Receiver failed: " + error;
        }
        if (method == "POST" && target.rfind("/api/matrix", 0) == 0) {
            const auto params = query_params(target);
            const auto routes_param = params.find("routes");
            if (routes_param == params.end() || routes_param->second.empty()) return "Select at least one matrix route.";
            audio::MatrixSettings settings;
            std::unordered_map<std::string, std::size_t> sources, sinks;
            // The embedded page serializes separators as JavaScript escape
            // sequences. Normalize them before parsing, while still accepting
            // ordinary literal tabs/newlines from other future clients.
            std::string route_text = routes_param->second;
            for (std::size_t position = 0; (position = route_text.find("\\n", position)) != std::string::npos; ++position)
                route_text.replace(position, 2, "\n");
            for (std::size_t position = 0; (position = route_text.find("\\t", position)) != std::string::npos; ++position)
                route_text.replace(position, 2, "\t");
            std::stringstream stream(route_text); std::string line;
            while (std::getline(stream, line, '\n')) {
                const auto first_separator = line.find('\t');
                const auto second_separator = line.find('\t', first_separator + 1);
                if (first_separator == std::string::npos || second_separator == std::string::npos) continue;
                const std::string source = line.substr(0, first_separator);
                const bool is_render_loopback = line.substr(first_separator + 1, second_separator - first_separator - 1) == "true";
                const std::string sink = line.substr(second_separator + 1);
                if (is_render_loopback && audio::selector_device_id(source) == audio::selector_device_id(sink)) continue;
                const auto [source_it, source_new] = sources.emplace(source, sources.size());
                if (source_new) { settings.source_devices.push_back(source); settings.source_is_render_loopback.push_back(is_render_loopback); }
                const auto [sink_it, sink_new] = sinks.emplace(sink, sinks.size());
                if (sink_new) settings.sink_devices.push_back(sink);
                settings.routes.push_back({source_it->second, sink_it->second});
            }
            if (add_route("Local matrix (" + std::to_string(settings.routes.size()) + " links)", settings)) return "Matrix route added.";
            return "Matrix failed: " + error;
        }
        if (method == "POST" && target.rfind("/api/routes/", 0) == 0) {
            const auto slash = target.find('/', std::string_view{"/api/routes/"}.size()); const auto id_text = target.substr(std::string_view{"/api/routes/"}.size(), slash - std::string_view{"/api/routes/"}.size()); std::size_t id{};
            try { id = std::stoull(id_text); } catch (...) { return "Invalid route id."; }
            if (target.find("/toggle", slash) != std::string::npos) { const auto enabled = query_params(target).contains("enabled") && query_params(target).at("enabled") == "true"; return set_route_enabled(id, enabled) ? "Route updated." : "Could not update route: " + error; }
            if (target.find("/profile", slash) != std::string::npos) { audio::NetworkProfile profile; const auto query = query_params(target); return network_profile_from(query, profile) && update_network_route(id, profile) ? "Route properties updated." : "Could not update route properties: " + error; }
            if (target.find("/delete", slash) != std::string::npos) return erase_route(id) ? "Route deleted." : "Route not found.";
        }
        if (method == "POST" && target.rfind("/api/stop", 0) == 0) { stop_all_routes(); pairing.set_telemetry({}); pairing.announce(); return "All routes stopped and telemetry synchronized."; }
        return "Not found";
    }
};

WebServer::WebServer() : impl_(std::make_unique<Impl>()) {
    impl_->pairing.set_remote_route_handler([this](const pairing::RemoteRouteRequest& request, std::string& error) { return impl_->start_remote_command(request, error); });
}
WebServer::~WebServer() { stop(); }

bool WebServer::serve(std::uint16_t port) {
    if (!impl_->backend) { impl_->error = "No audio backend is available."; return false; }
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) { impl_->error = "Windows sockets could not start."; return false; }
#endif
    if (!impl_->pairing.start(8791)) { impl_->error = "Could not start pairing service: " + impl_->pairing.last_error(); return false; }
    Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) { impl_->error = "Could not create HTTP socket."; return false; }
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0) {
        impl_->error = "Could not bind 127.0.0.1:" + std::to_string(port) + "."; close_socket(listener); impl_->pairing.stop(); return false;
    }
    std::cout << "OAMR Web UI: http://127.0.0.1:" << port << "\nPress Ctrl+C to stop.\n";
    while (!impl_->stopping) {
        fd_set set; FD_ZERO(&set); FD_SET(listener, &set); timeval timeout{0, 200000};
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) > 0) {
            Socket client = accept(listener, nullptr, nullptr); if (client == kInvalidSocket) continue;
            char buffer[8192]{}; const int length = recv(client, buffer, sizeof(buffer) - 1, 0);
            std::string first_line(buffer, length > 0 ? static_cast<std::size_t>(length) : 0);
            const auto end = first_line.find("\r\n"); first_line.resize(end == std::string::npos ? first_line.size() : end);
            std::istringstream request(first_line); std::string method, target; request >> method >> target;
            std::string type; const std::string body = impl_->handle(method, target, type);
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            send(client, response.data(), static_cast<int>(response.size()), 0); close_socket(client);
        }
        impl_->poll_routes_and_sync_telemetry();
    }
    close_socket(listener);
    impl_->pairing.stop();
#ifdef _WIN32
    WSACleanup();
#endif
    return true;
}

void WebServer::stop() noexcept { impl_->stopping = true; impl_->stop_all_routes(); impl_->pairing.stop(); }
const std::string& WebServer::last_error() const noexcept { return impl_->error; }

} // namespace oamr::web
