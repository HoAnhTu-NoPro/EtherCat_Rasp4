'use strict';

// Trình duyệt chỉ gửi ý định. Mọi giới hạn an toàn nằm trong ecat-rt:
// jog phải được gửi lại mỗi 50 ms, quá 200 ms không nhận được thì trục tự dừng.

const JOG_PERIOD_MS = 50;
const WS_STALE_MS = 1000;

const $ = (id) => document.getElementById(id);
const AX_ENABLED = 5, AX_FAULT = 8, AX_FAULT_REACTION = 7;

let ws = null;
let wsOpen = false;
let lastMsgAt = 0;
let reconnectDelay = 500;
let status = null;
let role = {controller: false};
let jogTimer = null;
let jogDir = 0;
let jogBtn = null;

// ------------------------------------------------------------ WebSocket

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => { wsOpen = true; reconnectDelay = 500; lastMsgAt = Date.now(); render(); };
  ws.onmessage = (ev) => {
    lastMsgAt = Date.now();
    const m = JSON.parse(ev.data);
    if (m.t === 'status') { status = m; render(); }
    else if (m.t === 'role') { role = m; render(); }
    else if (m.t === 'msg') toast(m.text);
  };
  ws.onclose = (ev) => {
    wsOpen = false;
    endJog();
    status = null;
    role = {controller: false};
    render();
    if (ev.code === 4401) { location.href = '/login'; return; }
    setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 5000);
  };
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) { ws.send(JSON.stringify(obj)); return true; }
  return false;
}

// Mất bản tin trạng thái quá lâu thì coi như mất kết nối, kể cả khi socket chưa báo đóng.
setInterval(() => {
  if (wsOpen && Date.now() - lastMsgAt > WS_STALE_MS) { endJog(); render(); }
}, 250);

function wsHealthy() { return wsOpen && Date.now() - lastMsgAt <= WS_STALE_MS; }

// ------------------------------------------------------------ Jog giữ để chạy

function canJog() {
  return canControl() && status.axis && status.axis.state_id === AX_ENABLED &&
         status.axis.enable_req && !status.axis.stop_latched;
}

function startJog(dir, btn) {
  if (!canJog() || jogTimer) return;
  jogDir = dir;
  jogBtn = btn;
  btn.classList.add('held');
  sendJog();
  jogTimer = setInterval(sendJog, JOG_PERIOD_MS);
}

function sendJog() {
  if (!canJog()) { endJog(); return; }
  send({t: 'jog', v: jogDir * speedValue()});
}

function endJog() {
  if (!jogTimer) return;
  clearInterval(jogTimer);
  jogTimer = null;
  if (jogBtn) jogBtn.classList.remove('held');
  jogBtn = null;
  send({t: 'jog', v: 0}); // nhả nút: giảm tốc bình thường
}

function bindJog(btn, dir) {
  btn.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    btn.setPointerCapture(e.pointerId);
    startJog(dir, btn);
  });
  for (const ev of ['pointerup', 'pointercancel', 'lostpointercapture']) btn.addEventListener(ev, endJog);
  btn.addEventListener('contextmenu', (e) => e.preventDefault());
}

bindJog($('jog-neg'), -1);
bindJog($('jog-pos'), +1);
window.addEventListener('blur', endJog);
document.addEventListener('visibilitychange', () => { if (document.hidden) endJog(); });

// ------------------------------------------------------------ Nút

function stop() {
  endJog();
  if (!send({t: 'stop'})) toast('Mất kết nối, STOP không gửi được! Dùng nút dừng khẩn cấp phần cứng.');
}

$('stop-btn').addEventListener('click', stop);
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') stop(); });

$('enable').addEventListener('click', () => send({t: 'enable'}));
$('disable').addEventListener('click', () => send({t: 'disable'}));
$('reset').addEventListener('click', () => send({t: 'reset'}));
$('release').addEventListener('click', () => { endJog(); send({t: 'release'}); });
$('take').addEventListener('click', () => {
  if (role.controller_label && !role.controller) {
    if (!confirm(`${role.controller_label} đang điều khiển. Lấy quyền? Trục sẽ được STOP.`)) return;
    send({t: 'take', force: true});
  } else {
    send({t: 'take'});
  }
});
$('logout').addEventListener('click', async () => {
  endJog();
  await fetch('/api/logout', {method: 'POST'});
  location.href = '/login';
});

const speed = $('speed');
try { const v = localStorage.getItem('jogSpeed'); if (v) speed.value = v; } catch {}
speed.addEventListener('input', () => {
  try { localStorage.setItem('jogSpeed', speed.value); } catch {}
  render();
});
function speedValue() { return Number(speed.value); }

// ------------------------------------------------------------ Hiển thị

function canControl() {
  return wsHealthy() && role.controller && status && status.alive &&
         status.master && status.master.state === 'RUNNING';
}

const fmt = (v, d = 1) => (v === null || v === undefined || Number.isNaN(v)) ? '—' : Number(v).toFixed(d);
const hex4 = (v) => '0x' + v.toString(16).toUpperCase().padStart(4, '0');
const hex8 = (v) => '0x' + v.toString(16).toUpperCase().padStart(8, '0');

function setPill(el, cls, text) { el.className = 'pill ' + cls; el.textContent = text; }

function render() {
  const healthy = wsHealthy();
  const alive = healthy && status && status.alive;
  const m = status && status.master;
  const a = status && status.axis;
  const lim = status && status.limits;

  // Pill và banner kết nối
  setPill($('p-ws'), healthy ? 'ok' : 'bad', healthy ? 'Web' : 'Web mất kết nối');
  setPill($('p-rt'), alive ? 'ok' : 'bad', alive ? 'ecat-rt' : 'ecat-rt mất');
  const link = status && status.link;
  const linkUp = link && link.carrier === '1';
  setPill($('p-link'), !link ? '' : linkUp ? 'ok' : 'bad',
          link ? `${link.ifname} ${linkUp ? (link.speed || '?') + ' Mb/s' : 'mất link'}` : 'eth0');

  let bt = '', bd = '';
  if (!healthy) { bt = 'MẤT KẾT NỐI VỚI WEB SERVER'; bd = 'Đang kết nối lại… Trục sẽ tự dừng nếu đang jog.'; }
  else if (!status.alive) { bt = 'MẤT KẾT NỐI VỚI ecat-rt'; bd = (status.reason || '') + ' — không điều khiển được trục.'; }
  else if (m && m.state === 'BUS_LOST') { bt = 'MẤT BUS EtherCAT'; bd = 'Trục đã bị disable. Kiểm tra cáp/nguồn driver.'; }
  else if (m && m.state === 'STARTING') { bt = 'ecat-rt đang khởi động'; bd = 'Chờ slave lên OP…'; }
  $('banner').className = bt ? 'show' : '';
  $('banner-title').textContent = bt;
  $('banner-detail').textContent = bd;
  for (const id of ['sec-axis', 'sec-master', 'sec-slaves']) $(id).classList.toggle('stale', !alive);

  // Quyền điều khiển
  if (!healthy) $('role-text').textContent = 'Chưa kết nối';
  else if (role.controller) $('role-text').textContent = `Bạn đang điều khiển (${role.you})`;
  else if (role.controller_label) $('role-text').textContent = `Chỉ xem — ${role.controller_label} đang điều khiển`;
  else $('role-text').textContent = 'Chỉ xem — chưa ai điều khiển';
  $('take').hidden = role.controller;
  $('take').disabled = !healthy;
  $('take').textContent = role.controller_label && !role.controller ? 'Lấy quyền điều khiển' : 'Nhận quyền điều khiển';
  $('release').hidden = !role.controller;

  const ctl = canControl();
  const enabled = a && a.state_id === AX_ENABLED;
  const fault = a && (a.state_id === AX_FAULT || a.state_id === AX_FAULT_REACTION);
  $('enable').disabled = !ctl || fault || (a && a.enable_req);
  $('disable').disabled = !ctl || !(a && (a.enable_req || enabled));
  $('reset').disabled = !ctl || !fault;
  $('jog-neg').disabled = $('jog-pos').disabled = !canJog() && !jogTimer;
  speed.disabled = !ctl;
  $('stop-btn').disabled = false; // luôn bấm được; nếu mất kết nối sẽ báo

  if (lim) {
    const max = Math.max(1, Math.floor(lim.max_jog_rpm));
    if (Number(speed.max) !== max) { speed.max = max; if (speedValue() > max) speed.value = max; }
  }
  $('speed-out').textContent = `${speedValue()} rpm`;

  // Ghi chú trạng thái
  const notes = [];
  if (a && a.stop_latched) notes.push('STOP đang chốt: nhả nút jog để jog lại.');
  if (a && a.hb_expired) notes.push('Lần jog trước dừng do mất heartbeat.');
  if (a && a.enable_timeout) notes.push('Enable thất bại (driver không vào trạng thái Enabled trong 1 s).');
  if (a && a.stopping) notes.push('Đang giảm tốc dừng…');
  if (role.controller && a && !enabled && !fault) notes.push('Bấm Enable để cấp điện motor trước khi jog.');
  if (fault) notes.push('Driver đang Fault: xử lý nguyên nhân rồi bấm Fault Reset.');
  $('notes').replaceChildren(...notes.map((t) => { const d = document.createElement('div'); d.textContent = t; return d; }));

  if (!status || !m) return;

  // Trục
  const st = $('ax-state');
  st.textContent = a.state;
  st.className = 'big-state ' + (fault ? 'bad' : enabled ? 'ok' : 'warn');
  $('ax-bits').replaceChildren(...a.status_bits.map((b) => { const s = document.createElement('span'); s.textContent = b; return s; }));
  $('ax-sw').textContent = hex4(a.statusword);
  $('ax-cw').textContent = hex4(a.controlword);
  $('ax-mode').textContent = a.mode;
  $('ax-pos').textContent = a.position.toLocaleString('vi-VN') + ' xung';
  $('ax-vel').textContent = fmt(a.actual_rpm) + ' rpm';
  $('ax-prof').textContent = `${fmt(a.profile_rpm)} → ${fmt(a.target_rpm)} rpm`;
  $('ax-err').textContent = a.error_code ? hex4(a.error_code) : '0 (không lỗi)';
  $('ax-errbox').className = 'error-box' + (a.error_code ? ' show' : '');
  $('ax-errbox').textContent = a.error_code ? `${hex4(a.error_code)}: ${a.error_text}` : '';

  // Master
  const ms = $('ms-state');
  ms.textContent = m.state;
  ms.className = 'big-state ' + (m.state === 'RUNNING' ? 'ok' : m.state === 'BUS_LOST' ? 'bad' : 'warn');
  $('ms-cycle').textContent = `${fmt(m.cycle_us, 0)} µs`;
  $('ms-avg').textContent = `${fmt(m.period_avg_us)} µs`;
  $('ms-minmax').textContent = `${fmt(m.period_min_us)} / ${fmt(m.period_max_us)} µs`;
  $('ms-lat').textContent = `${fmt(m.lat_max_us)} µs`;
  $('ms-lat-total').textContent = `${fmt(m.lat_max_total_us)} µs (>100 µs: ${m.lat_over_100us})`;
  $('ms-dc').textContent = `${fmt(m.dc_error_us)} µs`;
  $('ms-wkc').textContent = `${m.wkc} / ${m.expected_wkc}`;
  $('ms-wkc-err').textContent = String(m.wkc_errors);
  $('ms-link').textContent = link ? `${link.ifname}: ${link.operstate}, ${linkUp ? (link.speed + ' Mb/s ' + (link.duplex || '')) : 'không có carrier'}` : '—';

  // Slave
  const rows = status.slaves.map((s, i) => {
    const tr = document.createElement('tr');
    const cells = [String(i + 1), s.name, `${hex8(s.vendor)} / ${hex8(s.product)}`,
                   s.state_text + (s.al_text ? ` (AL ${s.al_text})` : '')];
    for (const c of cells) { const td = document.createElement('td'); td.textContent = c; tr.appendChild(td); }
    tr.lastChild.className = 'mono';
    return tr;
  });
  $('slaves').replaceChildren(...rows);

  $('lim-v').textContent = `${fmt(lim.max_jog_rpm, 0)} rpm`;
  $('lim-a').textContent = `${fmt(lim.accel_rpm_s, 0)} / ${fmt(lim.stop_decel_rpm_s, 0)} rpm/s`;
  $('lim-hb').textContent = `${lim.heartbeat_timeout_ms} ms`;
}

let toastTimer = null;
function toast(text) {
  const t = $('toast');
  t.textContent = text;
  t.className = 'show';
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = ''; }, 3500);
}

render();
connect();
