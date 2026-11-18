// 页面状态、控件初始化与配置准备。

'use strict';
let algorithms = [];
const $ = id => document.getElementById(id),
      colors = ['#187a9d', '#ca7827', '#388258', '#9b4c99'];
let token = sessionStorage.getItem('heaterToken') || '', socket = null,
    cursor = 0, boot = '', history = [], status = null, ownsControl = false,
    polling = false, heartbeatBusy = false, reconnectTimer = null,
    controlEpoch = 0;
$('token').value = token;
for (let i = 0; i < 4; i++) {
  const card = document.createElement('div');
  card.className = 'channel';
  card.innerHTML = `<div style="color:${colors[i]}">通道 ${
      i + 1}</div><div class="value" id="r${
      i}">— Ω</div><div class="muted" id="d${i}">占空比 0</div>`;
  $('channels').append(card);
  const label = document.createElement('label');
  label.textContent = `通道 ${i + 1} `;
  const input = document.createElement('input');
  input.id = `duty${i}`;
  input.type = 'number';
  input.min = 0;
  input.max = 95;
  input.step = 1;
  input.value = 0;
  label.append(input);
  $('duties').append(label);
  const targetLabel = document.createElement('label');
  targetLabel.textContent = `通道 ${i + 1} 目标（Ω） `;
  const target = document.createElement('input');
  target.id = `target${i}`;
  target.type = 'number';
  target.step = '0.01';
  target.value = 112;
  targetLabel.append(target);
  $('targets').append(targetLabel);
}
function modeChanged() {
  const closed = $('controlMode').value === 'resistance';
  $('duties').hidden = closed;
  $('resistanceSettings').hidden = !closed;
  $('algorithm').disabled = !closed;
}
$('controlMode').onchange = modeChanged;
modeChanged();
function algorithmDefaults() {
  const a = algorithms.find(a => a.id === Number($('algorithm').value));
  if (!a) return;
  const params = {};
  for (const p of a.parameters)
    params[p.name] = p.count === 1 ? p.default : Array(p.count).fill(p.default);
  $('parameters').value = JSON.stringify(params, null, 2);
}
$('algorithm').onchange = algorithmDefaults;
async function loadAlgorithms() {
  const data = await api('/api/controllers');
  algorithms = data.algorithms;
  $('algorithm').textContent = '';
  for (const a of algorithms) {
    const option = document.createElement('option');
    option.value = a.id;
    option.textContent = a.name;
    $('algorithm').append(option);
  }
  if (algorithms.length) $('algorithm').value = algorithms[0].id;
  algorithmDefaults();
}
function readController() {
  const target =
      Array.from({length: 4}, (_, i) => Number($(`target${i}`).value));
  const tolerance = Number($('tolerance').value),
        max = Number($('outputLimit').value);
  if (target.some(v => !Number.isFinite(v) || v < 90 || v > 180) ||
      !Number.isFinite(tolerance) || tolerance <= 0 || !Number.isFinite(max) ||
      max < 0 || max > (status?.max_duty ?? 95))
    throw new Error('检查目标电阻、误差带及输出上限。');
  return {
    algorithm: Number($('algorithm').value),
    target,
    parameters: JSON.parse($('parameters').value),
    tolerance_ohm: tolerance,
    max_duty: max
  };
}
// 配置成功响应表示已接受，必须等待同一 config_id 准备完成。
async function configureController() {
  const reply = await api('/api/controller/config', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(readController())
  });
  const deadline = Date.now() + 2000;
  while (Date.now() < deadline) {
    const snapshot = await api('/api/status');
    if (snapshot.controller?.config_id !== reply.config_id)
      throw new Error('配置已被其他请求替换。');
    if (snapshot.controller.ready) return reply;
    if (snapshot.controller.state === 3) throw new Error('算法配置失败。');
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  throw new Error('算法配置准备超时。');
}
function message(text) {
  $('message').textContent = text;
}
