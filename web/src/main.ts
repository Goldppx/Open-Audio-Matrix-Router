import '@material/web/button/filled-button.js';
import '@material/web/button/outlined-button.js';
import '@material/web/button/text-button.js';
import '@material/web/checkbox/checkbox.js';
import '@material/web/dialog/dialog.js';
import '@material/web/divider/divider.js';
import '@material/web/iconbutton/icon-button.js';
import '@material/web/select/outlined-select.js';
import '@material/web/select/select-option.js';
import '@material/web/textfield/outlined-text-field.js';
import './style.css';

type Direction = 'source' | 'sink';
type Quality = 'low' | 'medium' | 'high';
type Mode = 'stable' | 'auto' | 'low-latency';
type Device = { id: string; name: string; endpoint?: string; renderLoopback?: boolean; peer?: string; remote?: boolean };
type RemoteDevice = Device & { peer: string; remote: true };
type Devices = { sources: Device[]; sinks: Device[] };
type Endpoint = { id: string; name: string; direction: Direction };
type Peer = {
  nodeId: string; alias: string; host: string; port: number; endpoints: Endpoint[];
  telemetry: { quality: string; latencyMs: number; packetLossPercent: number; deviceName: string };
};
type Route = { id: number; label: string; enabled: boolean; network: boolean; quality?: Quality; latency?: number; mode?: Mode };
type Checkable = HTMLElement & { checked: boolean };
type ValueElement = HTMLElement & { value: string };
type DialogElement = HTMLElement & { show: () => Promise<void>; close: () => Promise<void> };

const app = document.querySelector<HTMLDivElement>('#app')!;
let devices: Devices = { sources: [], sinks: [] };
let peers: Peer[] = [];
let matrixDirty = false;
let deleteConfirmation: number | undefined;
let selectedRoute: Route | undefined;
let localAlias = 'This computer';
type LogLevel = 'VERBOSE' | 'INFO' | 'WARNING' | 'ERROR';
const logs: Array<{ level: LogLevel; message: string; time: string }> = [];
const language = localStorage.getItem('oamr-language') === 'en' ? 'en' : 'zh-CN';
const theme = localStorage.getItem('oamr-theme') === 'dark' ? 'dark' : 'light';

const escapeHtml = (value: unknown) => String(value).replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[char]!);
const byId = <T extends HTMLElement>(id: string) => document.querySelector<T>(`#${id}`)!;
const value = (id: string) => (byId<ValueElement>(id).value ?? '').trim();
const checked = (selector: string) => Array.from(document.querySelectorAll<Checkable>(selector)).filter(item => item.checked);

function logEvent(message: string, level: LogLevel = 'INFO'): void {
  logs.unshift({ level, message, time: new Date().toLocaleTimeString() });
  renderLogs();
}

function setStatus(message: string, _level: LogLevel = 'INFO'): void {
  void message;
}

function renderLogs(): void {
  const output = document.querySelector<HTMLElement>('#status');
  if (!output) return;
  const filter = document.querySelector<ValueElement>('#logLevel')?.value ?? 'INFO';
  const minimum = ['VERBOSE', 'INFO', 'WARNING', 'ERROR'].indexOf(filter);
  output.textContent = logs.filter(item => ['VERBOSE', 'INFO', 'WARNING', 'ERROR'].indexOf(item.level) >= minimum)
    .map(item => `[${item.time}] ${item.level}: ${item.message}`).join('\n') || 'No log entries.';
}

async function request(path: string, method = 'GET'): Promise<string> {
  const response = await fetch(path, { method });
  const body = await response.text();
  if (!response.ok) throw new Error(body || `HTTP ${response.status}`);
  return body;
}

async function post(path: string): Promise<string> {
  try {
    const message = await request(path, 'POST');
    setStatus(message);
    if (/failed|could not|invalid|error/i.test(message)) logEvent(message, 'ERROR');
    else if (/route|pair/i.test(path)) logEvent(message, 'INFO');
    await refreshRoutes();
    return message;
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    setStatus(`Operation failed: ${message}`);
    logEvent(`Operation failed: ${message}`, 'ERROR');
    throw error;
  }
}

function selectOptions(items: Device[]): string {
  return items.map(item => `<md-select-option value="${escapeHtml(item.id)}"><div slot="headline">${escapeHtml(item.name)}</div></md-select-option>`).join('');
}

function tutorialContent(): string {
  if (language === 'en') return `<div slot="headline">OAMR guide</div><div slot="content" class="tutorial-content"><section><h3>Start here</h3><p>Choose a source and a playback target in Audio matrix, then add the selected route.</p></section><section><h3>LAN audio</h3><p>Use Send to LAN and Receive from LAN for a direct RTP connection. For paired devices, select the remote endpoint directly in the matrix.</p></section><section><h3>Pair devices</h3><p>Expose local endpoints, generate a one-time code, then enter the other device IP and its code. Pairing control always uses port 8791.</p></section><section><h3>Route settings</h3><p>Use Configure in Route table to change quality, latency target, and mode after creating a network route.</p></section><section><h3>Telemetry</h3><p>Receiver-side packet loss is sampled from the active RTP jitter buffer and synchronized to paired devices. A dash means the route has no local receive leg yet.</p></section></div>`;
  return `<div slot="headline">OAMR 使用说明</div><div slot="content" class="tutorial-content"><section><h3>快速开始</h3><p>在音频矩阵中选择来源与播放目标，然后添加已勾选的路线。</p></section><section><h3>局域网音频</h3><p>使用发送到局域网和从局域网接收创建直接 RTP 连接；已配对设备可直接在矩阵中选择远端端点。</p></section><section><h3>设备配对</h3><p>开放本机端点，生成一次性代码，然后输入另一台设备的 IP 与代码。配对控制固定使用 8791 端口。</p></section><section><h3>路线配置</h3><p>网络路线创建后，可在路由表中点击配置，调整音质、目标延迟和模式。</p></section><section><h3>遥测</h3><p>接收端会从活动 RTP 抖动缓冲区采样真实丢包率，并同步到已配对设备；横线表示本机尚无可采样的接收链路。</p></section></div>`;
}

function renderShell(): void {
  app.innerHTML = `
    <main class="app-shell">
      <header class="app-header">
        <h1>Open Audio Matrix Router</h1>
      </header>
      <div class="grid">
        <section class="card wide"><h2>路由表</h2><div id="routeTable" class="empty">正在加载路线…</div><div class="actions"><md-outlined-button id="stopAll">停止并清空所有路由</md-outlined-button></div></section>

        <section class="card"><h2>发送到局域网</h2><div class="stack"><md-outlined-select id="networkSource" label="音频来源"></md-outlined-select><div class="form-grid"><md-outlined-text-field id="sendHost" label="IP / 主机名" value="127.0.0.1"></md-outlined-text-field><md-outlined-text-field id="sendPort" label="UDP 端口" type="number" value="5004"></md-outlined-text-field></div><md-filled-button id="createSender">创建发送路线</md-filled-button></div></section>
        <section class="card"><h2>从局域网接收</h2><div class="stack"><md-outlined-select id="receiveSink" label="播放到"></md-outlined-select><md-outlined-text-field id="receivePort" label="UDP 端口" type="number" value="5004"></md-outlined-text-field><md-filled-button id="createReceiver">创建接收路线</md-filled-button></div></section>

        <section class="card wide"><h2>音频矩阵</h2><div id="matrix" class="data-table-wrap"><div class="empty">正在加载设备…</div></div><div class="actions"><md-filled-button id="applyMatrix">添加已勾选的路线</md-filled-button></div></section>

        <section class="card wide"><h2>设备配对</h2><div id="exposure" class="exposure-list"></div><div class="actions"><md-outlined-button id="saveProfile">保存开放设备</md-outlined-button></div><md-divider></md-divider><div class="pairing-grid"><div class="stack"><md-outlined-text-field id="pairCode" label="一次性配对代码" readonly><md-icon-button id="newCode" class="code-action" slot="trailing-icon" title="${language === 'en' ? 'Generate code' : '生成新代码'}" aria-label="${language === 'en' ? 'Generate code' : '生成新代码'}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M17.65 6.35A7.95 7.95 0 0 0 12 4a8 8 0 1 0 7.75 10h-2.08A6 6 0 1 1 12 6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35Z"/></svg></md-icon-button></md-outlined-text-field></div><div class="stack"><md-outlined-text-field id="peerHost" label="另一台设备的 IP / 主机名" placeholder="192.168.31.100"></md-outlined-text-field><md-outlined-text-field id="peerCode" label="对方的一次性代码"></md-outlined-text-field></div></div><div class="actions"><md-filled-button id="pairRemote">开始配对</md-filled-button></div></section>

        <section class="card wide"><h2>已配对设备与传输遥测</h2><div id="peerList" class="empty">尚未配对设备。</div></section>
        <section class="card wide"><h2>运行日志</h2><pre id="status" class="log">No log entries.</pre></section>
      </div>
    </main>
    <md-dialog id="peerDialog"><div slot="headline">更新设备地址</div><form id="peerForm" slot="content" class="dialog-form" method="dialog"><md-outlined-text-field id="editHost" label="IP / 主机名"></md-outlined-text-field></form><div slot="actions"><md-text-button id="cancelPeer">取消</md-text-button><md-filled-button id="savePeer">保存</md-filled-button></div></md-dialog>
    <md-dialog id="tutorialDialog">${tutorialContent()}<div slot="actions"><md-filled-button id="closeTutorial">${language === 'en' ? 'Got it' : '知道了'}</md-filled-button></div></md-dialog>`;
}

function renderDeviceControls(): void {
  byId<HTMLElement>('networkSource').innerHTML = selectOptions(devices.sources);
  byId<HTMLElement>('receiveSink').innerHTML = selectOptions(devices.sinks);
  byId<HTMLElement>('exposure').innerHTML = [
    ...devices.sources.map(device => `<label class="exposure-row"><md-checkbox data-direction="source" data-id="${escapeHtml(device.id)}" data-name="${escapeHtml(device.name)}"></md-checkbox><span>来源 · ${escapeHtml(device.name)}</span></label>`),
    ...devices.sinks.map(device => `<label class="exposure-row"><md-checkbox data-direction="sink" data-id="${escapeHtml(device.id)}" data-name="${escapeHtml(device.name)}"></md-checkbox><span>输出 · ${escapeHtml(device.name)}</span></label>`)
  ].join('');
}

function enhancePanels(): void {
  const header = document.querySelector<HTMLElement>('.app-header');
  if (header && !document.querySelector('#uiLanguage')) {
    const themeLabel = language === 'en' ? (theme === 'dark' ? 'Use light theme' : 'Use dark theme') : (theme === 'dark' ? '切换浅色模式' : '切换深色模式');
    const refreshLabel = language === 'en' ? 'Refresh page' : '刷新页面';
    const languageLabel = language === 'en' ? 'Choose language' : '选择语言';
    const controls = document.createElement('div');
    controls.className = 'header-controls';
    const tutorialLabel = language === 'en' ? 'Open guide' : '打开教程';
    controls.innerHTML = `<md-icon-button id="openTutorial" title="${tutorialLabel}" aria-label="${tutorialLabel}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M11 18h2v-2h-2v2Zm1-16A10 10 0 1 0 12 22 10 10 0 0 0 12 2Zm0 18a8 8 0 1 1 8-8 8 8 0 0 1-8 8Zm0-14a3 3 0 0 0-3 3h2a1 1 0 1 1 1 1 3 3 0 0 0-1 5v1h2v-1a3 3 0 0 0-1-5 1 1 0 1 1 1-1h2a3 3 0 0 0-3-3Z"/></svg></md-icon-button><div class="language-control"><md-icon-button id="uiLanguage" title="${languageLabel}" aria-label="${languageLabel}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12.87 15.07 10.33 12.56l.03-.03A17.3 17.3 0 0 0 14.07 6h2.86V4h-7V2h-2v2H1v2h11.17a15.4 15.4 0 0 1-2.82 5.35A15.1 15.1 0 0 1 7.35 8.6h-2a17 17 0 0 0 2.73 4.17l-5.07 5.02L4.41 19.2l5-5 3.11 3.11.35-2.24ZM18.5 10h-2l-4.5 12h2l1.12-3h4.75L21 22h2l-4.5-12Zm-2.63 7 1.63-4.33L19.13 17h-3.26Z"/></svg></md-icon-button><div id="languageMenu" class="language-menu" role="menu" hidden><md-text-button data-language="zh-CN">中文</md-text-button><md-text-button data-language="en">English</md-text-button></div></div><md-icon-button id="themeToggle" title="${themeLabel}" aria-label="${themeLabel}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3a9 9 0 1 0 9 9c0-.46-.04-.91-.1-1.35A7 7 0 0 1 12 3Z"/></svg></md-icon-button><md-icon-button id="refreshPage" title="${refreshLabel}" aria-label="${refreshLabel}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M17.65 6.35A7.95 7.95 0 0 0 12 4a8 8 0 1 0 7.75 10h-2.08A6 6 0 1 1 12 6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35Z"/></svg></md-icon-button>`;
    header.append(controls);
    const languageMenu = byId<HTMLElement>('languageMenu');
    const closeLanguageMenu = () => {
      if (languageMenu.hidden || !languageMenu.classList.contains('open')) return;
      languageMenu.classList.remove('open');
      languageMenu.addEventListener('transitionend', () => { if (!languageMenu.classList.contains('open')) languageMenu.hidden = true; }, { once: true });
    };
    byId<HTMLElement>('uiLanguage').addEventListener('click', () => {
      if (languageMenu.classList.contains('open')) { closeLanguageMenu(); return; }
      const button = byId<HTMLElement>('uiLanguage').getBoundingClientRect();
      languageMenu.classList.toggle('align-right', window.innerWidth - button.left < 132);
      languageMenu.hidden = false;
      window.requestAnimationFrame(() => languageMenu.classList.add('open'));
    });
    document.querySelectorAll<HTMLElement>('[data-language]').forEach(item => item.addEventListener('click', () => {
      localStorage.setItem('oamr-language', item.dataset.language ?? 'zh-CN');
      window.location.reload();
    }));
    document.addEventListener('click', event => { if (!byId<HTMLElement>('uiLanguage').contains(event.target as Node) && !languageMenu.contains(event.target as Node)) closeLanguageMenu(); });
    byId<HTMLElement>('themeToggle').addEventListener('click', () => { localStorage.setItem('oamr-theme', theme === 'dark' ? 'light' : 'dark'); window.location.reload(); });
    byId<HTMLElement>('refreshPage').addEventListener('click', () => window.location.reload());
    byId<HTMLElement>('openTutorial').addEventListener('click', () => {
      // MdDialog completes its opening sequence asynchronously. Calling its
      // public API (rather than toggling an attribute) keeps focus trapping,
      // Escape handling, and Material's enter/exit animation intact.
      void byId<DialogElement>('tutorialDialog').show();
    });
    byId<HTMLElement>('closeTutorial').addEventListener('click', () => {
      void byId<DialogElement>('tutorialDialog').close();
    });
  }
  const log = byId<HTMLElement>('status');
  const logCard = log.closest('section');
  if (logCard && !document.querySelector('#logLevel')) {
    const controls = document.createElement('div');
    controls.className = 'log-controls';
    controls.innerHTML = `<md-outlined-select id="logLevel" label="显示级别"><md-select-option value="VERBOSE"><div slot="headline">VERBOSE</div></md-select-option><md-select-option value="INFO" selected><div slot="headline">INFO</div></md-select-option><md-select-option value="WARNING"><div slot="headline">WARNING</div></md-select-option><md-select-option value="ERROR"><div slot="headline">ERROR</div></md-select-option></md-outlined-select>`;
    logCard.insertBefore(controls, log);
    byId<HTMLElement>('logLevel').addEventListener('change', renderLogs);
  }
  const exposure = byId<HTMLElement>('exposure');
  const save = byId<HTMLElement>('saveProfile');
  if (!exposure.parentElement?.querySelector('.exposure-details')) {
    const details = document.createElement('section');
    details.className = 'exposure-details';
    details.innerHTML = '<button class="exposure-toggle" type="button" aria-expanded="false"><span>选择要向已配对设备开放的本机端点</span><svg viewBox="0 0 24 24" aria-hidden="true"><path d="m7.41 8.59L12 13.17l4.59-4.58L18 10l-6 6-6-6 1.41-1.41Z"/></svg></button><div class="exposure-content"></div>';
    exposure.replaceWith(details);
    const content = details.querySelector<HTMLElement>('.exposure-content')!;
    content.append(exposure);
    save.parentElement?.replaceWith(save);
    content.append(save);
    details.querySelector<HTMLButtonElement>('.exposure-toggle')!.addEventListener('click', () => {
      const expanded = details.classList.toggle('open');
      details.querySelector<HTMLButtonElement>('.exposure-toggle')!.setAttribute('aria-expanded', String(expanded));
    });
  }
}

function applyPreferences(): void {
  document.documentElement.lang = language;
  document.documentElement.dataset.theme = theme;
  if (language !== 'en') return;
  const dictionary: Record<string, string> = {
    '路由表': 'Route table', '发送到局域网': 'Send to LAN', '从局域网接收': 'Receive from LAN', '音频矩阵': 'Audio matrix', '设备配对': 'Device pairing',
    '已配对设备与传输遥测': 'Paired devices & telemetry', '运行日志': 'Event log', '停止并清空所有路由': 'Stop and clear all routes',
    '创建发送路线': 'Create sender route', '创建接收路线': 'Create receiver route', '添加已勾选的路线': 'Add selected routes',
    '保存开放设备': 'Save exposed devices', '生成新代码': 'Generate code', '开始配对': 'Pair device', '配置': 'Configure', '暂停': 'Pause', '恢复': 'Resume',
    '删除': 'Delete', '确认删除': 'Confirm delete', '运行中': 'Running', '已暂停': 'Paused', '本地路线': 'Local route', '暂无路线。': 'No routes.',
    '取消': 'Cancel', '保存': 'Save', '路线配置': 'Route settings', '音质': 'Quality', '最大延迟': 'Maximum latency', '模式': 'Mode',
    '稳定模式': 'Stable mode', '自动模式': 'Auto mode', '低延迟模式': 'Low-latency mode', '显示级别': 'Minimum level', '暂无日志。': 'No log entries.',
    '选择要向已配对设备开放的本机端点': 'Choose local endpoints exposed to paired devices',
    '本机音频矩阵 · 配对设备 · RTP/Opus 局域网传输': 'Local audio matrix · paired devices · RTP/Opus LAN streaming',
    '仅本机 Web UI': 'Local-only Web UI',
    '每条路线独立运行。网络路线的音质、最大延迟和模式在这里修改。': 'Each route runs independently. Configure quality, maximum latency, and mode for network routes here.',
    '为未配对设备手工创建 RTP 发送路线。': 'Create an RTP sender route manually for an unpaired device.',
    '为未配对设备手工创建 RTP 接收路线。': 'Create an RTP receiver route manually for an unpaired device.',
    '勾选来源与播放目标的交叉点即可创建路线。本机、已配对设备都会列在这里；网络路线的属性在路由表中设置。': 'Select source-to-target intersections to create routes. Local and paired devices are listed here; configure network routes in the route table.',
    '网页仅监听本机。配对控制使用 TCP 8791，一次性代码十分钟有效且仅能使用一次。': 'The web UI listens locally only. Pairing control uses TCP 8791; one-time codes expire after ten minutes and work once.',
    '允许已配对设备看到的本机端点': 'Local endpoints visible to paired devices',
    '保存开放设备、修改别名、启动或停止网络流后会同步；此处每五秒自动刷新。': 'Changes to exposed devices, aliases, and network streams are synchronized. This list refreshes every five seconds.',
    '音频来源': 'Audio source', 'IP / 主机名': 'IP / host name', 'UDP 端口': 'UDP port', '播放到': 'Play to',
    '本机别名': 'Local alias', '配对控制端口': 'Pairing control port', '一次性配对代码': 'One-time pairing code',
    '另一台设备的 IP / 主机名': 'Other device IP / host name', 'TCP 端口': 'TCP port', '别名': 'Alias', '对方的一次性代码': 'Peer one-time code',
    '来源 \\ 播放目标': 'Source \\ playback target', '暂不支持': 'Not supported', '禁用': 'Disabled', '当前没有传输音频': 'No active audio stream',
    '对方未开放设备': 'The peer exposes no devices', '尚未配对设备。': 'No paired devices.', '来源': 'Source', '输出': 'Output', '编辑': 'Edit',
    '传输属性': 'Transport properties', '路线': 'Route', '状态': 'Status', '操作': 'Actions', '低': 'Low', '中': 'Medium', '高': 'High',
    '正在加载路线…': 'Loading routes…', '正在加载设备…': 'Loading devices…', '更新地址': 'Update address', '更新设备地址': 'Update device address', '网络': 'Network'
  };
  const translate = (input: string) => dictionary[input.trim()] ?? input;
  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
  const nodes: Text[] = []; let node: Node | null;
  while ((node = walker.nextNode())) nodes.push(node as Text);
  nodes.forEach(item => { const replacement = translate(item.data); if (replacement !== item.data) item.data = replacement; });
  document.querySelectorAll<HTMLElement>('[label],[placeholder],[title],[aria-label]').forEach(element => {
    for (const attribute of ['label', 'placeholder', 'title', 'aria-label']) {
      const current = element.getAttribute(attribute); if (current) element.setAttribute(attribute, translate(current));
    }
  });
}

function remoteDevices(direction: Direction): RemoteDevice[] {
  return peers.flatMap(peer => peer.endpoints.filter(endpoint => endpoint.direction === direction).map(endpoint => ({ id: endpoint.id, name: `${peer.alias} · ${endpoint.name}`, peer: peer.nodeId, remote: true })));
}

function isRemoteDevice(device: Device | RemoteDevice): device is RemoteDevice {
  return 'remote' in device && device.remote === true;
}

function renderMatrix(): void {
  const remoteSources = remoteDevices('source');
  const remoteSinks = remoteDevices('sink');
  const sources = [...devices.sources, ...remoteSources];
  const sinks = [...devices.sinks, ...remoteSinks];
  const rows = sources.map(source => {
    const cells = sinks.map(sink => {
      if ('remote' in source && 'remote' in sink) return '<td class="disabled">暂不支持</td>';
      if ('remote' in source) return `<td><md-checkbox data-kind="receive" data-peer="${escapeHtml(source.peer)}" data-remote="${escapeHtml(source.id)}" data-local="${escapeHtml(sink.id)}"></md-checkbox></td>`;
      if ('remote' in sink) return `<td><md-checkbox data-kind="send" data-peer="${escapeHtml(sink.peer)}" data-remote="${escapeHtml(sink.id)}" data-local="${escapeHtml(source.id)}" data-loopback="${source.renderLoopback === true}"></md-checkbox></td>`;
      if (source.renderLoopback && source.endpoint === sink.endpoint) return '<td class="disabled">禁用</td>';
      return `<td><md-checkbox data-source="${escapeHtml(source.id)}" data-sink="${escapeHtml(sink.id)}" data-loopback="${source.renderLoopback === true}"></md-checkbox></td>`;
    }).join('');
    return `<tr><th>${escapeHtml(source.name)}${'remote' in source ? ' <small>网络</small>' : ''}</th>${cells}</tr>`;
  }).join('');
  byId<HTMLElement>('matrix').innerHTML = `<table class="data-table matrix-table"><thead><tr><th>来源 \ 播放目标</th>${sinks.map(sink => `<th>${escapeHtml(sink.name)}${'remote' in sink ? ' <small>网络</small>' : ''}</th>`).join('')}</tr></thead><tbody>${rows}</tbody></table>`;
  document.querySelectorAll<HTMLElement>('#matrix md-checkbox').forEach(item => item.addEventListener('change', () => { matrixDirty = true; }));
  applyPreferences();
}

function renderRoutes(routes: Route[]): void {
  const rows = routes.map(route => {
    const deleteText = deleteConfirmation === route.id ? '确认删除' : '删除';
    const deleteClass = deleteConfirmation === route.id ? 'danger-action' : '';
    const configuration = route.network ? `<md-text-button data-config="${route.id}">配置</md-text-button>` : '';
    return `<tr><td>${escapeHtml(route.label)}</td><td>${route.enabled ? '运行中' : '已暂停'}</td><td>${route.network ? `${route.quality} · ${route.latency} ms · ${route.mode}` : '本地路线'}</td><td><div class="route-actions"><md-text-button data-toggle="${route.id}" data-enabled="${!route.enabled}">${route.enabled ? '暂停' : '恢复'}</md-text-button>${configuration}<md-text-button class="${deleteClass}" data-delete="${route.id}">${deleteText}</md-text-button></div></td></tr>`;
  }).join('');
  byId<HTMLElement>('routeTable').innerHTML = routes.length ? `<div class="data-table-wrap"><table class="data-table"><thead><tr><th>路线</th><th>状态</th><th>传输属性</th><th>操作</th></tr></thead><tbody>${rows}</tbody></table></div>` : '<div class="empty">暂无路线。</div>';
  document.querySelectorAll<HTMLElement>('[data-toggle]').forEach(button => button.addEventListener('click', () => void post(`/api/routes/${button.dataset.toggle}/toggle?enabled=${button.dataset.enabled}`)));
  document.querySelectorAll<HTMLElement>('[data-config]').forEach(button => button.addEventListener('click', () => openRouteDialog(routes.find(route => route.id === Number(button.dataset.config))!)));
  document.querySelectorAll<HTMLElement>('[data-delete]').forEach(button => button.addEventListener('click', () => {
    const id = Number(button.dataset.delete);
    if (deleteConfirmation !== id) { deleteConfirmation = id; renderRoutes(routes); setStatus('再次点击“确认删除”才会移除此路线。', 'WARNING'); return; }
    deleteConfirmation = undefined;
    void post(`/api/routes/${id}/delete`);
  }));
  applyPreferences();
}

function openRouteDialog(route: Route): void {
  selectedRoute = route;
  let dialog = document.querySelector<HTMLElement & { show: () => void; close: () => void }>('#routeDialog');
  if (!dialog) {
    dialog = document.createElement('md-dialog') as HTMLElement & { show: () => void; close: () => void };
    dialog.id = 'routeDialog';
    dialog.innerHTML = `<div slot="headline">路线配置</div><div slot="content" class="dialog-form"><md-outlined-select id="routeQuality" label="音质"><md-select-option value="low"><div slot="headline">低</div></md-select-option><md-select-option value="medium"><div slot="headline">中</div></md-select-option><md-select-option value="high"><div slot="headline">高</div></md-select-option></md-outlined-select><md-outlined-select id="routeLatency" label="最大延迟"><md-select-option value="40"><div slot="headline">40 ms</div></md-select-option><md-select-option value="60"><div slot="headline">60 ms</div></md-select-option><md-select-option value="100"><div slot="headline">100 ms</div></md-select-option><md-select-option value="150"><div slot="headline">150 ms</div></md-select-option></md-outlined-select><md-outlined-select id="routeMode" label="模式"><md-select-option value="stable"><div slot="headline">稳定模式</div></md-select-option><md-select-option value="auto"><div slot="headline">自动模式</div></md-select-option><md-select-option value="low-latency"><div slot="headline">低延迟模式</div></md-select-option></md-outlined-select></div><div slot="actions"><md-text-button id="cancelRoute">取消</md-text-button><md-filled-button id="saveRoute">保存</md-filled-button></div>`;
    document.body.append(dialog);
    byId<HTMLElement>('cancelRoute').addEventListener('click', () => dialog?.close());
    byId<HTMLElement>('saveRoute').addEventListener('click', () => void (async () => {
      if (!selectedRoute) return;
      await post(`/api/routes/${selectedRoute.id}/profile?quality=${value('routeQuality')}&max-latency-ms=${value('routeLatency')}&mode=${value('routeMode')}`);
      dialog?.close();
    })());
  }
  byId<ValueElement>('routeQuality').value = route.quality ?? 'medium';
  byId<ValueElement>('routeLatency').value = String(route.latency ?? 100);
  byId<ValueElement>('routeMode').value = route.mode ?? 'auto';
  dialog.show();
}

function renderPeers(): void {
  byId<HTMLElement>('peerList').innerHTML = peers.length ? peers.map(peer => {
    const loss = peer.telemetry.packetLossPercent < 0 ? '—' : `${peer.telemetry.packetLossPercent.toFixed(2)}%`;
    const telemetry = peer.telemetry.deviceName ? `音质 ${escapeHtml(peer.telemetry.quality)} · 目标延迟 ${peer.telemetry.latencyMs} ms · 丢包 ${loss} · 设备 ${escapeHtml(peer.telemetry.deviceName)}` : '当前没有传输音频';
    const endpoints = peer.endpoints.length ? peer.endpoints.map(endpoint => `<md-assist-chip class="endpoint-chip" label="${endpoint.direction === 'source' ? '来源' : '输出'} · ${escapeHtml(endpoint.name)}"></md-assist-chip>`).join('') : '<span class="muted">对方未开放设备</span>';
    return `<article class="peer-card"><div class="peer-title"><strong>${escapeHtml(peer.alias)}</strong><md-text-button data-edit-peer="${escapeHtml(peer.nodeId)}">更新地址</md-text-button></div><div class="muted">${escapeHtml(peer.host)}:8791</div><div class="muted">${telemetry}</div><div>${endpoints}</div></article>`;
  }).join('') : '<div class="empty">尚未配对设备。</div>';
  document.querySelectorAll<HTMLElement>('[data-edit-peer]').forEach(button => button.addEventListener('click', () => openPeerDialog(button.dataset.editPeer!)));
  applyPreferences();
}

function openPeerDialog(nodeId: string): void {
  const peer = peers.find(item => item.nodeId === nodeId);
  if (!peer) return;
  byId<ValueElement>('editHost').value = peer.host;
  byId<HTMLElement & { show: () => void }>('peerDialog').show();
  byId<HTMLElement>('savePeer').onclick = () => void (async () => {
    await post(`/api/pair/endpoint?node=${encodeURIComponent(nodeId)}&host=${encodeURIComponent(value('editHost'))}&port=8791`);
    byId<HTMLElement & { close: () => void }>('peerDialog').close();
    await refreshPeers();
  })();
}

async function refreshPeers(): Promise<void> {
  peers = JSON.parse(await request('/api/pair/peers')) as Peer[];
  renderPeers();
  if (!matrixDirty) renderMatrix();
}

async function refreshRoutes(): Promise<void> {
  renderRoutes(JSON.parse(await request('/api/routes')) as Route[]);
}

async function applyMatrix(): Promise<void> {
  const selected = checked('#matrix md-checkbox');
  if (!selected.length) { setStatus('请至少选择一条矩阵路线。'); return; }
  const local = selected.filter(item => item.dataset.source);
  const remote = selected.filter(item => item.dataset.kind);
  if (local.length) {
    const routes = local.map(item => `${item.dataset.source}\t${item.dataset.loopback}\t${item.dataset.sink}`).join('\n');
    await post(`/api/matrix?routes=${encodeURIComponent(routes)}`);
  }
  for (const item of remote) {
    const query = new URLSearchParams({ peer: item.dataset.peer!, kind: item.dataset.kind!, local: item.dataset.local!, remote: item.dataset.remote!, loopback: item.dataset.loopback ?? 'false', quality: 'medium', 'max-latency-ms': '100', mode: 'auto' });
    await post(`/api/paired/route?${query}`);
  }
  await refreshPeers();
  matrixDirty = false;
  renderMatrix();
}

function wireEvents(): void {
  byId<HTMLElement>('stopAll').addEventListener('click', () => void post('/api/stop'));
  byId<HTMLElement>('createSender').addEventListener('click', () => {
    const source = devices.sources.find(item => item.id === value('networkSource'));
    if (!source) { setStatus('请选择音频来源。'); return; }
    const query = new URLSearchParams({ source: source.id, loopback: String(source.renderLoopback === true), host: value('sendHost'), port: value('sendPort'), quality: 'medium', 'max-latency-ms': '100', mode: 'auto' });
    void post(`/api/network/send?${query}`);
  });
  byId<HTMLElement>('createReceiver').addEventListener('click', () => {
    const query = new URLSearchParams({ sink: value('receiveSink'), port: value('receivePort'), quality: 'medium', 'max-latency-ms': '100', mode: 'auto' });
    void post(`/api/network/receive?${query}`);
  });
  byId<HTMLElement>('applyMatrix').addEventListener('click', () => void applyMatrix());
  byId<HTMLElement>('newCode').addEventListener('click', () => void (async () => { byId<ValueElement>('pairCode').value = await request('/api/pair/code'); setStatus('已生成一次性配对代码；十分钟内有效。'); })());
  byId<HTMLElement>('saveProfile').addEventListener('click', () => void (async () => {
    const endpoints = checked('#exposure md-checkbox').map(item => `${item.dataset.direction === 'source' ? 'S' : 'K'}\t${item.dataset.id}\t${item.dataset.name}`).join('\n');
    await post(`/api/pair/config?alias=${encodeURIComponent(localAlias)}&endpoints=${encodeURIComponent(endpoints)}`);
    await refreshPeers();
  })());
  byId<HTMLElement>('pairRemote').addEventListener('click', () => void (async () => {
    const query = new URLSearchParams({ host: value('peerHost'), port: '8791', alias: localAlias, code: value('peerCode') });
    const message = await post(`/api/pair/connect?${query}`);
    if (message.startsWith('Pairing succeeded')) await refreshPeers();
  })());
  byId<HTMLElement>('cancelPeer').addEventListener('click', () => byId<HTMLElement & { close: () => void }>('peerDialog').close());
}

async function start(): Promise<void> {
  renderShell();
  wireEvents();
  enhancePanels();
  applyPreferences();
  try {
    devices = JSON.parse(await request('/api/devices')) as Devices;
    const local = JSON.parse(await request('/api/pair/local')) as { alias: string };
    localAlias = local.alias;
    renderDeviceControls();
    await Promise.all([refreshPeers(), refreshRoutes()]);
    window.setInterval(() => { void refreshPeers().catch(() => undefined); void refreshRoutes().catch(() => undefined); }, 5000);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    setStatus(language === 'en' ? `Loading failed: ${message}` : `加载失败：${message}`);
    logEvent(`Startup failed: ${message}`, 'ERROR');
  }
}

void start();
