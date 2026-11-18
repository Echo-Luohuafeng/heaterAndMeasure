// CSV 与配置元数据导出，以及 OTA 上传。
function experimentColumns(f) {
  const e = f[10];
  return e ?
      [
        e.run_id, e.config_id, e.mode, e.algorithm, ...e.target, ...e.requested,
        e.limited_mask, e.blocked_mask, e.source_sequence, e.computation_us
      ] :
      Array(16).fill('');
}
$('csv').onclick = async () => {
  const button = $('csv');
  button.disabled = true;
  try {
    const snapshot = await api('/api/status');
    const target = snapshot.store.next;
    const metadata = snapshot.controller ? await api('/api/experiments') : null;
    let from = snapshot.store.oldest;
    const lines = [
      'boot_id,cursor,sequence,cycle_start_us,duty1,duty2,duty3,duty4,R1,R2,R3,R4,valid_mask,faults,deadline_misses,telemetry_drops,control_stack_free_bytes,run_id,config_id,mode,algorithm,target1,target2,target3,target4,requested1,requested2,requested3,requested4,limited_mask,blocked_mask,source_sequence,computation_us'
    ];
    while (from < target) {
      const batch = await api(
          `/api/data?from=${from}&limit=250&boot=${snapshot.boot_id}`);
      if (batch.lost) throw new Error('导出期间缓存已覆盖，请重新导出。');
      for (const f of batch.frames) {
        if (f[0] >= target) break;
        lines.push([
          snapshot.boot_id, f[0], f[1], f[2], ...f[3],
          ...f[4].map(r => r === null ? '' : r), ...f.slice(5, 10),
          ...experimentColumns(f)
        ].join(','));
      }
      if (batch.next <= from) break;
      from = batch.next;
    }
    const url = URL.createObjectURL(
        new Blob([lines.join('\n') + '\n'], {type: 'text/csv;charset=utf-8'}));
    const link = document.createElement('a');
    link.href = url;
    link.download = `heater-${snapshot.boot_id}.csv`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    if (metadata) {
      const metaUrl = URL.createObjectURL(new Blob(
          [JSON.stringify(metadata, null, 2)], {type: 'application/json'}));
      const meta = document.createElement('a');
      meta.href = metaUrl;
      meta.download = `heater-${snapshot.boot_id}.metadata.json`;
      meta.click();
      setTimeout(() => URL.revokeObjectURL(metaUrl), 1000);
    }
    message(`已导出 ${lines.length - 1} 帧。`);
  } catch (e) {
    message(e.message);
  } finally {
    button.disabled = false;
  }
};
$('ota').onclick = async () => {
  const file = $('firmware').files[0];
  if (!file) {
    message('请选择应用 .bin 文件。');
    return;
  }
  if (!confirm('停止加热和采样，并上传新固件？')) return;
  controlEpoch++;
  ownsControl = false;
  $('ota').disabled = true;
  message('正在上传固件，请保持页面与设备连接。');
  try {
    await api('/api/ota', {
      method: 'POST',
      headers: {'Content-Type': 'application/octet-stream'},
      body: file
    });
    message('升级完成，设备正在重启。');
  } catch (e) {
    message(`升级未确认：${e.message}。查看设备状态，失败后可恢复采样。`);
  } finally {
    $('ota').disabled = false;
  }
};
if (token) {
  connectDashboard().catch(e => message(e.message));
}
