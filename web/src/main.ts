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
type Device = { id: string; name: string; endpoint?: string; renderLoopback?: boolean };
type RemoteDevice = Device & { peer: string; remote: true };
type Devices = { sources: Device[]; sinks: Device[] };
type Endpoint = { id: string; name: string; direction: Direction };
type Peer = {
  nodeId: string; alias: string; host: string; port: number; endpoints: Endpoint[];
  telemetry: { quality: string; latencyMs: number; packetLossPercent: number; deviceName: string };
};
type DiscoveredDevice = { nodeId: string; alias: string; host: string; port: number };
type DiscoveryState = { enabled: boolean; devices: DiscoveredDevice[] };
type Route = { id: number; label: string; enabled: boolean; network: boolean; quality?: Quality; latency?: number; mode?: Mode };
type MatrixChoice = {
  key: string; source?: string; sink?: string; kind?: 'send' | 'receive'; peer?: string;
  remote?: string; local?: string; loopback: boolean; sourceName: string; targetName: string;
};
type Checkable = HTMLElement & { checked: boolean };
type ValueElement = HTMLElement & { value: string };
type DialogElement = HTMLElement & { show: () => Promise<void>; close: () => Promise<void> };

const app = document.querySelector<HTMLDivElement>('#app')!;
let devices: Devices = { sources: [], sinks: [] };
let peers: Peer[] = [];
const matrixSelections = new Set<string>();
const matrixChoices = new Map<string, MatrixChoice>();
let mobileMatrixSource = '';
let matrixView: 'auto' | 'builder' | 'matrix' = 'auto';
let peerTopologyFingerprint = '';
let routeFingerprint = '';
let discoveryFingerprint = '';
let deleteConfirmation: number | undefined;
let peerDeleteConfirmation: string | undefined;
let selectedRoute: Route | undefined;
let localAlias = 'This computer';
type LogLevel = 'VERBOSE' | 'INFO' | 'WARNING' | 'ERROR';
const logs: Array<{ level: LogLevel; message: string; time: string }> = [];
type ServerLogEntry = { sequence: number; timestamp: number; level: LogLevel; message: string };
let lastServerLogSequence = 0;
const language = localStorage.getItem('oamr-language') === 'en' ? 'en' : 'zh-CN';
const theme = localStorage.getItem('oamr-theme') === 'dark' ? 'dark' : 'light';

const escapeHtml = (value: unknown) => String(value).replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[char]!);
const byId = <T extends HTMLElement>(id: string) => document.querySelector<T>(`#${id}`)!;
const value = (id: string) => (byId<ValueElement>(id).value ?? '').trim();
const checked = (selector: string) => Array.from(document.querySelectorAll<Checkable>(selector)).filter(item => item.checked);
const uiLabel = (zh: string, en: string) => language === 'en' ? en : zh;
const iconButton = (attribute: string, label: string, path: string, className = '') => `<md-icon-button ${attribute} class="${className}" title="${escapeHtml(label)}" aria-label="${escapeHtml(label)}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="${path}"/></svg></md-icon-button>`;
const icons = {
  addLink: 'M18 13c.56 0 1.08-.15 1.54-.39L13.5 6.57A4 4 0 0 0 12 6.05V8.1c.22.1.42.24.59.41l4.94 4.94c.15-.28.29-.42.47-.42ZM6.46 11.39 2.5 7.43 3.91 6.02l16.97 16.97-1.41 1.41-3.16-3.16a4 4 0 0 1-5.95-5.21l-2.49-2.49A4 4 0 0 1 6.46 11.39ZM7 14a2 2 0 1 0 2.83 2.83L7 14Zm5.4 5.4 1.42 1.42A4 4 0 0 1 8.18 15.2l1.42 1.42a2 2 0 0 0 2.8 2.78Z',
  send: 'M2.01 21 23 12 2.01 3 2 10l15 2-15 2 .01 7Z',
  receive: 'M19 3h2v6h-2V6.41l-5.29 5.3-1.42-1.42L17.59 5H15V3h4ZM5 15h2v2.59l5.29-5.3 1.42 1.42L8.41 19H11v2H5v-6Z',
  edit: 'M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25ZM20.71 7.04a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83Z',
  delete: 'M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12ZM8 9h8v10H8V9Zm7.5-5-1-1h-5l-1 1H5v2h14V4h-3.5Z',
  play: 'M8 5v14l11-7L8 5Z',
  pause: 'M6 19h4V5H6v14Zm8-14v14h4V5h-4Z',
  pair: 'M10.59 13.41 9.17 12l2.83-2.83 2.83 2.83-1.42 1.41L12 12l-1.41 1.41ZM7.05 16.95a5 5 0 0 1 0-7.07l2.12 2.12a2 2 0 0 0 0 2.83l-2.12 2.12ZM16.95 7.05a5 5 0 0 1 0 7.07l-2.12-2.12a2 2 0 0 0 0-2.83l2.12-2.12Z'
};

function logEvent(message: string, level: LogLevel = 'INFO', timestamp = new Date()): void {
  const time = timestamp.toLocaleTimeString();
  if (logs[0]?.level === level && logs[0]?.message === message && logs[0]?.time === time) return;
  logs.unshift({ level, message, time });
  if (logs.length > 500) logs.length = 500;
  renderLogs();
}

async function refreshServerLogs(): Promise<void> {
  const entries = JSON.parse(await request(`/api/logs?after=${lastServerLogSequence}`)) as ServerLogEntry[];
  for (const entry of entries) {
    lastServerLogSequence = Math.max(lastServerLogSequence, entry.sequence);
    logEvent(entry.message, entry.level, new Date(entry.timestamp));
  }
}

/**
 * Material Web's dialog scroll container lives in its open Shadow DOM and does
 * not expose a CSS part or a scrollbar token. Add the same app-local scrollbar
 * treatment once, without changing the dialog's structure or behavior.
 */
function styleDialogScrollbar(dialog: DialogElement): void {
  const apply = () => {
    const root = dialog.shadowRoot;
    if (!root || root.querySelector('[data-oamr-dialog-scrollbar]')) return;
    const style = document.createElement('style');
    style.dataset.oamrDialogScrollbar = 'true';
    style.textContent = `.scroller { scrollbar-color: var(--md-sys-color-primary) var(--md-sys-color-surface-container-highest); scrollbar-width: thin; }
      .scroller::-webkit-scrollbar { width: var(--oamr-scrollbar-size); height: var(--oamr-scrollbar-size); }
      .scroller::-webkit-scrollbar-track { margin: var(--oamr-space-1); border-radius: var(--oamr-radius-pill); background: var(--md-sys-color-surface-container-highest); }
      .scroller::-webkit-scrollbar-thumb { min-height: 28px; border: 3px solid var(--md-sys-color-surface-container-highest); border-radius: var(--oamr-radius-pill); background: var(--md-sys-color-primary); }
      .scroller::-webkit-scrollbar-thumb:hover { background: var(--md-sys-color-primary); }`;
    root.append(style);
  };
  if (dialog.shadowRoot) apply();
  else void customElements.whenDefined('md-dialog').then(apply);
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

async function refreshPairCode(regenerate = false): Promise<void> {
  const code = await request('/api/pair/code', regenerate ? 'POST' : 'GET');
  const output = byId<HTMLOutputElement>('pairCode');
  if (output.textContent !== code) output.textContent = code;
}

async function post(path: string): Promise<string> {
  try {
    const message = await request(path, 'POST');
    if (/failed|could not|invalid|error/i.test(message)) logEvent(message, 'ERROR');
    await refreshRoutes();
    await refreshServerLogs();
    return message;
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
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
        <section class="card wide"><h2>路由表</h2><div id="routeTable" class="route-table-host"><div class="route-table-placeholder empty">正在加载路线…</div></div><div class="actions"><md-outlined-button id="stopAll">停止并清空所有路由</md-outlined-button></div></section>

        <section class="card"><h2>发送到局域网</h2><div class="stack"><md-outlined-select id="networkSource" label="音频来源"></md-outlined-select><div class="form-grid"><md-outlined-text-field id="sendHost" label="IP / 主机名" value="127.0.0.1"></md-outlined-text-field><md-outlined-text-field id="sendPort" label="UDP 端口" type="number" value="5004"></md-outlined-text-field></div></div><div class="card-action-bar"><div class="icon-action">${iconButton('id="createSender"', uiLabel('创建发送路线', 'Create sender route'), icons.send)}</div></div></section>
        <section class="card"><h2>从局域网接收</h2><div class="stack"><md-outlined-select id="receiveSink" label="播放到"></md-outlined-select><md-outlined-text-field id="receivePort" label="UDP 端口" type="number" value="5004"></md-outlined-text-field></div><div class="card-action-bar"><div class="icon-action">${iconButton('id="createReceiver"', uiLabel('创建接收路线', 'Create receiver route'), icons.receive)}</div></div></section>

        <section class="card wide"><h2>音频矩阵</h2><div id="matrix" class="matrix-host"><div class="data-table-wrap matrix-placeholder empty">正在加载设备…</div></div><div class="card-action-bar"><div class="icon-action">${iconButton('id="applyMatrix"', uiLabel('添加已勾选的路线', 'Add selected routes'), icons.addLink)}</div></div></section>

        <section class="card wide"><h2>设备配对</h2><div id="exposure" class="exposure-list"></div><div class="actions"><md-outlined-button id="saveProfile">保存开放设备</md-outlined-button></div><md-divider></md-divider><div class="pairing-grid"><div class="stack"><div class="pair-code-field" role="group" aria-labelledby="pairCodeLabel"><span id="pairCodeLabel" class="pair-code-label">一次性配对代码</span><output id="pairCode" class="pair-code-value" aria-live="polite">••••••</output><md-icon-button id="newCode" class="code-action" title="${language === 'en' ? 'Generate code' : '生成新代码'}" aria-label="${language === 'en' ? 'Generate code' : '生成新代码'}"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M17.65 6.35A7.95 7.95 0 0 0 12 4a8 8 0 1 0 7.75 10h-2.08A6 6 0 1 1 12 6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35Z"/></svg></md-icon-button></div></div><div class="stack"><md-outlined-text-field id="peerHost" label="另一台设备的 IP / 主机名" placeholder="192.168.31.100"></md-outlined-text-field><md-outlined-text-field id="peerCode" label="对方的一次性代码"></md-outlined-text-field></div></div><div class="card-action-bar"><div class="icon-action">${iconButton('id="pairRemote"', uiLabel('开始配对', 'Pair device'), icons.pair)}</div></div></section>

        <section class="card wide"><h2>已配对设备与传输遥测</h2><div id="peerList" class="peer-list-host"><div class="peer-list-placeholder empty">尚未配对设备。</div></div></section>
        <section class="card wide"><h2>运行日志</h2><pre id="status" class="log">No log entries.</pre></section>
      </div>
    </main>
    <md-dialog id="peerDialog"><div slot="headline">编辑已配对设备</div><form id="peerForm" slot="content" class="dialog-form" method="dialog"><md-outlined-text-field id="editAlias" label="设备名称"></md-outlined-text-field><md-outlined-text-field id="editHost" label="IP / 主机名"></md-outlined-text-field></form><div slot="actions"><md-text-button id="cancelPeer">取消</md-text-button><md-filled-button id="savePeer">保存</md-filled-button></div></md-dialog>
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

function renderDiscovery(state: DiscoveryState): void {
  const list = byId<HTMLElement>('discoveryList');
  list.innerHTML = state.enabled
    ? (state.devices.length ? state.devices.map(device => `<article class="discovery-device" data-host="${escapeHtml(device.host)}" data-port="${device.port}" data-alias="${escapeHtml(device.alias)}"><div><strong>${escapeHtml(device.alias)}</strong><div class="muted">${escapeHtml(device.host)}:${device.port}</div></div><md-outlined-text-field class="discovery-code" label="一次性配对代码"></md-outlined-text-field><md-filled-button class="pair-discovered">配对</md-filled-button></article>`).join('') : '<div class="empty">正在搜索局域网中的 OAMR 设备…</div>')
    : '<div class="muted">启用后会显示同一局域网中已启用发现的 OAMR 设备。</div>';
  document.querySelectorAll<HTMLElement>('.pair-discovered').forEach(button => button.addEventListener('click', () => void (async () => {
    const item = button.closest<HTMLElement>('.discovery-device'); const code = item?.querySelector<ValueElement>('.discovery-code')?.value.trim() ?? '';
    if (!item || !code) { logEvent('A one-time pairing code is required.', 'WARNING'); return; }
    const query = new URLSearchParams({ host: item.dataset.host!, port: item.dataset.port!, alias: localAlias, code });
    const message = await post(`/api/pair/connect?${query}`);
    if (message.startsWith('Pairing succeeded')) { await Promise.all([refreshPeers(), refreshDiscovery()]); }
  })()));
  applyPreferences();
}

async function refreshDiscovery(): Promise<void> {
  const next = JSON.parse(await request('/api/discovery')) as DiscoveryState;
  const fingerprint = JSON.stringify(next);
  if (fingerprint === discoveryFingerprint) return;
  discoveryFingerprint = fingerprint;
  const toggle = document.querySelector<Checkable>('#discoveryToggle');
  if (toggle) toggle.checked = next.enabled;
  renderDiscovery(next);
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
      const dialog = byId<DialogElement>('tutorialDialog');
      styleDialogScrollbar(dialog);
      void dialog.show();
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
  const pairingCard = exposure.closest<HTMLElement>('.card');
  const exposureDetails = exposure.closest<HTMLElement>('.exposure-details');
  if (pairingCard && exposureDetails && !document.querySelector('#discoveryPanel')) {
    const panel = document.createElement('section');
    panel.id = 'discoveryPanel';
    panel.className = 'discovery-panel';
    panel.innerHTML = '<div class="discovery-heading"><div><h3>局域网发现</h3><p class="muted">发现不会公开网页或配对密钥。</p></div><label class="discovery-toggle"><md-checkbox id="discoveryToggle"></md-checkbox><span>启用发现</span></label></div><div id="discoveryList"></div>';
    pairingCard.insertBefore(panel, exposureDetails);
    byId<HTMLElement>('discoveryToggle').addEventListener('change', () => void (async () => {
      const enabled = byId<Checkable>('discoveryToggle').checked;
      await post(`/api/discovery?enabled=${enabled}`);
      discoveryFingerprint = '';
      await refreshDiscovery();
    })());
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
    '正在加载路线…': 'Loading routes…', '正在加载设备…': 'Loading devices…', '更新地址': 'Update address', '更新设备地址': 'Update device address', '网络': 'Network',
    '编辑已配对设备': 'Edit paired device', '设备名称': 'Device name', '局域网发现': 'LAN discovery', '发现不会公开网页或配对密钥。': 'Discovery does not expose the web UI or pairing codes.',
    '启用发现': 'Enable discovery', '启用后会显示同一局域网中已启用发现的 OAMR 设备。': 'Enabled OAMR devices on this LAN appear here.', '正在搜索局域网中的 OAMR 设备…': 'Searching for OAMR devices on this LAN…',
    '路线选择': 'Route builder', '矩阵视图': 'Matrix view', '选择一个音频来源，然后勾选一个或多个播放目标。': 'Choose one audio source, then select one or more playback targets.',
    '播放目标': 'Playback targets', '本机': 'Local', '尚未选择路线。': 'No routes selected.'
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

function matrixDeviceKey(device: Device | RemoteDevice): string {
  return isRemoteDevice(device) ? `remote:${device.peer}:${device.id}` : `local:${device.id}`;
}

function matrixChoice(source: Device | RemoteDevice, sink: Device | RemoteDevice): MatrixChoice | undefined {
  if (isRemoteDevice(source) && isRemoteDevice(sink)) return undefined;
  if (isRemoteDevice(source)) {
    const key = JSON.stringify(['receive', source.peer, source.id, sink.id]);
    return { key, kind: 'receive', peer: source.peer, remote: source.id, local: sink.id, loopback: false, sourceName: source.name, targetName: sink.name };
  }
  if (isRemoteDevice(sink)) {
    const key = JSON.stringify(['send', sink.peer, sink.id, source.id, source.renderLoopback === true]);
    return { key, kind: 'send', peer: sink.peer, remote: sink.id, local: source.id, loopback: source.renderLoopback === true, sourceName: source.name, targetName: sink.name };
  }
  if (source.renderLoopback && source.endpoint === sink.endpoint) return undefined;
  const key = JSON.stringify(['local', source.id, sink.id, source.renderLoopback === true]);
  return { key, source: source.id, sink: sink.id, loopback: source.renderLoopback === true, sourceName: source.name, targetName: sink.name };
}

function matrixCheckbox(choice: MatrixChoice): string {
  return `<md-checkbox data-matrix-key="${escapeHtml(choice.key)}"${matrixSelections.has(choice.key) ? ' checked' : ''}></md-checkbox>`;
}

function matrixSummaryHtml(): string {
  const selected = [...matrixSelections].map(key => matrixChoices.get(key)).filter((choice): choice is MatrixChoice => choice !== undefined);
  if (!selected.length) return `<div class="matrix-summary-empty">${uiLabel('尚未选择路线。', 'No routes selected.')}</div>`;
  const mixerTargets = new Map<string, MatrixChoice[]>();
  for (const choice of selected) {
    const entries = mixerTargets.get(choice.targetName) ?? [];
    entries.push(choice);
    mixerTargets.set(choice.targetName, entries);
  }
  const mixerHints = [...mixerTargets.values()]
    .filter(entries => entries.length > 1)
    .map(entries => `<div class="matrix-mixer-hint">${uiLabel(`混音：${entries.length} 个来源 → ${entries[0].targetName}`, `Mixer: ${entries.length} sources → ${entries[0].targetName}`)}</div>`)
    .join('');
  return `<div class="matrix-summary-title">${uiLabel(`已选择 ${selected.length} 条路线`, `${selected.length} route${selected.length === 1 ? '' : 's'} selected`)}</div>${mixerHints}${selected.map(choice => `<div class="matrix-summary-route"><span>${escapeHtml(choice.sourceName)}</span><span aria-hidden="true">→</span><span>${escapeHtml(choice.targetName)}</span></div>`).join('')}`;
}

function updateMatrixViewState(): void {
  const host = document.querySelector<HTMLElement>('#matrix');
  if (!host) return;
  host.dataset.view = matrixView;
  const effective = matrixView === 'auto' ? (window.matchMedia('(max-width: 719px)').matches ? 'builder' : 'matrix') : matrixView;
  document.querySelectorAll<HTMLButtonElement>('[data-matrix-view]').forEach(button => {
    const active = button.dataset.matrixView === effective;
    button.classList.toggle('selected', active);
    button.setAttribute('aria-pressed', String(active));
  });
}

function wireMatrixControls(): void {
  document.querySelectorAll<Checkable>('[data-matrix-key]').forEach(item => item.addEventListener('change', () => {
    const key = item.dataset.matrixKey!;
    if (item.checked) matrixSelections.add(key); else matrixSelections.delete(key);
    document.querySelectorAll<Checkable>('[data-matrix-key]').forEach(peer => { if (peer.dataset.matrixKey === key) peer.checked = item.checked; });
    const summary = document.querySelector<HTMLElement>('.matrix-selection-summary');
    if (summary) summary.innerHTML = matrixSummaryHtml();
  }));
  document.querySelectorAll<HTMLButtonElement>('[data-matrix-view]').forEach(button => button.addEventListener('click', () => {
    matrixView = button.dataset.matrixView === 'builder' ? 'builder' : 'matrix';
    updateMatrixViewState();
  }));
  document.querySelector<HTMLElement>('#matrixMobileSource')?.addEventListener('change', () => {
    mobileMatrixSource = value('matrixMobileSource');
    renderMatrix();
  });
  updateMatrixViewState();
}

function renderMatrix(): void {
  const remoteSources = remoteDevices('source');
  const remoteSinks = remoteDevices('sink');
  const sources = [...devices.sources, ...remoteSources];
  const sinks = [...devices.sinks, ...remoteSinks];
  matrixChoices.clear();
  for (const source of sources) for (const sink of sinks) {
    const choice = matrixChoice(source, sink);
    if (choice) matrixChoices.set(choice.key, choice);
  }
  for (const key of [...matrixSelections]) if (!matrixChoices.has(key)) matrixSelections.delete(key);
  if (!sources.some(source => matrixDeviceKey(source) === mobileMatrixSource)) mobileMatrixSource = sources[0] ? matrixDeviceKey(sources[0]) : '';
  const activeSource = sources.find(source => matrixDeviceKey(source) === mobileMatrixSource);
  const rows = sources.map(source => {
    const cells = sinks.map(sink => {
      const choice = matrixChoice(source, sink);
      if (!choice) return `<td class="disabled">${isRemoteDevice(source) && isRemoteDevice(sink) ? '暂不支持' : '禁用'}</td>`;
      return `<td>${matrixCheckbox(choice)}</td>`;
    }).join('');
    return `<tr><th>${escapeHtml(source.name)}${isRemoteDevice(source) ? ' <small>网络</small>' : ''}</th>${cells}</tr>`;
  }).join('');
  const sourceOptions = sources.map(source => `<md-select-option value="${escapeHtml(matrixDeviceKey(source))}"${matrixDeviceKey(source) === mobileMatrixSource ? ' selected' : ''}><div slot="headline">${escapeHtml(source.name)}${isRemoteDevice(source) ? ' · 网络' : ''}</div></md-select-option>`).join('');
  const targets = activeSource ? sinks.map(sink => {
    const choice = matrixChoice(activeSource, sink);
    const origin = isRemoteDevice(sink) ? '网络' : '本机';
    return `<label class="matrix-target${choice ? '' : ' disabled'}">${choice ? matrixCheckbox(choice) : '<span class="matrix-disabled-mark">—</span>'}<span class="matrix-target-name">${escapeHtml(sink.name)}</span><span class="matrix-origin">${origin}</span></label>`;
  }).join('') : '<div class="empty">正在加载设备…</div>';
  byId<HTMLElement>('matrix').innerHTML = `<div class="matrix-toolbar" role="group" aria-label="${uiLabel('矩阵显示方式', 'Matrix display mode')}"><button type="button" data-matrix-view="builder">路线选择</button><button type="button" data-matrix-view="matrix">矩阵视图</button></div><div class="matrix-builder"><p class="matrix-help">${uiLabel('可将多个来源勾选到同一播放目标，OAMR 会在该目标混音。', 'Select multiple sources for one playback target to mix them together.')}</p><md-outlined-select id="matrixMobileSource" label="音频来源" clamp-menu-width>${sourceOptions}</md-outlined-select><div class="matrix-target-heading">播放目标</div><div class="matrix-targets">${targets}</div><div class="matrix-selection-summary" aria-live="polite">${matrixSummaryHtml()}</div></div><div class="matrix-desktop data-table-wrap"><table class="data-table matrix-table"><thead><tr><th>来源 \ 播放目标</th>${sinks.map(sink => `<th>${escapeHtml(sink.name)}${isRemoteDevice(sink) ? ' <small>网络</small>' : ''}</th>`).join('')}</tr></thead><tbody>${rows}</tbody></table></div>`;
  wireMatrixControls();
  applyPreferences();
}

function renderRoutes(routes: Route[]): void {
  const rows = routes.map(route => {
    const deleteClass = deleteConfirmation === route.id ? 'danger-action' : '';
    const configuration = route.network ? iconButton(`data-config="${route.id}"`, uiLabel('配置路线', 'Configure route'), icons.edit) : '';
    const toggle = iconButton(`data-toggle="${route.id}" data-enabled="${!route.enabled}"`, route.enabled ? uiLabel('暂停路线', 'Pause route') : uiLabel('恢复路线', 'Resume route'), route.enabled ? icons.pause : icons.play);
    const deletion = iconButton(`data-delete="${route.id}"`, deleteConfirmation === route.id ? uiLabel('确认删除路线', 'Confirm route deletion') : uiLabel('删除路线', 'Delete route'), icons.delete, deleteClass);
    return `<tr><td>${escapeHtml(route.label)}</td><td>${route.enabled ? '运行中' : '已暂停'}</td><td>${route.network ? `${route.quality} · ${route.latency} ms · ${route.mode}` : '本地路线'}</td><td><div class="route-actions">${toggle}${configuration}${deletion}</div></td></tr>`;
  }).join('');
  byId<HTMLElement>('routeTable').innerHTML = routes.length ? `<div class="data-table-wrap"><table class="data-table"><thead><tr><th>路线</th><th>状态</th><th>传输属性</th><th>操作</th></tr></thead><tbody>${rows}</tbody></table></div>` : '<div class="route-table-placeholder empty">暂无路线。</div>';
  document.querySelectorAll<HTMLElement>('[data-toggle]').forEach(button => button.addEventListener('click', () => void post(`/api/routes/${button.dataset.toggle}/toggle?enabled=${button.dataset.enabled}`)));
  document.querySelectorAll<HTMLElement>('[data-config]').forEach(button => button.addEventListener('click', () => openRouteDialog(routes.find(route => route.id === Number(button.dataset.config))!)));
  document.querySelectorAll<HTMLElement>('[data-delete]').forEach(button => button.addEventListener('click', () => {
    const id = Number(button.dataset.delete);
    if (deleteConfirmation !== id) { deleteConfirmation = id; renderRoutes(routes); return; }
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
    const telemetry = peerTelemetryText(peer);
    const endpoints = peer.endpoints.length ? peer.endpoints.map(endpoint => `<md-assist-chip class="endpoint-chip" label="${endpoint.direction === 'source' ? '来源' : '输出'} · ${escapeHtml(endpoint.name)}"></md-assist-chip>`).join('') : '<span class="muted">对方未开放设备</span>';
    const edit = iconButton(`data-edit-peer="${escapeHtml(peer.nodeId)}"`, uiLabel('编辑名称和地址', 'Edit name and address'), icons.edit);
    const removal = iconButton(`data-delete-peer="${escapeHtml(peer.nodeId)}"`, peerDeleteConfirmation === peer.nodeId ? uiLabel('确认删除已配对设备', 'Confirm paired device deletion') : uiLabel('删除已配对设备', 'Delete paired device'), icons.delete, peerDeleteConfirmation === peer.nodeId ? 'danger-action' : '');
    return `<article class="peer-card"><div class="peer-title"><strong>${escapeHtml(peer.alias)}</strong><span class="peer-actions">${edit}${removal}</span></div><div class="muted">${escapeHtml(peer.host)}:8791</div><div class="muted peer-telemetry" data-telemetry-node="${escapeHtml(peer.nodeId)}">${telemetry}</div><div>${endpoints}</div></article>`;
  }).join('') : '<div class="peer-list-placeholder empty">尚未配对设备。</div>';
  document.querySelectorAll<HTMLElement>('[data-edit-peer]').forEach(button => button.addEventListener('click', () => openPeerDialog(button.dataset.editPeer!)));
  document.querySelectorAll<HTMLElement>('[data-delete-peer]').forEach(button => button.addEventListener('click', () => void (async () => {
    const nodeId = button.dataset.deletePeer!;
    if (peerDeleteConfirmation !== nodeId) { peerDeleteConfirmation = nodeId; renderPeers(); return; }
    peerDeleteConfirmation = undefined;
    await post(`/api/pair/delete?node=${encodeURIComponent(nodeId)}`);
    await refreshPeers();
  })()));
  applyPreferences();
}

function peerTelemetryText(peer: Peer): string {
  const loss = peer.telemetry.packetLossPercent < 0 ? '—' : `${peer.telemetry.packetLossPercent.toFixed(2)}%`;
  return peer.telemetry.deviceName
    ? `音质 ${peer.telemetry.quality} · 目标延迟 ${peer.telemetry.latencyMs} ms · 丢包 ${loss} · 设备 ${peer.telemetry.deviceName}`
    : '当前没有传输音频';
}

function updatePeerTelemetry(): void {
  const telemetryByNode = new Map(peers.map(peer => [peer.nodeId, peerTelemetryText(peer)]));
  document.querySelectorAll<HTMLElement>('[data-telemetry-node]').forEach(element => {
    const next = telemetryByNode.get(element.dataset.telemetryNode ?? '');
    if (next !== undefined && element.textContent !== next) element.textContent = next;
  });
}

function peerTopologyOf(items: Peer[]): string {
  return JSON.stringify(items.map(peer => ({
    nodeId: peer.nodeId,
    alias: peer.alias,
    host: peer.host,
    endpoints: peer.endpoints.map(endpoint => [endpoint.id, endpoint.name, endpoint.direction])
  })));
}

function openPeerDialog(nodeId: string): void {
  const peer = peers.find(item => item.nodeId === nodeId);
  if (!peer) return;
  byId<ValueElement>('editAlias').value = peer.alias;
  byId<ValueElement>('editHost').value = peer.host;
  byId<HTMLElement & { show: () => void }>('peerDialog').show();
  byId<HTMLElement>('savePeer').onclick = () => void (async () => {
    await post(`/api/pair/alias?node=${encodeURIComponent(nodeId)}&alias=${encodeURIComponent(value('editAlias'))}`);
    await post(`/api/pair/endpoint?node=${encodeURIComponent(nodeId)}&host=${encodeURIComponent(value('editHost'))}&port=8791`);
    byId<HTMLElement & { close: () => void }>('peerDialog').close();
    await refreshPeers();
  })();
}

async function refreshPeers(): Promise<void> {
  const nextPeers = JSON.parse(await request('/api/pair/peers')) as Peer[];
  const nextTopology = peerTopologyOf(nextPeers);
  const topologyChanged = nextTopology !== peerTopologyFingerprint;
  peers = nextPeers;
  if (topologyChanged) {
    peerTopologyFingerprint = nextTopology;
    renderPeers();
    renderMatrix();
  } else {
    updatePeerTelemetry();
  }
}

async function refreshRoutes(): Promise<void> {
  const nextRoutes = JSON.parse(await request('/api/routes')) as Route[];
  const nextFingerprint = JSON.stringify(nextRoutes);
  if (nextFingerprint === routeFingerprint) return;
  routeFingerprint = nextFingerprint;
  deleteConfirmation = undefined;
  renderRoutes(nextRoutes);
}

async function applyMatrix(): Promise<void> {
  const selected = [...matrixSelections].map(key => matrixChoices.get(key)).filter((choice): choice is MatrixChoice => choice !== undefined);
  if (!selected.length) { logEvent('Select at least one matrix route.', 'WARNING'); return; }
  const local = selected.filter(item => item.source !== undefined);
  const remote = selected.filter(item => item.kind !== undefined);
  if (local.length) {
    const routes = local.map(item => `${item.source}\t${item.loopback}\t${item.sink}`).join('\n');
    await post(`/api/matrix?routes=${encodeURIComponent(routes)}`);
  }
  const mixerGroups = new Map<string, MatrixChoice[]>();
  for (const item of remote.filter(item => item.kind === 'receive')) {
    const group = mixerGroups.get(item.local!) ?? [];
    group.push(item);
    mixerGroups.set(item.local!, group);
  }
  const mixed = new Set<string>();
  for (const group of mixerGroups.values()) {
    if (group.length < 2) continue;
    const rows = group.map(item => `${item.peer}\t${item.remote}\t${item.local}`).join('\n');
    await post(`/api/paired/mixer?routes=${encodeURIComponent(rows)}&quality=medium&max-latency-ms=100&mode=auto`);
    for (const item of group) mixed.add(item.key);
  }
  for (const item of remote.filter(item => !mixed.has(item.key))) {
    const query = new URLSearchParams({ peer: item.peer!, kind: item.kind!, local: item.local!, remote: item.remote!, loopback: String(item.loopback), quality: 'medium', 'max-latency-ms': '100', mode: 'auto' });
    await post(`/api/paired/route?${query}`);
  }
  await refreshPeers();
  matrixSelections.clear();
  renderMatrix();
}

function wireEvents(): void {
  byId<HTMLElement>('stopAll').addEventListener('click', () => void post('/api/stop'));
  byId<HTMLElement>('createSender').addEventListener('click', () => {
    const source = devices.sources.find(item => item.id === value('networkSource'));
    if (!source) { logEvent('Select an audio source before creating a sender route.', 'WARNING'); return; }
    const query = new URLSearchParams({ source: source.id, loopback: String(source.renderLoopback === true), host: value('sendHost'), port: value('sendPort'), quality: 'medium', 'max-latency-ms': '100', mode: 'auto' });
    void post(`/api/network/send?${query}`);
  });
  byId<HTMLElement>('createReceiver').addEventListener('click', () => {
    const query = new URLSearchParams({ sink: value('receiveSink'), port: value('receivePort'), quality: 'medium', 'max-latency-ms': '100', mode: 'auto' });
    void post(`/api/network/receive?${query}`);
  });
  byId<HTMLElement>('applyMatrix').addEventListener('click', () => void applyMatrix());
  byId<HTMLElement>('newCode').addEventListener('click', () => void refreshPairCode(true).catch(error => logEvent(`Could not regenerate pairing code: ${String(error)}`, 'ERROR')));
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
  window.matchMedia('(max-width: 719px)').addEventListener('change', updateMatrixViewState);
  wireEvents();
  enhancePanels();
  applyPreferences();
  void refreshPairCode().catch(error => logEvent(`Could not load pairing code: ${String(error)}`, 'ERROR'));
  void refreshServerLogs().catch(error => logEvent(`Could not load diagnostics: ${String(error)}`, 'ERROR'));
  try {
    devices = JSON.parse(await request('/api/devices')) as Devices;
    const local = JSON.parse(await request('/api/pair/local')) as { alias: string };
    localAlias = local.alias;
    renderDeviceControls();
    await Promise.all([refreshPeers(), refreshRoutes(), refreshDiscovery()]);
    window.setInterval(() => { void refreshPairCode().catch(() => undefined); void refreshPeers().catch(() => undefined); void refreshRoutes().catch(() => undefined); void refreshDiscovery().catch(() => undefined); void refreshServerLogs().catch(() => undefined); }, 3000);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    logEvent(`Startup failed: ${message}`, 'ERROR');
  }
}

void start();
