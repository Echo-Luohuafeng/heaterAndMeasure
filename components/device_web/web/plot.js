// 测量值显示及历史曲线绘制。
function updateFrame(f) {
  for (let i = 0; i < 4; i++) {
    $(`r${i}`).textContent =
        f[4][i] === null ? '无效' : `${f[4][i].toFixed(4)} Ω`;
    $(`d${i}`).textContent = `记录占空比 ${f[3][i]}`;
  }
  $('diagnostics').textContent = `采样序号 ${f[1]} · 有效标记 ${f[5]} · 故障 ${
      f[6]} · 时序迟到 ${f[7]} · 队列丢帧 ${f[8]} · 控制栈余量 ${f[9]} 字节${
      f[10]?.mode ? ` · 恒电阻 · 算法耗时 ${f[10].computation_us} µs · 限幅 ${
                        f[10].limited_mask}` :
                    ''}`;
}
function draw() {
  const canvas = $('plot'), ratio = window.devicePixelRatio || 1,
        w = canvas.clientWidth, h = 260;
  canvas.width = w * ratio;
  canvas.height = h * ratio;
  const c = canvas.getContext('2d');
  c.scale(ratio, ratio);
  c.clearRect(0, 0, w, h);
  if (!history.length) return;
  const left = 55, right = w - 12, top = 20, bottom = h - 30,
        end = history.at(-1)[2], start = Math.max(0, end - 60000000);
  let min = Infinity, max = -Infinity;
  for (const f of history)
    for (const r of [...f[4], ...(f[10]?.mode ? f[10].target : [])])
      if (r !== null) {
        min = Math.min(min, r);
        max = Math.max(max, r);
      }
  if (!Number.isFinite(min)) {
    min = 90;
    max = 180;
  }
  const margin = Math.max(.05, (max - min) * .1);
  min -= margin;
  max += margin;
  c.font = '12px system-ui';
  for (let i = 0; i < 5; i++) {
    const y = top + (bottom - top) * i / 4;
    c.strokeStyle = '#e2e9ee';
    c.beginPath();
    c.moveTo(left, y);
    c.lineTo(right, y);
    c.stroke();
    c.fillStyle = '#536b79';
    c.fillText((max - (max - min) * i / 4).toFixed(2), 0, y + 4);
  }
  for (let i = 0; i < 4; i++) {
    c.strokeStyle = colors[i];
    c.lineWidth = 1.5;
    c.beginPath();
    let pen = false, previous = 0;
    for (const f of history) {
      const r = f[4][i];
      if (r === null || f[2] < start) {
        pen = false;
        continue;
      }
      const x = left + (right - left) * (f[2] - start) / 60000000,
            y = bottom - (bottom - top) * (r - min) / (max - min);
      if (pen && f[2] - previous < 40000)
        c.lineTo(x, y);
      else
        c.moveTo(x, y);
      pen = true;
      previous = f[2];
    }
    c.stroke();
  }
  for (let i = 0; i < 4; i++) {
    c.strokeStyle = colors[i];
    c.setLineDash([4, 4]);
    c.beginPath();
    let pen = false;
    for (const f of history) {
      const e = f[10];
      if (!e || !e.mode || f[2] < start) {
        pen = false;
        continue;
      }
      const x = left + (right - left) * (f[2] - start) / 60000000,
            y = bottom - (bottom - top) * (e.target[i] - min) / (max - min);
      if (pen)
        c.lineTo(x, y);
      else
        c.moveTo(x, y);
      pen = true;
    }
    c.stroke();
    c.setLineDash([]);
  }
  c.fillStyle = '#536b79';
  c.fillText('最近 60 秒', left, h - 8);
}
