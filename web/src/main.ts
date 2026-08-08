import '@material/web/button/filled-button.js';
import '@material/web/button/outlined-button.js';
import '@material/web/button/text-button.js';
import '@material/web/checkbox/checkbox.js';
import '@material/web/chips/assist-chip.js';
import '@material/web/dialog/dialog.js';
import '@material/web/divider/divider.js';
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

const app = document.querySelector<HTMLDivElement>('#app')!;
let devices: Devices = { sources: [], sinks: [] };
let peers: Peer[] = [];

const escapeHtml = (value: unknown) => String(value).replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[char]!);
const byId = <T extends HTMLElement>(id: string) => document.querySelector<T>(`#${id}`)!;
const value = (id: string) => (byId<ValueElement>(id).value ?? '').trim();
const checked = (selector: string) => Array.from(document.querySelectorAll<Checkable>(selector)).filter(item => item.checked);

function setStatus(message: string): void {
  byId<HTMLElement>('status').textContent = message;
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
    await refreshRoutes();
    return message;
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    setStatus(`操作失败：${message}`);
    throw error;
  }
}

function selectOptions(items: Device[]): string {
  return items.map(item => `<md-select-option value="${escapeHtml(item.id)}"><div slot="headline">${escapeHtml(item.name)}</div></md-select-option>`).join('');
}

function renderShell(): void {
  app.innerHTML = `
    <main class="app-shell">
      <header class="app-header">
        <div><h1>Open Audio Matrix Router</h1><p>本机音频矩阵 · 配对设备 · RTP/Opus 局域网传输</p></div>
        <md-assist-chip class="status-chip" label="仅本机 Web UI"></md-assist-chip>
      </header>
      <div class="grid">
        <section class="card wide"><h2>路由表</h2><p class="muted">每条路线独立运行。网络路线的音质、最大延迟和模式在这里修改。</p><div id="routeTable" class="empty">正在加载路线…</div><div class="actions"><md-outlined-button id="stopAll">停止并清空所有路由</md-outlined-button></div></section>

        <section class="card"><h2>发送到局域网</h2><p class="muted">为未配对设备手工创建 RTP 发送路线。</p><div class="stack"><md-outlined-select id="networkSource" label="音频来源"></md-outlined-select><div class="form-grid"><md-outlined-text-field id="sendHost" label="IP / 主机名" value="127.0.0.1"></md-outlined-text-field><md-outlined-text-field id="sendPort" label="UDP 端口" type="number" value="5004"></md-outlined-text-field></div><md-filled-button id="createSender">创建发送路线</md-filled-button></div></section>
        <section class="card"><h2>从局域网接收</h2><p class="muted">为未配对设备手工创建 RTP 接收路线。</p><div class="stack"><md-outlined-select id="receiveSink" label="播放到"></md-outlined-select><md-outlined-text-field id="receivePort" label="UDP 端口" type="number" value="5004"></md-outlined-text-field><md-filled-button id="createReceiver">创建接收路线</md-filled-button></div></section>

        <section class="card wide"><h2>音频矩阵</h2><p class="muted">勾选来源与播放目标的交叉点即可创建路线。本机、已配对设备都会列在这里；网络路线的属性在路由表中设置。</p><div id="matrix" class="data-table-wrap"><div class="empty">正在加载设备…</div></div><div class="actions"><md-filled-button id="applyMatrix">添加已勾选的路线</md-filled-button></div></section>

        <section class="card wide"><h2>设备配对</h2><p class="muted">网页仅监听本机。配对控制使用 TCP 8791，一次性代码十分钟有效且仅能使用一次。</p><div class="form-grid"><md-outlined-text-field id="localAlias" label="本机别名"></md-outlined-text-field><md-outlined-text-field label="配对控制端口" value="8791" disabled></md-outlined-text-field></div><p class="muted">允许已配对设备看到的本机端点</p><div id="exposure" class="exposure-list"></div><div class="actions"><md-outlined-button id="saveProfile">保存开放设备</md-outlined-button></div><md-divider></md-divider><div class="form-grid"><div class="stack"><md-outlined-text-field id="pairCode" label="一次性配对代码" readonly></md-outlined-text-field><md-outlined-button id="newCode">生成新代码</md-outlined-button></div><div class="stack"><md-outlined-text-field id="peerHost" label="另一台设备的 IP / 主机名" placeholder="192.168.31.100"></md-outlined-text-field><div class="form-grid"><md-outlined-text-field id="peerPort" label="TCP 端口" type="number" value="8791"></md-outlined-text-field><md-outlined-text-field id="peerAlias" label="别名" placeholder="客厅电脑"></md-outlined-text-field></div><md-outlined-text-field id="peerCode" label="对方的一次性代码"></md-outlined-text-field><md-filled-button id="pairRemote">开始配对</md-filled-button></div></div></section>

        <section class="card wide"><h2>已配对设备与传输遥测</h2><p class="muted">保存开放设备、修改别名、启动或停止网络流后会同步；此处每五秒自动刷新。</p><div id="peerList" class="empty">尚未配对设备。</div></section>
        <section class="card wide"><h2>运行日志</h2><pre id="status" class="log">正在加载设备…</pre></section>
      </div>
    </main>
    <md-dialog id="peerDialog"><div slot="headline">编辑已配对设备</div><form id="peerForm" slot="content" class="dialog-form" method="dialog"><md-outlined-text-field id="editAlias" label="别名"></md-outlined-text-field><md-outlined-text-field id="editHost" label="IP / 主机名"></md-outlined-text-field><md-outlined-text-field id="editPort" label="TCP 端口" type="number"></md-outlined-text-field></form><div slot="actions"><md-text-button id="cancelPeer">取消</md-text-button><md-filled-button id="savePeer">保存</md-filled-button></div></md-dialog>`;
}

function renderDeviceControls(): void {
  byId<HTMLElement>('networkSource').innerHTML = selectOptions(devices.sources);
  byId<HTMLElement>('receiveSink').innerHTML = selectOptions(devices.sinks);
  byId<HTMLElement>('exposure').innerHTML = [
    ...devices.sources.map(device => `<label class="exposure-row"><md-checkbox data-direction="source" data-id="${escapeHtml(device.id)}" data-name="${escapeHtml(device.name)}"></md-checkbox><span>来源 · ${escapeHtml(device.name)}</span></label>`),
    ...devices.sinks.map(device => `<label class="exposure-row"><md-checkbox data-direction="sink" data-id="${escapeHtml(device.id)}" data-name="${escapeHtml(device.name)}"></md-checkbox><span>输出 · ${escapeHtml(device.name)}</span></label>`)
  ].join('');
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
}

function profileSelect(id: string, current: string | number | undefined, options: Array<[string, string]>): string {
  return `<md-outlined-select id="${id}" class="route-property">${options.map(([key, label]) => `<md-select-option value="${key}" ${String(current) === key ? 'selected' : ''}><div slot="headline">${label}</div></md-select-option>`).join('')}</md-outlined-select>`;
}

function renderRoutes(routes: Route[]): void {
  const content = routes.length ? `<div class="data-table-wrap"><table class="data-table"><thead><tr><th>路线</th><th>状态</th><th>网络属性</th><th>操作</th></tr></thead><tbody>${routes.map(route => `<tr><td>${escapeHtml(route.label)}</td><td>${route.enabled ? '运行中' : '已暂停'}</td><td>${route.network ? `<div class="route-actions">${profileSelect(`quality-${route.id}`, route.quality, [['low', '低音质'], ['medium', '中音质'], ['high', '高音质']])}${profileSelect(`latency-${route.id}`, route.latency, [['40', '40 ms'], ['60', '60 ms'], ['100', '100 ms'], ['150', '150 ms']])}${profileSelect(`mode-${route.id}`, route.mode, [['stable', '稳定'], ['auto', '自动'], ['low-latency', '低延迟']])}<md-text-button data-profile="${route.id}">应用</md-text-button></div>` : '本地路线'}</td><td><div class="route-actions"><md-text-button data-toggle="${route.id}" data-enabled="${!route.enabled}">${route.enabled ? '暂停' : '恢复'}</md-text-button><md-text-button data-delete="${route.id}">删除</md-text-button></div></td></tr>`).join('')}</tbody></table></div>` : '<div class="empty">暂无路线。</div>';
  byId<HTMLElement>('routeTable').innerHTML = content;
  document.querySelectorAll<HTMLElement>('[data-toggle]').forEach(button => button.addEventListener('click', () => void post(`/api/routes/${button.dataset.toggle}/toggle?enabled=${button.dataset.enabled}`)));
  document.querySelectorAll<HTMLElement>('[data-delete]').forEach(button => button.addEventListener('click', () => void post(`/api/routes/${button.dataset.delete}/delete`)));
  document.querySelectorAll<HTMLElement>('[data-profile]').forEach(button => button.addEventListener('click', () => {
    const id = button.dataset.profile!;
    void post(`/api/routes/${id}/profile?quality=${encodeURIComponent(value(`quality-${id}`))}&max-latency-ms=${encodeURIComponent(value(`latency-${id}`))}&mode=${encodeURIComponent(value(`mode-${id}`))}`);
  }));
}

function renderPeers(): void {
  byId<HTMLElement>('peerList').innerHTML = peers.length ? peers.map(peer => {
    const telemetry = peer.telemetry.deviceName ? `音质 ${escapeHtml(peer.telemetry.quality)} · 目标延迟 ${peer.telemetry.latencyMs} ms · 丢包 ${peer.telemetry.packetLossPercent}% · 设备 ${escapeHtml(peer.telemetry.deviceName)}` : '当前没有传输音频';
    const endpoints = peer.endpoints.length ? peer.endpoints.map(endpoint => `<md-assist-chip class="endpoint-chip" label="${endpoint.direction === 'source' ? '来源' : '输出'} · ${escapeHtml(endpoint.name)}"></md-assist-chip>`).join('') : '<span class="muted">对方未开放设备</span>';
    return `<article class="peer-card"><div class="peer-title"><strong>${escapeHtml(peer.alias)}</strong><md-text-button data-edit-peer="${escapeHtml(peer.nodeId)}">编辑</md-text-button></div><div class="muted">${escapeHtml(peer.host)}:${peer.port}</div><div class="muted">${telemetry}</div><div>${endpoints}</div></article>`;
  }).join('') : '<div class="empty">尚未配对设备。</div>';
  document.querySelectorAll<HTMLElement>('[data-edit-peer]').forEach(button => button.addEventListener('click', () => openPeerDialog(button.dataset.editPeer!)));
}

function openPeerDialog(nodeId: string): void {
  const peer = peers.find(item => item.nodeId === nodeId);
  if (!peer) return;
  byId<ValueElement>('editAlias').value = peer.alias;
  byId<ValueElement>('editHost').value = peer.host;
  byId<ValueElement>('editPort').value = String(peer.port);
  byId<HTMLElement & { show: () => void }>('peerDialog').show();
  byId<HTMLElement>('savePeer').onclick = () => void (async () => {
    await post(`/api/pair/alias?node=${encodeURIComponent(nodeId)}&alias=${encodeURIComponent(value('editAlias'))}`);
    await post(`/api/pair/endpoint?node=${encodeURIComponent(nodeId)}&host=${encodeURIComponent(value('editHost'))}&port=${encodeURIComponent(value('editPort'))}`);
    byId<HTMLElement & { close: () => void }>('peerDialog').close();
    await refreshPeers();
  })();
}

async function refreshPeers(): Promise<void> {
  peers = JSON.parse(await request('/api/pair/peers')) as Peer[];
  renderPeers();
  renderMatrix();
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
    await post(`/api/pair/config?alias=${encodeURIComponent(value('localAlias') || 'This computer')}&endpoints=${encodeURIComponent(endpoints)}`);
    await refreshPeers();
  })());
  byId<HTMLElement>('pairRemote').addEventListener('click', () => void (async () => {
    const query = new URLSearchParams({ host: value('peerHost'), port: value('peerPort'), alias: value('peerAlias') || value('peerHost'), code: value('peerCode') });
    const message = await post(`/api/pair/connect?${query}`);
    if (message.startsWith('Pairing succeeded')) await refreshPeers();
  })());
  byId<HTMLElement>('cancelPeer').addEventListener('click', () => byId<HTMLElement & { close: () => void }>('peerDialog').close());
}

async function start(): Promise<void> {
  renderShell();
  wireEvents();
  try {
    devices = JSON.parse(await request('/api/devices')) as Devices;
    const local = JSON.parse(await request('/api/pair/local')) as { alias: string };
    byId<ValueElement>('localAlias').value = local.alias;
    renderDeviceControls();
    await Promise.all([refreshPeers(), refreshRoutes()]);
    setStatus('设备已就绪。');
    window.setInterval(() => { void refreshPeers().catch(() => undefined); void refreshRoutes().catch(() => undefined); }, 5000);
  } catch (error) {
    setStatus(`加载失败：${error instanceof Error ? error.message : String(error)}`);
  }
}

void start();
