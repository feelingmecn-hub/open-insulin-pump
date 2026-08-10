// 泵 Web 调试前端逻辑
const $ = (id) => document.getElementById(id);
let gConnected = false;   // 连接状态（虚拟按键按下/抬起配对时用于防误触未连接）

async function api(path, body) {
  try {
    const opt = { method: body ? 'POST' : 'GET', headers: { 'Content-Type': 'application/json' } };
    if (body) opt.body = JSON.stringify(body);
    const r = await fetch(path, opt);
    return await r.json();
  } catch (e) {
    return { ok: false, msg: '网络错误: ' + e.message };
  }
}

function log(msg, ok = true) {
  const el = $('log');
  const t = new Date().toLocaleTimeString('zh-CN', { hour12: false });
  const line = document.createElement('div');
  line.textContent = `[${t}] ${ok ? '✓' : '✗'} ${msg}`;
  line.style.color = ok ? '#cfe6ff' : '#ffb3b3';
  el.prepend(line);
  while (el.childElementCount > 80) el.removeChild(el.lastChild);
}

// ---------- 诊断信息展示 ----------
function logDiag(diag) {
  if (!diag) return;
  if (diag.scan) {
    log(`扫描到 ${diag.scan.length} 个设备：`, true);
    diag.scan.forEach(d => {
      const svc = (d.svc || []).join(',') || '—';
      log(`  · ${d.name ?? '?'}  addr=${d.addr ?? '?'}  rssi=${d.rssi ?? '?'}  广播服务=${svc}`, true);
    });
  }
  if (diag.discovery) {
    log(`GATT 枚举：共 ${diag.discovery.length} 个服务`, true);
    diag.discovery.forEach(s => {
      log(`  服务 ${s.uuid}`, true);
      (s.chars || []).forEach(c => log(`    └ 特征 ${c.uuid}  h=${c.handle}  ${c.props.join('/')}`, true));
    });
  }
  if (diag.hint) log('提示：' + diag.hint, false);
}

// ---------- 连接 ----------
async function connect() {
  $('btnConnect').disabled = true;
  log('正在扫描并连接泵…');
  const r = await api('/api/connect', {});
  log(r.msg || JSON.stringify(r), r.ok);
  logDiag(r.diag);
  if (r.ok) {
    setConn(true);
    // 连接后先发一次 release，清除固件端可能残留的「按住自动重复」卡死态
    api('/api/key', { name: 'release' });
  }
  $('btnConnect').disabled = false;
}

async function scanOnly() {
  $('btnScan').disabled = true;
  log('仅扫描（不连接）…');
  const r = await api('/api/scan', {});
  if (r.ok) logDiag({ scan: r.devices });
  else log(r.msg || JSON.stringify(r), false);
  $('btnScan').disabled = false;
}
function setConn(on) {
  gConnected = on;
  $('connDot').className = 'dot ' + (on ? 'on' : 'off');
  $('connText').textContent = on ? '已连接' : '未连接';
  $('btnDisconnect').disabled = !on;
}

// ---------- 电机点动 ----------
$('btnMove').onclick = async () => {
  const fwd = document.querySelector('input[name=dir]:checked').value === 'fwd';
  const steps = parseInt($('steps').value, 10) || 0;
  const speed = parseInt($('speed').value, 10) || 1500;
  spinGear(true);
  const r = await api('/api/move', { fwd, steps, speed });
  spinGear(false);
  log(`${fwd ? '前进' : '后退'} ${steps} 微步 @${speed}Hz → ${r.msg || JSON.stringify(r)}`, r.ok);
};
$('btnStop').onclick = async () => {
  const r = await api('/api/stop', {});
  spinGear(false);
  log(r.msg || JSON.stringify(r), r.ok);
};
$('btnJog').onclick = async () => {
  const fwd = document.querySelector('input[name=dir]:checked').value === 'fwd';
  const speed = parseInt($('speed').value, 10) || 1500;
  spinGear(true);                       // 持续旋转动画（停止按钮或撞限位才停）
  const r = await api('/api/move', { fwd, steps: 0, speed });
  // 连续点动不会自动结束：保持齿轮旋转，直到用户点「停止」或前端检测到位置不再变化
  log(`${fwd ? '前进' : '后退'} 连续点动 @${speed}Hz → ${r.msg || JSON.stringify(r)}`, r.ok);
};
document.querySelectorAll('.chip').forEach(c => c.onclick = () => {
  $('steps').value = parseInt($('steps').value, 10) + parseInt(c.dataset.steps, 10);
});

// ---------- 维护操作 ----------
$('btnPrime').onclick = async () => {
  const u = parseFloat($('primeU').value) || 0;
  const r = await api('/api/prime', { units: u });
  log(`排气 ${u}U → ${r.msg || JSON.stringify(r)}`, r.ok);
};
$('btnRewind').onclick = async () => { const r = await api('/api/rewind', {}); log(`退回 → ${r.msg || JSON.stringify(r)}`, r.ok); };
$('btnClear').onclick  = async () => { const r = await api('/api/clear', {});  log(`清报警 → ${r.msg || JSON.stringify(r)}`, r.ok); };
// ---------- 虚拟按键（按下/抬起配对，等同物理按键，杜绝固件长按自动重复卡死）----------
// 固件 ui_screen_key() 在 300ms 后会触发「按住自动重复」(ui_screen.cpp:1606)，
// 必须由 ui_screen_release() 终止。若只发按下不发抬起，ESP32 会一直重复触发该键。
// 故：pointerdown 发 key(name)，pointerup/leave/cancel 发 key('release')。
document.querySelectorAll('.kbtn').forEach(b => {
  const name = b.dataset.key;
  b.addEventListener('pointerdown', async (e) => {
    e.preventDefault();
    if (!gConnected) { log('未连接泵', false); return; }
    const r = await api('/api/key', { name });
    log(`按下 ${name} → ${r.msg || JSON.stringify(r)}`, r.ok);
  });
  const release = async () => {
    if (!gConnected) return;
    const r = await api('/api/key', { name: 'release' });
    log(`抬起 release → ${r.msg || JSON.stringify(r)}`, r.ok);
  };
  b.addEventListener('pointerup', release);
  b.addEventListener('pointerleave', release);
  b.addEventListener('pointercancel', release);
});

$('btnConnect').onclick = connect;
$('btnScan').onclick = scanOnly;
$('btnDisconnect').onclick = async () => { const r = await api('/api/disconnect', {}); setConn(false); log(r.msg || '', r.ok); };

// ---------- 可视化 ----------
let gearTimer = null;
function spinGear(on) {
  const g = $('gear');
  if (on) { g.classList.add('spinning'); }
  else { clearTimeout(gearTimer); gearTimer = setTimeout(() => g.classList.remove('spinning'), 400); }
}
function updateViz(posU, fullU) {
  const barrelTop = 20, barrelBottom = 256, maxH = barrelBottom - barrelTop - 4;
  const ratio = Math.max(0, Math.min(1, posU / fullU));
  const h = ratio * maxH;
  const liq = $('liquid');
  liq.setAttribute('y', barrelBottom - h);
  liq.setAttribute('height', h);
  $('plunger').setAttribute('y', barrelBottom - h - 14);
  $('posU').textContent = posU.toFixed(3) + ' U';
  $('posSteps').textContent = Math.round(posU * 2178) + ' 微步';
}

// ---------- 状态轮询 ----------
async function poll() {
  const s = await api('/api/state');
  if (s.connected) setConn(true);
  if (!s.valid && !s.connected) return;
  $('sState').textContent = s.state || '—';
  $('sLoop').textContent  = s.loop || '—';
  $('sBatt').textContent  = (s.battery != null ? s.battery + '%' : '—');
  $('sAlarm').textContent = s.alarm ? ('是(' + (s.alarm_code ?? '?') + ')') : '否';
  $('sAlarm').style.color = s.alarm ? 'var(--red)' : '';
  $('sRes').textContent   = (s.reservoir_u != null ? s.reservoir_u + ' U' : '—');
  $('sIob').textContent   = (s.iob_u != null ? s.iob_u + ' U' : '—');
  $('sBasal').textContent = (s.basal_uh != null ? s.basal_uh + ' U/h' : '—');
  $('sGlu').textContent   = (s.glucose != null ? s.glucose + ' mg/dL' : '—');
  $('sClk').textContent   = s.clock || '—';
  $('sStep').textContent  = s.step_loss ? '是' : '否';
  $('sStep').style.color  = s.step_loss ? 'var(--amber)' : '';
  const pct = s.bolus_progress || 0;
  $('bolusPct').textContent = pct + '%';
  $('bolusBar').style.width = pct + '%';
  if (s.motor_pos_u != null) updateViz(s.motor_pos_u, s.reservoir_full_u || 300);
  const wb = $('warnBox');
  if (s.alarm) { wb.style.display = 'block'; wb.textContent = '⚠ 泵报警中（code ' + (s.alarm_code ?? '?') + '），请处理后再操作。'; }
  else wb.style.display = 'none';
}
setInterval(poll, 600);

log('面板已就绪。点击「连接泵」开始（确保泵已上电、蓝牙开启）。');
