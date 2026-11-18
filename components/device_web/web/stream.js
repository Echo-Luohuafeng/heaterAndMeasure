// 历史批次、重连与设备状态查询。
function stream() {
  clearTimeout(reconnectTimer);
  if (socket) {
    socket.onclose = null;
    socket.close();
  }
  const current =
      new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${
          location.host}/ws?token=${encodeURIComponent(token)}&from=${
          cursor}&boot=${boot}`);
  socket = current;
  current.onopen = () => {
    $('connection').textContent = '已连接';
  };
  current.onmessage = event => {
    const batch = JSON.parse(event.data);
    if (boot && boot !== batch.boot_id) {
      history = [];
      cursor = 0;
      ownsControl = false;
      message('设备已重启，加热设定已清除。');
    }
    boot = batch.boot_id;
    if (batch.lost) message(`缓存中已有 ${batch.lost} 帧被覆盖。`);
    for (const f of batch.frames) {
      if (f[0] < cursor) continue;
      history.push(f);
      updateFrame(f);
    }
    cursor = batch.next;
    if (history.length) {
      const cutoff = history.at(-1)[2] - 60000000;
      history = history.filter(f => f[2] >= cutoff);
    }
    draw();
  };
  current.onclose = () => {
    $('connection').textContent = '数据连接中断';
    if (socket === current) reconnectTimer = setTimeout(stream, 2000);
  };
  current.onerror = () => {
    $('connection').textContent = '连接失败';
  };
}
async function poll() {
  if (!token || polling) return;
  polling = true;
  const epoch = controlEpoch;
  try {
    status = await api('/api/status');
    if (status.measurement) updateFrame(status.measurement);
    if (epoch === controlEpoch &&
        ((boot && status.boot_id !== boot) || !status.running))
      ownsControl = false;
    for (let i = 0; i < 4; i++) $(`duty${i}`).max = status.max_duty ?? 95;
    $('device').textContent =
        `固件 ${status.version} · ${status.network.ip} · USB ${
            status.network.connected ? '已连接' : '已断开'} · ${
            status.controller?.mode === 'resistance' ?
                '恒电阻' :
                '手动占空比'} · 内部 RAM ${status.free_internal_bytes} 字节 · ${
            status.verified ? '自检通过' : '自检未通过'} · ${
            status.maintenance ? '维护中' :
                status.running ? '加热与记录中' :
                                 '未开始加热，未记录'}`;
    $('store').textContent = `${
        status.store.psram ?
            'PSRAM' :
            '内部 RAM 降级缓存'} · ${status.store.next - status.store.oldest}/${
        status.store.capacity} 帧 · 历史覆盖 ${status.store.overwritten} 帧 · ${
        status.recording ? '正在记录' : '记录已停止'}`;
    $('otaStatus').textContent = status.ota.active ?
        `已写入 ${status.ota.received}/${status.ota.total} 字节` :
        status.ota.error ? `升级错误 ${status.ota.error}` :
                           '';
    if (status.maintenance && epoch === controlEpoch) ownsControl = false;
    if (!status.running)
      for (let i = 0; i < 4; i++) $(`d${i}`).textContent = '当前未加热';
    if (status.control_ready === false) {
      $('diagnostics').textContent = `采样初始化失败（${
          status
              .control_init_error}），输出已关闭。检查 ADC 供电与连接后重启；网页和 OTA 可继续使用。`;
    }
    $('start').disabled = !status.verified || status.running ||
        status.ota.active || status.control_ready === false;
    $('set').disabled = !status.verified || status.maintenance ||
        status.control_ready === false ||
        ($('controlMode').value === 'resistance' && status.running);
    $('controlMode').disabled = status.running;
    $('outputLimit').max = status.max_duty ?? 95;
  } catch (error) {
    $('connection').textContent = error.message;
  } finally {
    polling = false;
  }
}
async function connectDashboard() {
  const snapshot = await api('/api/status');
  status = snapshot;
  await loadAlgorithms();
  boot = snapshot.boot_id;
  cursor = Math.max(snapshot.store.oldest, snapshot.store.next - 50);
  if (snapshot.latest) updateFrame(snapshot.latest);
  stream();
  poll();
}
$('connect').onclick = async () => {
  token = $('token').value.trim();
  cursor = 0;
  boot = '';
  history = [];
  ownsControl = false;
  try {
    await connectDashboard();
    sessionStorage.setItem('heaterToken', token);
    message('连接成功。');
  } catch (e) {
    message(e.message);
  }
};
