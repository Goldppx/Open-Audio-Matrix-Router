#include "oamr/web/web_server.hpp"

#include "oamr/gstreamer/device_enumerator.hpp"
#include "oamr/gstreamer/rtp_opus_pipeline.hpp"
#include "oamr/pairing/pairing_service.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string_view>
#include <unordered_map>

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

const char* kPage = R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>OAMR Matrix</title><style>body{font:16px system-ui;margin:2rem;background:#111;color:#eee}table{border-collapse:collapse;width:100%;overflow:auto}th,td{border:1px solid #444;padding:.5rem;text-align:center}th{position:sticky;top:0;background:#222}button{margin:1rem 0;padding:.6rem 1rem;background:#6d5dfc;color:white;border:0;border-radius:6px}#status{white-space:pre-wrap}</style><h1>Open Audio Matrix Router</h1><p>&#26412;&#26426;&#38899;&#39057;&#30697;&#38453;</p><div id="matrix">&#21152;&#36733;&#35774;&#22791;…</div><button onclick="applyMatrix()">&#24212;&#29992;&#30697;&#38453;</button><button onclick="stopRoute()">&#20572;&#27490;</button><p id="status"></p><script>let routes=[];const s=x=>status.textContent=x;async function load(){let d=await fetch("/api/devices").then(x=>x.json()),src=Object.entries(d.sources),sink=Object.entries(d.sinks);let h="<table><tr><th>Source \\ Sink</th>"+sink.map(x=>"<th>"+x[1]+"</th>").join("")+"</tr>";h+=src.map(a=>"<tr><th>"+a[1]+"</th>"+sink.map(b=>"<td><input type=checkbox data-s="+encodeURIComponent(a[0])+" data-k="+encodeURIComponent(b[0])+"></td>").join("")+"</tr>").join("")+"</table>";matrix.innerHTML=h;s("Ready.")}async function applyMatrix(){let r=[...document.querySelectorAll("input:checked")].map(x=>decodeURIComponent(x.dataset.s)+"\t"+decodeURIComponent(x.dataset.k)).join("\n");s(await fetch("/api/matrix?routes="+encodeURIComponent(r),{method:"POST"}).then(x=>x.text()))}async function stopRoute(){s(await fetch("/api/stop",{method:"POST"}).then(x=>x.text()))}load().catch(s);</script></html>)HTML";

// The task-oriented page is intentionally separate from the raw matrix. Users
// choose a playback device, not an implementation detail such as WASAPI loopback.
const char* kTaskPage = R"HTML(<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>OAMR</title><style>:root{color-scheme:dark}body{font:16px system-ui;margin:0;background:#101218;color:#eef1f7}.wrap{max-width:1080px;margin:auto;padding:32px 22px}.card{background:#191d27;border:1px solid #303746;border-radius:12px;padding:20px;margin:20px 0}.muted{color:#aab3c3}label{display:block;margin:12px 0 5px}select,button{font:inherit;padding:10px;border-radius:7px;border:1px solid #465066;background:#10131a;color:#eef1f7;width:100%;box-sizing:border-box}button{border:0;background:#6d5dfc;font-weight:700;cursor:pointer;margin-top:14px}.secondary{background:#303746}.matrix{overflow:auto}table{border-collapse:collapse;min-width:720px;width:100%}th,td{border:1px solid #3b4352;padding:9px;text-align:center}th{background:#222836}td.disabled{background:#151821;color:#697386}input{width:20px;height:20px}#status{white-space:pre-wrap;margin:12px 0 0}summary{cursor:pointer;font-weight:700}</style><main class=wrap><h1>Open Audio Matrix Router</h1><p class=muted>&#26412;&#26426;&#38899;&#39057;&#36335;&#30001;&#65306;&#20808;&#36873;&#25321;&#24819;&#23436;&#25104;&#30340;&#20107;&#12290;</p><section class=card><h2>&#21516;&#27493;&#27491;&#22312;&#25773;&#25918;&#30340;&#22768;&#38899;</h2><p class=muted>&#21407;&#26377;&#30340;&#25773;&#25918;&#19981;&#20250;&#34987;&#20013;&#26029;&#12290;</p><label>&#27491;&#22312;&#25773;&#25918;&#21040;</label><select id=playing></select><label>&#21516;&#27493;&#25773;&#25918;&#21040;</label><select id=target></select><button onclick=mirror()>&#21551;&#21160;&#21516;&#27493;&#25773;&#25918;</button></section><details class=card><summary>&#39640;&#32423;&#30697;&#38453;</summary><p class=muted>&#8220;&#31995;&#32479;&#25773;&#25918; &#183; &#8221;&#26159;&#36755;&#20986;&#35774;&#22791;&#30340;&#22238;&#37319;&#12290;&#21516;&#19968;&#35774;&#22791;&#30340;&#22238;&#37319;&#19981;&#21487;&#20889;&#22238;&#35813;&#35774;&#22791;&#65292;&#20197;&#38450;&#27490;&#22238;&#25480;&#12290;</p><div id=matrix class=matrix></div><button onclick=applyMatrix()>&#24212;&#29992;&#39640;&#32423;&#30697;&#38453;</button></details><button class=secondary onclick=stopRoute()>&#20572;&#27490;&#24403;&#21069;&#36335;&#30001;</button><p id=status class=muted>&#21152;&#36733;&#35774;&#22791;&#20013;&#8230;</p></main><script>let data;const status=x=>document.querySelector('#status').textContent=x,esc=x=>x.replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));function opt(x){return '<option value="'+encodeURIComponent(x.id)+'">'+esc(x.name)+'</option>'}async function load(){data=await fetch('/api/devices').then(x=>x.json());playing.innerHTML=data.renderSources.map(opt).join('');target.innerHTML=data.sinks.map(opt).join('');renderMatrix();status('&#35774;&#22791;&#24050;&#23601;&#32490;&#12290;')}function renderMatrix(){let h='<table><tr><th>&#26469;&#28304; \\ &#25773;&#25918;&#30446;&#26631;</th>'+data.sinks.map(x=>'<th>'+esc(x.name)+'</th>').join('')+'</tr>';h+=data.sources.map(a=>'<tr><th>'+esc(a.name)+'</th>'+data.sinks.map(b=>a.renderLoopback&&a.endpoint===b.endpoint?'<td class=disabled>&#31105;&#29992;</td>':'<td><input type=checkbox data-source="'+encodeURIComponent(a.id)+'" data-loopback="'+a.renderLoopback+'" data-sink="'+encodeURIComponent(b.id)+'"></td>').join('')+'</tr>').join('')+'</table>';matrix.innerHTML=h}async function mirror(){let p=decodeURIComponent(playing.value),t=decodeURIComponent(target.value);if(!p||!t||p===t)return status('&#35831;&#36873;&#25321;&#19981;&#21516;&#30340;&#25773;&#25918;&#30446;&#26631;&#12290;');status(await fetch('/api/mirror?source='+encodeURIComponent(p)+'&sink='+encodeURIComponent(t),{method:'POST'}).then(x=>x.text()))}async function applyMatrix(){let r=[...document.querySelectorAll('input:checked')].map(x=>decodeURIComponent(x.dataset.source)+'\\t'+x.dataset.loopback+'\\t'+decodeURIComponent(x.dataset.sink)).join('\\n');if(!r)return status('&#35831;&#33267;&#23569;&#36873;&#25321;&#19968;&#26465;&#36335;&#30001;&#12290;');status(await fetch('/api/matrix?routes='+encodeURIComponent(r),{method:'POST'}).then(x=>x.text()))}async function stopRoute(){status(await fetch('/api/stop',{method:'POST'}).then(x=>x.text()))}load().catch(e=>status('Error: '+e));</script></html>)HTML";

const char* kControlPage = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>OAMR</title>
<style>:root{color-scheme:dark}*{box-sizing:border-box}body{font:15px system-ui;margin:0;background:#101218;color:#eef1f7}.wrap{max-width:1240px;margin:auto;padding:30px 22px}.layout{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px}.wide{grid-column:1/-1}.card{background:#191d27;border:1px solid #303746;border-radius:12px;padding:20px}.muted{color:#aab3c3;margin:.4em 0 1em}h1,h2{margin:0 0 8px}h2{font-size:1.18rem}label{display:block;margin:10px 0 4px}input,select,button{font:inherit;padding:10px;border-radius:7px;border:1px solid #465066;background:#10131a;color:#eef1f7;width:100%}button{border:0;background:#6d5dfc;font-weight:700;cursor:pointer;margin-top:14px}.secondary{background:#303746}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.matrix{overflow:auto}table{border-collapse:collapse;min-width:720px;width:100%}th,td{border:1px solid #3b4352;padding:9px;text-align:center}th{background:#222836}td.disabled{background:#151821;color:#697386}input[type=checkbox]{width:20px;height:20px}#status{white-space:pre-wrap;min-height:42px}.badge{display:inline-block;background:#29314a;border-radius:99px;padding:4px 9px;font-size:.82em}@media(max-width:760px){.layout{grid-template-columns:1fr}.wide{grid-column:auto}}</style>
<main class=wrap><h1>Open Audio Matrix Router</h1><p class=muted><span class=badge>&#26412;&#26426;&#25511;&#21046;</span> &#26412;&#22320;&#36335;&#30001;&#19982;&#20869;&#32593; RTP/Opus &#20256;&#36755;</p>
<div class=layout>
<section class=card><h2>&#21516;&#27493;&#27491;&#22312;&#25773;&#25918;&#30340;&#22768;&#38899;</h2><p class=muted>&#20363;&#65306;&#32819;&#26426;&#27491;&#22312;&#25773;&#25918;&#30340;&#22768;&#38899;&#21516;&#26102;&#36865;&#21040;&#25196;&#22768;&#22120;&#12290;</p><label>&#27491;&#22312;&#25773;&#25918;&#21040;</label><select id=playing></select><label>&#21516;&#27493;&#25773;&#25918;&#21040;</label><select id=target></select><button onclick=mirror()>&#21551;&#21160;&#21516;&#27493;&#25773;&#25918;</button></section>
<section class=card><h2>&#21457;&#36865;&#21040;&#20869;&#32593;&#35774;&#22791;</h2><p class=muted>&#23558;&#26412;&#26426;&#30340;&#36755;&#20837;&#25110;&#31995;&#32479;&#25773;&#25918;&#21457;&#36865;&#21040;&#25351;&#23450; IP &#21644;&#31471;&#21475;&#12290;</p><label>&#38899;&#39057;&#26469;&#28304;</label><select id=networkSource></select><div class=row><div><label>IP / &#20027;&#26426;&#21517;</label><input id=sendHost value=127.0.0.1></div><div><label>UDP &#31471;&#21475;</label><input id=sendPort type=number value=5004 min=1 max=65535></div></div><div class=row><div><label>&#38899;&#36136;</label><select id=sendQuality><option value=low>&#20302;</option><option value=medium selected>&#20013;</option><option value=high>&#39640;</option></select></div><div><label>&#26368;&#22823;&#24310;&#36831;</label><select id=sendLatency><option>40</option><option>60</option><option selected>100</option><option>150</option></select></div></div><label>&#27169;&#24335;</label><select id=sendMode><option value=stable>&#31283;&#23450;</option><option value=auto selected>&#33258;&#21160;</option><option value=low-latency>&#20302;&#24310;&#36831;</option></select><button onclick=networkStart('send')>&#21551;&#21160;&#21457;&#36865;</button></section>
<section class=card><h2>&#20174;&#20869;&#32593;&#35774;&#22791;&#25509;&#25910;</h2><p class=muted>&#22312;&#26412;&#26426; UDP &#31471;&#21475;&#25509;&#25910; RTP/Opus&#65292;&#24182;&#25773;&#25918;&#21040;&#36873;&#23450;&#35774;&#22791;&#12290;</p><label>&#25773;&#25918;&#21040;</label><select id=receiveSink></select><div class=row><div><label>UDP &#31471;&#21475;</label><input id=receivePort type=number value=5004 min=1 max=65535></div><div><label>&#38899;&#36136;</label><select id=receiveQuality><option value=low>&#20302;</option><option value=medium selected>&#20013;</option><option value=high>&#39640;</option></select></div></div><div class=row><div><label>&#26368;&#22823;&#24310;&#36831;</label><select id=receiveLatency><option>40</option><option>60</option><option selected>100</option><option>150</option></select></div><div><label>&#27169;&#24335;</label><select id=receiveMode><option value=stable>&#31283;&#23450;</option><option value=auto selected>&#33258;&#21160;</option><option value=low-latency>&#20302;&#24310;&#36831;</option></select></div></div><button onclick=networkStart('receive')>&#21551;&#21160;&#25509;&#25910;</button></section>
<section class=card><h2>&#24403;&#21069;&#36335;&#30001;</h2><p class=muted>&#19968;&#20010; MVP &#36827;&#31243;&#24403;&#21069;&#36816;&#34892;&#19968;&#26465;&#27963;&#21160;&#36335;&#30001;&#12290;&#26032;&#21551;&#21160;&#20250;&#26367;&#25442;&#26087;&#36335;&#30001;&#12290;</p><p id=status>Ready.</p><button class=secondary onclick=stopRoute()>&#20572;&#27490;&#24403;&#21069;&#36335;&#30001;</button></section>
<details class="card wide"><summary><b>&#39640;&#32423;&#30697;&#38453;</b></summary><p class=muted>&#8220;&#31995;&#32479;&#25773;&#25918; &#183; &#8221;&#26159;&#36755;&#20986;&#35774;&#22791;&#30340;&#22238;&#37319;&#12290;&#21516;&#19968;&#35774;&#22791;&#30340;&#22238;&#37319;&#19981;&#21487;&#20889;&#22238;&#35813;&#35774;&#22791;&#65292;&#20197;&#38450;&#27490;&#22238;&#25480;&#12290;</p><div id=matrix class=matrix></div><button onclick=applyMatrix()>&#24212;&#29992;&#39640;&#32423;&#30697;&#38453;</button></details>
</div></main><script>
let data;const status=x=>document.querySelector('#status').textContent=x,esc=x=>x.replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c]));
function opt(x){return '<option value="'+encodeURIComponent(x.id)+'">'+esc(x.name)+'</option>'}function srcopt(x){return '<option data-loopback="'+x.renderLoopback+'" value="'+encodeURIComponent(x.id)+'">'+esc(x.name)+'</option>'}
async function load(){data=await fetch('/api/devices').then(x=>x.json());playing.innerHTML=data.renderSources.map(opt).join('');target.innerHTML=data.sinks.map(opt).join('');networkSource.innerHTML=data.sources.map(srcopt).join('');receiveSink.innerHTML=data.sinks.map(opt).join('');renderMatrix();status('Ready.')}
function renderMatrix(){let h='<table><tr><th>&#26469;&#28304; \\ &#25773;&#25918;&#30446;&#26631;</th>'+data.sinks.map(x=>'<th>'+esc(x.name)+'</th>').join('')+'</tr>';h+=data.sources.map(a=>'<tr><th>'+esc(a.name)+'</th>'+data.sinks.map(b=>a.renderLoopback&&a.endpoint===b.endpoint?'<td class=disabled>&#31105;&#29992;</td>':'<td><input type=checkbox data-source="'+encodeURIComponent(a.id)+'" data-loopback="'+a.renderLoopback+'" data-sink="'+encodeURIComponent(b.id)+'"></td>').join('')+'</tr>').join('')+'</table>';matrix.innerHTML=h}
async function post(path){status(await fetch(path,{method:'POST'}).then(x=>x.text()))}function mirror(){let p=decodeURIComponent(playing.value),t=decodeURIComponent(target.value);if(!p||!t||p===t)return status('Choose a different target.');post('/api/mirror?source='+encodeURIComponent(p)+'&sink='+encodeURIComponent(t))}
function networkStart(kind){let pre=kind==='send'?'send':'receive',q='quality='+encodeURIComponent(document.querySelector('#'+pre+'Quality').value)+'&max-latency-ms='+document.querySelector('#'+pre+'Latency').value+'&mode='+encodeURIComponent(document.querySelector('#'+pre+'Mode').value);if(kind==='send'){let s=networkSource.selectedOptions[0];q+='&source='+encodeURIComponent(decodeURIComponent(s.value))+'&loopback='+s.dataset.loopback+'&host='+encodeURIComponent(sendHost.value)+'&port='+sendPort.value}else q+='&sink='+encodeURIComponent(decodeURIComponent(receiveSink.value))+'&port='+receivePort.value;post('/api/network/'+kind+'?'+q)}
function applyMatrix(){let r=[...document.querySelectorAll('input[type=checkbox]:checked')].map(x=>decodeURIComponent(x.dataset.source)+'\\t'+x.dataset.loopback+'\\t'+decodeURIComponent(x.dataset.sink)).join('\\n');if(!r)return status('Select at least one route.');post('/api/matrix?routes='+encodeURIComponent(r))}function stopRoute(){post('/api/stop')}load().catch(e=>status('Error: '+e));
</script></html>)HTML";

// Pairing deliberately lives in the loopback-only UI.  The only LAN listener
// is the small, code-gated pairing service on TCP 8791.
const char* kPairControlPage = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>OAMR</title>
<style>:root{color-scheme:dark}*{box-sizing:border-box}body{font:15px system-ui;margin:0;background:#101218;color:#eef1f7}.wrap{max-width:1260px;margin:auto;padding:30px 22px}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px}.wide{grid-column:1/-1}.card{background:#191d27;border:1px solid #303746;border-radius:12px;padding:20px}.muted{color:#aab3c3;margin:.4em 0 1em}h1,h2{margin:0 0 8px}h2{font-size:1.18rem}label{display:block;margin:10px 0 4px}input,select,button{font:inherit;padding:10px;border-radius:7px;border:1px solid #465066;background:#10131a;color:#eef1f7;width:100%}button{border:0;background:#6d5dfc;font-weight:700;cursor:pointer;margin-top:14px}.secondary{background:#303746}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.matrix{overflow:auto}table{border-collapse:collapse;min-width:720px;width:100%}th,td{border:1px solid #3b4352;padding:9px;text-align:center}th{background:#222836}td.disabled{background:#151821;color:#697386}input[type=checkbox]{width:20px;height:20px}.expose{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px;max-height:210px;overflow:auto}.expose label{margin:0;padding:8px;background:#131720;border-radius:6px}.expose input{width:auto;margin-right:7px}code{font-size:1.35rem;letter-spacing:.18em;background:#10131a;padding:7px 10px;border-radius:6px}#status{white-space:pre-wrap;min-height:42px}.peer{border-top:1px solid #303746;padding:10px 0}.badge{display:inline-block;background:#29314a;border-radius:99px;padding:4px 9px;font-size:.82em}@media(max-width:760px){.grid,.row,.expose{grid-template-columns:1fr}.wide{grid-column:auto}}</style>
<main class=wrap><h1>Open Audio Matrix Router</h1><p class=muted><span class=badge>仅本机 Web UI</span> 配对控制端口为 TCP 8791；网页不会暴露到局域网。</p><div class=grid>
<section class=card><h2>同步正在播放的声音</h2><p class=muted>把一个耳机或扬声器的系统播放同步到另一个输出。</p><label>正在播放到</label><select id=playing></select><label>同步播放到</label><select id=target></select><button onclick=mirror()>启动同步播放</button></section>
<section class=card><h2>发送到局域网</h2><p class=muted>向目标机器的 UDP 音频端口发送 RTP/Opus。</p><label>音频来源</label><select id=networkSource></select><div class=row><div><label>IP / 主机名</label><input id=sendHost value=127.0.0.1></div><div><label>UDP 端口</label><input id=sendPort type=number value=5004></div></div><div class=row><div><label>音质</label><select id=sendQuality><option value=low>低</option><option value=medium selected>中</option><option value=high>高</option></select></div><div><label>最大延迟</label><select id=sendLatency><option>40</option><option>60</option><option selected>100</option><option>150</option></select></div></div><label>模式</label><select id=sendMode><option value=stable>稳定</option><option value=auto selected>自动</option><option value=low-latency>低延迟</option></select><button onclick=networkStart('send')>启动发送</button></section>
<section class=card><h2>从局域网接收</h2><p class=muted>在本机 UDP 端口接收 RTP/Opus 并播放。</p><label>播放到</label><select id=receiveSink></select><div class=row><div><label>UDP 端口</label><input id=receivePort type=number value=5004></div><div><label>音质</label><select id=receiveQuality><option value=low>低</option><option value=medium selected>中</option><option value=high>高</option></select></div></div><div class=row><div><label>最大延迟</label><select id=receiveLatency><option>40</option><option>60</option><option selected>100</option><option>150</option></select></div><div><label>模式</label><select id=receiveMode><option value=stable>稳定</option><option value=auto selected>自动</option><option value=low-latency>低延迟</option></select></div></div><button onclick=networkStart('receive')>启动接收</button></section>
<section class=card><h2>当前路由</h2><p class=muted>当前 MVP 同时运行一条活动音频路由。配对信息与设备开放列表不会受停止影响。</p><p id=status>正在加载设备…</p><button class=secondary onclick=stopRoute()>停止当前音频路由</button></section>
<section class="card wide"><h2>设备配对</h2><p class=muted>先在要被配对的机器生成一次性代码；再在本机填入那台机器的 IP、TCP 端口和代码。代码十分钟有效，使用一次即失效。</p><div class=row><div><label>本机别名</label><input id=localAlias value="This computer"></div><div><label>配对控制端口</label><input value=8791 disabled></div></div><label>允许已配对机器看到的本机设备</label><div id=exposure class=expose></div><button onclick=savePairProfile()>保存本机开放设备</button><div class=row><div><label>一次性配对代码</label><code id=pairCode>------</code><button class=secondary onclick=newCode()>生成新代码</button></div><div><label>配对另一台机器</label><input id=peerHost placeholder="192.168.31.100"><div class=row><input id=peerPort type=number value=8791><input id=peerAlias placeholder="别名，例如 客厅电脑"></div><input id=peerCode placeholder="对方显示的六位代码"><button onclick=pairRemote()>开始配对</button></div></div></section>
<section class="card wide"><h2>已配对设备与传输遥测</h2><p class=muted>目录与遥测会在保存开放设备、改别名、开始或停止网络流后同步；此区域也每五秒刷新一次。</p><div id=peers class=muted>尚未配对设备。</div></section>
<details class="card wide"><summary><b>高级本地矩阵</b></summary><p class=muted>系统播放是输出设备的回采；相同设备的回采不可写回自身以防回授。已配对的远端设备显示在上方目录，网络路由通过 RTP 卡片启动。</p><div id=matrix class=matrix></div><button onclick=applyMatrix()>应用本地矩阵</button></details>
</div></main><script>
let data;const status=x=>document.querySelector('#status').textContent=x,esc=x=>String(x).replace(/[&<>\"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function opt(x){return '<option value="'+encodeURIComponent(x.id)+'">'+esc(x.name)+'</option>'}function srcopt(x){return '<option data-loopback="'+x.renderLoopback+'" value="'+encodeURIComponent(x.id)+'">'+esc(x.name)+'</option>'}
async function load(){data=await fetch('/api/devices').then(x=>x.json());playing.innerHTML=data.renderSources.map(opt).join('');target.innerHTML=data.sinks.map(opt).join('');networkSource.innerHTML=data.sources.map(srcopt).join('');receiveSink.innerHTML=data.sinks.map(opt).join('');exposure.innerHTML=data.sources.map(x=>'<label><input type=checkbox data-e="S" value="'+encodeURIComponent(x.id)+'">来源 · '+esc(x.name)+'</label>').join('')+data.sinks.map(x=>'<label><input type=checkbox data-e="K" value="'+encodeURIComponent(x.id)+'">输出 · '+esc(x.name)+'</label>').join('');renderMatrix();await loadPeers();status('设备已就绪。')}
function renderMatrix(){let h='<table><tr><th>来源 \\ 播放目标</th>'+data.sinks.map(x=>'<th>'+esc(x.name)+'</th>').join('')+'</tr>';h+=data.sources.map(a=>'<tr><th>'+esc(a.name)+'</th>'+data.sinks.map(b=>a.renderLoopback&&a.endpoint===b.endpoint?'<td class=disabled>禁用</td>':'<td><input type=checkbox data-source="'+encodeURIComponent(a.id)+'" data-loopback="'+a.renderLoopback+'" data-sink="'+encodeURIComponent(b.id)+'"></td>').join('')+'</tr>').join('')+'</table>';matrix.innerHTML=h}
async function post(path){const t=await fetch(path,{method:'POST'}).then(x=>x.text());status(t);return t}function mirror(){let p=decodeURIComponent(playing.value),t=decodeURIComponent(target.value);if(!p||!t||p===t)return status('请选择不同的播放目标。');post('/api/mirror?source='+encodeURIComponent(p)+'&sink='+encodeURIComponent(t))}
function networkStart(kind){let pre=kind==='send'?'send':'receive',q='quality='+encodeURIComponent(document.querySelector('#'+pre+'Quality').value)+'&max-latency-ms='+document.querySelector('#'+pre+'Latency').value+'&mode='+encodeURIComponent(document.querySelector('#'+pre+'Mode').value);if(kind==='send'){let s=networkSource.selectedOptions[0];q+='&source='+encodeURIComponent(decodeURIComponent(s.value))+'&loopback='+s.dataset.loopback+'&host='+encodeURIComponent(sendHost.value)+'&port='+sendPort.value}else q+='&sink='+encodeURIComponent(decodeURIComponent(receiveSink.value))+'&port='+receivePort.value;post('/api/network/'+kind+'?'+q)}
function applyMatrix(){let r=[...matrix.querySelectorAll('input:checked')].map(x=>decodeURIComponent(x.dataset.source)+'\\t'+x.dataset.loopback+'\\t'+decodeURIComponent(x.dataset.sink)).join('\\n');if(!r)return status('请至少选择一条本地路由。');post('/api/matrix?routes='+encodeURIComponent(r))}function stopRoute(){post('/api/stop')}
async function savePairProfile(){let e=[...exposure.querySelectorAll('input:checked')].map(x=>x.dataset.e+'\\t'+decodeURIComponent(x.value)+'\\t'+x.parentElement.textContent.trim()).join('\\n');await post('/api/pair/config?alias='+encodeURIComponent(localAlias.value.trim()||'This computer')+'&endpoints='+encodeURIComponent(e));await loadPeers()}async function newCode(){pairCode.textContent=await fetch('/api/pair/code').then(x=>x.text());status('一次性配对代码已生成；十分钟内有效。')}async function pairRemote(){let r=await post('/api/pair/connect?host='+encodeURIComponent(peerHost.value.trim())+'&port='+encodeURIComponent(peerPort.value)+'&alias='+encodeURIComponent(peerAlias.value.trim()||peerHost.value.trim())+'&code='+encodeURIComponent(peerCode.value.trim()));if(r.startsWith('Pairing succeeded'))await loadPeers()}async function loadPeers(){let peerData=await fetch('/api/pair/peers').then(x=>x.json());document.querySelector('#peers').innerHTML=peerData.length?peerData.map(p=>{let t=p.telemetry||{},line=t.deviceName?'音质 '+esc(t.quality)+' · 目标延迟 '+t.latencyMs+' ms · 丢包 '+t.packetLossPercent+'% · 设备 '+esc(t.deviceName):'当前未传输音频';return '<div class=peer><b>'+esc(p.alias)+'</b> <span class=muted>'+esc(p.host)+':'+p.port+'</span><br><span class=badge>'+line+'</span><br>'+ (p.endpoints.length?p.endpoints.map(e=>'<span class=badge>'+ (e.direction==='source'?'来源':'输出')+' · '+esc(e.name)+'</span>').join(' '):'<span class=muted>对方未开放设备</span>')+'</div>'}).join(''):'尚未配对设备。'}
load().then(()=>setInterval(()=>loadPeers().catch(()=>{}),5000)).catch(e=>status('Error: '+e));
</script></html>)HTML";

bool is_default_device(const std::string& name) { return name.rfind("Default Audio ", 0) == 0; }
std::string source_selector_for_render_device(const std::string& sink_selector) {
    constexpr std::string_view prefix{"wasapi2sink|"};
    return sink_selector.rfind(prefix, 0) == 0 ? "wasapi2src|" + sink_selector.substr(prefix.size()) : std::string{};
}
std::string endpoint_id(const std::string& selector) {
    const auto delimiter = selector.find('|');
    return delimiter == std::string::npos ? selector : selector.substr(delimiter + 1);
}

bool parse_udp_port(const std::string& text, std::uint16_t& result) {
    unsigned value{};
    const auto [pointer, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || pointer != text.data() + text.size() || value == 0 || value > 65535) return false;
    result = static_cast<std::uint16_t>(value);
    return true;
}

bool network_profile_from(const std::unordered_map<std::string, std::string>& params,
                          gstreamer::NetworkAudioProfile& profile) {
    const auto quality = params.find("quality"), mode = params.find("mode"), latency = params.find("max-latency-ms");
    if (quality == params.end() || mode == params.end() || latency == params.end()) return false;
    if (quality->second == "low") profile.quality = gstreamer::AudioQuality::Low;
    else if (quality->second == "medium") profile.quality = gstreamer::AudioQuality::Medium;
    else if (quality->second == "high") profile.quality = gstreamer::AudioQuality::High;
    else return false;
    if (mode->second == "stable") profile.mode = gstreamer::LatencyMode::Stable;
    else if (mode->second == "auto") profile.mode = gstreamer::LatencyMode::Auto;
    else if (mode->second == "low-latency") profile.mode = gstreamer::LatencyMode::LowLatency;
    else return false;
    return parse_udp_port(latency->second, profile.max_latency_ms) && gstreamer::valid_max_latency(profile.max_latency_ms);
}

std::string quality_name(gstreamer::AudioQuality quality) {
    switch (quality) { case gstreamer::AudioQuality::Low: return "low"; case gstreamer::AudioQuality::Medium: return "medium"; case gstreamer::AudioQuality::High: return "high"; }
    return "unknown";
}

} // namespace

class WebServer::Impl {
public:
    std::atomic_bool stopping{false};
    std::string error;
    gstreamer::RtpOpusPipeline route;
    pairing::PairingService pairing;

    std::string devices_json() {
        gstreamer::DeviceEnumerator enumerator;
        std::ostringstream json;
        const auto captures = enumerator.list_capture_devices();
        const auto sinks = enumerator.list_playback_devices();
        std::unordered_map<std::string, bool> playback_endpoints;
        for (const auto& device : sinks) playback_endpoints.emplace(endpoint_id(device.backend_id), true);
        json << "{\"sources\":[";
        bool first = true;
        for (const auto& device : captures) {
            // GstDeviceMonitor reports WASAPI render endpoints under both
            // source and sink classes. Present those exactly once as the
            // explicit render-loopback sources below.
            if (is_default_device(device.name) || playback_endpoints.contains(endpoint_id(device.backend_id))) continue;
            if (!first) json << ','; first = false;
            json << "{\"id\":\"" << json_escape(device.backend_id) << "\",\"name\":\"" << json_escape(device.name)
                 << "\",\"renderLoopback\":false,\"endpoint\":\"" << json_escape(endpoint_id(device.backend_id)) << "\"}";
        }
        for (const auto& device : sinks) {
            if (is_default_device(device.name)) continue;
            const std::string source_selector = source_selector_for_render_device(device.backend_id);
            if (source_selector.empty()) continue;
            if (!first) json << ','; first = false;
            json << "{\"id\":\"" << json_escape(source_selector) << "\",\"name\":\"\\u7cfb\\u7edf\\u64ad\\u653e \\u00b7 " << json_escape(device.name)
                 << "\",\"renderLoopback\":true,\"endpoint\":\"" << json_escape(endpoint_id(device.backend_id)) << "\"}";
        }
        json << "],\"renderSources\":["; first = true;
        for (const auto& device : sinks) {
            if (is_default_device(device.name)) continue;
            const std::string source_selector = source_selector_for_render_device(device.backend_id);
            if (source_selector.empty()) continue;
            if (!first) json << ','; first = false;
            json << "{\"id\":\"" << json_escape(source_selector) << "\",\"name\":\"" << json_escape(device.name) << "\"}";
        }
        json << "],\"sinks\":["; first = true;
        for (const auto& device : sinks) {
            if (is_default_device(device.name)) continue;
            if (!first) json << ','; first = false;
            json << "{\"id\":\"" << json_escape(device.backend_id) << "\",\"name\":\"" << json_escape(device.name)
                 << "\",\"endpoint\":\"" << json_escape(endpoint_id(device.backend_id)) << "\"}";
        }
        return json << "]}", json.str();
    }

    std::string handle(const std::string& method, const std::string& target, std::string& type) {
        type = "text/plain; charset=utf-8";
        if (method == "GET" && target.rfind("/api/devices", 0) == 0) { type = "application/json; charset=utf-8"; return devices_json(); }
        if (method == "GET" && target.rfind("/api/pair/code", 0) == 0) return pairing.create_pair_code();
        if (method == "GET" && target.rfind("/api/pair/peers", 0) == 0) {
            type = "application/json; charset=utf-8"; std::ostringstream out; out << '['; bool first = true;
            for (const auto& peer : pairing.peers()) { if (!first) out << ','; first = false; out << "{\"alias\":\"" << json_escape(peer.alias) << "\",\"host\":\"" << json_escape(peer.host) << "\",\"port\":" << peer.port << ",\"telemetry\":{\"quality\":\"" << json_escape(peer.telemetry.quality) << "\",\"latencyMs\":" << peer.telemetry.target_latency_ms << ",\"packetLossPercent\":" << peer.telemetry.packet_loss_percent << ",\"deviceName\":\"" << json_escape(peer.telemetry.device_name) << "\"},\"endpoints\":["; bool endpoint_first = true; for (const auto& endpoint : peer.endpoints) { if (!endpoint_first) out << ','; endpoint_first = false; out << "{\"name\":\"" << json_escape(endpoint.name) << "\",\"direction\":\"" << (endpoint.direction == pairing::EndpointDirection::Source ? "source" : "sink") << "\"}"; } out << "]}"; } return out << ']', out.str();
        }
        if (method == "POST" && target.rfind("/api/pair/config", 0) == 0) {
            const auto query = query_params(target); const auto alias = query.find("alias"), endpoints = query.find("endpoints"); if (alias == query.end()) return "Missing local alias.";
            pairing.set_local_alias(alias->second); std::vector<pairing::ExposedEndpoint> exposed;
            if (endpoints != query.end()) { std::stringstream rows(endpoints->second); std::string row; while (std::getline(rows, row, '\n')) {
                const auto first_tab = row.find('\t'); const auto second_tab = row.find('\t', first_tab + 1);
                if (first_tab == std::string::npos || row.empty()) continue;
                const auto id = row.substr(first_tab + 1, second_tab == std::string::npos ? std::string::npos : second_tab - first_tab - 1);
                const auto name = second_tab == std::string::npos ? id : row.substr(second_tab + 1);
                exposed.push_back({id, name, row[0] == 'S' ? pairing::EndpointDirection::Source : pairing::EndpointDirection::Sink});
            } }
            pairing.set_exposed_endpoints(std::move(exposed)); pairing.announce(); return "Pairing profile saved and synchronized.";
        }
        if (method == "POST" && target.rfind("/api/pair/connect", 0) == 0) {
            const auto query = query_params(target); const auto host = query.find("host"), port = query.find("port"), alias = query.find("alias"), code = query.find("code"); std::uint16_t pairing_port{};
            if (host == query.end() || port == query.end() || alias == query.end() || code == query.end() || !parse_udp_port(port->second, pairing_port)) return "Invalid pairing request.";
            if (pairing.pair_remote(host->second, pairing_port, alias->second, code->second)) return "Pairing succeeded.";
            return "Pairing failed: " + pairing.last_error();
        }
        if (method == "POST" && target.rfind("/api/loopback", 0) == 0) {
            const auto params = query_params(target);
            const auto source = params.find("source"), sink = params.find("sink");
            if (source == params.end() || sink == params.end()) return "Missing Source or Sink.";
            if (route.start_loopback({source->second, sink->second, true})) return "Local route started.";
            return "Start failed: " + route.last_error();
        }
        if (method == "POST" && target.rfind("/api/mirror", 0) == 0) {
            const auto params = query_params(target);
            const auto source = params.find("source"), sink = params.find("sink");
            if (source == params.end() || sink == params.end()) return "Missing playback source or target.";
            if (endpoint_id(source->second) == endpoint_id(sink->second)) return "Choose a different playback target to avoid feedback.";
            if (route.start_loopback({source->second, sink->second, true})) return "Playback mirroring started.";
            return "Start failed: " + route.last_error();
        }
        if (method == "POST" && target.rfind("/api/network/send", 0) == 0) {
            const auto params = query_params(target);
            const auto host = params.find("host"), port = params.find("port"), source = params.find("source");
            std::uint16_t udp_port{}; gstreamer::NetworkAudioProfile profile;
            if (host == params.end() || source == params.end() || port == params.end() || !parse_udp_port(port->second, udp_port)
                || !network_profile_from(params, profile)) return "Invalid network sender settings.";
            gstreamer::SenderSettings settings;
            settings.host = host->second; settings.port = udp_port; settings.source_device = source->second;
            settings.capture_render_device = params.contains("loopback") && params.at("loopback") == "true";
            settings.network = profile;
            if (route.start_sender(settings)) { pairing.set_telemetry({quality_name(profile.quality), profile.max_latency_ms, 0.0, source->second}); pairing.announce(); return "Network sender started and telemetry synchronized."; }
            return "Sender failed: " + route.last_error();
        }
        if (method == "POST" && target.rfind("/api/network/receive", 0) == 0) {
            const auto params = query_params(target);
            const auto port = params.find("port"), sink = params.find("sink");
            std::uint16_t udp_port{}; gstreamer::NetworkAudioProfile profile;
            if (sink == params.end() || port == params.end() || !parse_udp_port(port->second, udp_port)
                || !network_profile_from(params, profile)) return "Invalid network receiver settings.";
            gstreamer::ReceiverSettings settings;
            settings.port = udp_port; settings.sink_device = sink->second; settings.network = profile;
            if (route.start_receiver(settings)) { pairing.set_telemetry({quality_name(profile.quality), profile.max_latency_ms, 0.0, sink->second}); pairing.announce(); return "Network receiver started and telemetry synchronized."; }
            return "Receiver failed: " + route.last_error();
        }
        if (method == "POST" && target.rfind("/api/matrix", 0) == 0) {
            const auto params = query_params(target);
            const auto routes = params.find("routes");
            if (routes == params.end() || routes->second.empty()) return "Select at least one matrix route.";
            gstreamer::LocalMatrixSettings settings;
            std::unordered_map<std::string, std::size_t> sources, sinks;
            // The embedded page serializes separators as JavaScript escape
            // sequences. Normalize them before parsing, while still accepting
            // ordinary literal tabs/newlines from other future clients.
            std::string route_text = routes->second;
            for (std::size_t position = 0; (position = route_text.find("\\\\n", position)) != std::string::npos; ++position)
                route_text.replace(position, 2, "\n");
            for (std::size_t position = 0; (position = route_text.find("\\\\t", position)) != std::string::npos; ++position)
                route_text.replace(position, 2, "\t");
            std::stringstream stream(route_text); std::string line;
            while (std::getline(stream, line, '\n')) {
                const auto first_separator = line.find('\t');
                const auto second_separator = line.find('\t', first_separator + 1);
                if (first_separator == std::string::npos || second_separator == std::string::npos) continue;
                const std::string source = line.substr(0, first_separator);
                const bool is_render_loopback = line.substr(first_separator + 1, second_separator - first_separator - 1) == "true";
                const std::string sink = line.substr(second_separator + 1);
                if (is_render_loopback && endpoint_id(source) == endpoint_id(sink)) continue;
                const auto [source_it, source_new] = sources.emplace(source, sources.size());
                if (source_new) { settings.source_devices.push_back(source); settings.source_is_render_loopback.push_back(is_render_loopback); }
                const auto [sink_it, sink_new] = sinks.emplace(sink, sinks.size());
                if (sink_new) settings.sink_devices.push_back(sink);
                settings.routes.push_back({source_it->second, sink_it->second});
            }
            if (route.start_local_matrix(settings)) return "Matrix started.";
            return "Matrix failed: " + route.last_error();
        }
        if (method == "POST" && target.rfind("/api/stop", 0) == 0) { route.stop(); pairing.set_telemetry({}); pairing.announce(); return "Route stopped and telemetry synchronized."; }
        if (method == "GET" && (target == "/" || target.rfind("/?", 0) == 0)) { type = "text/html; charset=utf-8"; return kPairControlPage; }
        return "Not found";
    }
};

WebServer::WebServer() : impl_(std::make_unique<Impl>()) {}
WebServer::~WebServer() { stop(); }

bool WebServer::serve(std::uint16_t port) {
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
        if (select(static_cast<int>(listener + 1), &set, nullptr, nullptr, &timeout) <= 0) continue;
        Socket client = accept(listener, nullptr, nullptr); if (client == kInvalidSocket) continue;
        char buffer[8192]{}; const int length = recv(client, buffer, sizeof(buffer) - 1, 0);
        std::string first_line(buffer, length > 0 ? static_cast<std::size_t>(length) : 0);
        const auto end = first_line.find("\r\n"); first_line.resize(end == std::string::npos ? first_line.size() : end);
        std::istringstream request(first_line); std::string method, target; request >> method >> target;
        std::string type; const std::string body = impl_->handle(method, target, type);
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        send(client, response.data(), static_cast<int>(response.size()), 0); close_socket(client);
        if (!impl_->route.poll()) { impl_->error = impl_->route.last_error(); impl_->route.stop(); }
    }
    close_socket(listener);
    impl_->pairing.stop();
#ifdef _WIN32
    WSACleanup();
#endif
    return true;
}

void WebServer::stop() noexcept { impl_->stopping = true; impl_->route.stop(); impl_->pairing.stop(); }
const std::string& WebServer::last_error() const noexcept { return impl_->error; }

} // namespace oamr::web


