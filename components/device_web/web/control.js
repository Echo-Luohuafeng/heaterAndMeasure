// 显式运行、手动设定、可选续约及 STOP。
async function command(text) {
  return api(
      '/api/control',
      {method: 'POST', headers: {'Content-Type': 'text/plain'}, body: text});
}
function readDuties() {
  const duties = Array.from({length: 4}, (_, i) => Number($(`duty${i}`).value));
  if (duties.some(
          v => !Number.isInteger(v) || v < 0 || v > (status?.max_duty ?? 95))) {
    message(`请输入四个 0–${status?.max_duty ?? 95} 整数。`);
    return null;
  }
  return duties;
}
$('set').onclick = async () => {
  if ($('controlMode').value === 'resistance') {
    try {
      await configureController();
      message('恒电阻配置已保存，点击开始后运行。');
    } catch (e) {
      message(e.message);
    }
    return;
  }
  const duties = readDuties();
  if (!duties) return;
  try {
    await command(`SET ${duties.join(' ')}`);
    message(
        ownsControl || status?.running ?
            '设定已接收，下一周期生效。' :
            '设定已保存，点击“开始加热”后才加热和记录。');
  } catch (e) {
    message(e.message);
  }
};
$('start').onclick = async () => {
  const closed = $('controlMode').value === 'resistance';
  const duties = closed ? null : readDuties();
  if (!closed && !duties) return;
  $('start').disabled = true;
  try {
    const configured = closed ? await configureController() : null;
    const reply = await command(
        closed ? `RSTART ${configured.config_id}` :
                 `START ${duties.join(' ')}`);
    controlEpoch++;
    ownsControl = true;
    history = [];
    cursor = reply.next;
    status = {...status, running: true, recording: true, maintenance: false};
    stream();
    message('已开始加热与记录。');
    if ($('hold').checked) await heartbeat();
  } catch (e) {
    message(e.message);
  } finally {
    poll();
  }
};
$('stop').onclick = async () => {
  controlEpoch++;
  ownsControl = false;
  status = {...status, running: false, recording: false};
  try {
    await command('STOP');
    message('加热与记录已停止，已有缓存保留。');
  } catch (e) {
    message(`停止命令未确认：${e.message}；控制租约到期后停止。`);
  }
};
$('reboot').onclick = async () => {
  if (!confirm('停止实验并重启设备？')) return;
  controlEpoch++;
  ownsControl = false;
  try {
    await api('/api/reboot', {method: 'POST'});
    message('设备正在重启。');
  } catch (e) {
    message(e.message);
  }
};
async function heartbeat() {
  if (!$('hold').checked || heartbeatBusy) return;
  heartbeatBusy = true;
  const epoch = controlEpoch;
  try {
    if (!ownsControl) {
      const snapshot = await api('/api/status');
      if (epoch !== controlEpoch || !$('hold').checked || !snapshot.running)
        return;
      status = snapshot;
      ownsControl = true;
    }
    await command('PING');
  } catch (e) {
    if (epoch === controlEpoch) ownsControl = false;
    message(`续约失败：${e.message}`);
  } finally {
    heartbeatBusy = false;
  }
}
$('hold').onchange = () => heartbeat();
setInterval(heartbeat, 1000);
setInterval(poll, 1000);
window.addEventListener('resize', draw);
window.addEventListener('pagehide', () => {
  if (ownsControl)
    fetch('/api/control', {
      method: 'POST',
      headers: {Authorization: `Bearer ${token}`, 'Content-Type': 'text/plain'},
      body: 'STOP',
      keepalive: true
    }).catch(() => {});
});
