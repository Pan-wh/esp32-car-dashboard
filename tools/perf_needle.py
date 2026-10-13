#!/usr/bin/env python3
"""
perf_needle.py —— 指针流畅度实测（可复现的前后对比口径）

## 为什么需要它

心跳行里的"重绘/s"只在**有数据变化**时才有意义：手机没连、速度不变时
重绘率恒为 0，看不出流畅度。要判断"指针是否卡顿"必须：

  1. 用 BLE **持续注入连续变化**的车速（默认 2Hz，与真实 App 同频）
  2. 同时从串口抓设备侧的**每 5 秒渲染统计**
  3. 用**固定口径**统计，才能前后对比

本脚本把这三步固化，并记录 uptime 以检测测试期间是否被重新烧录
（那样结果不可信）。

## 用法

    python3 tools/perf_needle.py                    # 默认 150 秒
    python3 tools/perf_needle.py --duration 300
    python3 tools/perf_needle.py --out docs/perf/after.json

## 前置

需先编译 BLE 注入工具：

    swiftc -O -o /tmp/ble_speed_probe -framework CoreBluetooth \\
      tools/nav_sim/NavSimBLE.swift tools/ble_speed_probe.swift

## 输出

JSON（便于跨版本对比）+ 控制台摘要。关键指标：

- `render_max_ms` 的 **p50 / p90 / max**：单帧渲染耗时分布
- `spikes_*`：超过 80/100/150ms 的 5 秒窗口数（卡顿次数）
- `duty_pct`：渲染占空比（是否接近饱和）
- `uptime_delta`：与测试时长的差值，用于确认未被重新烧录
"""

import argparse
import json
import os
import re
import select
import subprocess
import sys
import threading
import time

PORT = '/dev/cu.usbmodem101'
PROBE = '/tmp/ble_speed_probe'

# 与固件心跳行匹配（格式见 firmware/dashboard/dashboard.ino 的心跳输出）
RE_RUN = re.compile(r'\[运行\] (\d+) s\s+空闲堆 (\d+) B\s+循环 (\d+)/s\s+重绘 (\d+)/s')
RE_RENDER = re.compile(
    r'渲染最大 (\d+) ms（占空 (\d+)%）｜QSPI 传输 合计 (\d+) ms 单次最大 (\d+) ms')


def pct(values, p):
    if not values:
        return None
    s = sorted(values)
    idx = min(len(s) - 1, max(0, int(len(s) * p) - 1))
    return s[idx]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--duration', type=int, default=150, help='注入时长（秒）')
    ap.add_argument('--hz', type=float, default=2.0, help='注入频率（默认与 App 同频 2Hz）')
    ap.add_argument('--port', default=PORT)
    ap.add_argument('--probe', default=PROBE)
    ap.add_argument('--out', default=None, help='把结果 JSON 写到该路径')
    ap.add_argument('--face', type=int, default=2, help='测试前切到的表盘 id（默认速度表）')
    ap.add_argument('--label', default='', help='本次测量的标签（如 before/after）')
    args = ap.parse_args()

    if not os.path.exists(args.probe):
        print(f'❌ 缺少 BLE 注入工具 {args.probe}\n'
              f'   请先编译：\n'
              f'   swiftc -O -o {args.probe} -framework CoreBluetooth \\\n'
              f'     tools/nav_sim/NavSimBLE.swift tools/ble_speed_probe.swift')
        return 2

    os.system(f'stty -f {args.port} 115200 raw -echo 2>/dev/null')
    try:
        fd = os.open(args.port, os.O_RDWR | os.O_NONBLOCK)
    except OSError as e:
        print(f'❌ 打不开串口 {args.port}：{e}')
        return 2

    def drain(sec):
        end, buf = time.time() + sec, bytearray()
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    d = os.read(fd, 16384)
                except OSError:
                    d = b''
                if d:
                    buf += d
        return buf.decode('utf-8', 'replace')

    drain(1)
    os.write(fd, f'face {args.face}\n'.encode())
    time.sleep(1)
    drain(1.5)
    os.write(fd, b'ble\n')
    time.sleep(0.4)
    pre = drain(1.2)
    m = re.search(r'已运行 (\d+) s', pre)
    up_start = int(m.group(1)) if m else -1
    m = re.search(r'复位原因=(\S+)', pre)
    reset_reason = m.group(1) if m else '?'

    print(f'[测量] 开始 uptime={up_start}s 复位原因={reset_reason} '
          f'时长={args.duration}s 频率={args.hz}Hz 标签={args.label or "-"}')

    buf, stop = [], False

    def reader():
        while not stop:
            d = drain(0.4)
            if d:
                buf.append(d)

    th = threading.Thread(target=reader)
    th.start()

    t0 = time.time()
    proc = subprocess.Popen([args.probe, str(args.duration), str(args.hz)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    while proc.poll() is None and time.time() - t0 < args.duration + 90:
        time.sleep(2)
    stop = True
    th.join()
    out = ''.join(buf)

    os.write(fd, b'ble\n')
    time.sleep(0.4)
    post = drain(1.5)
    m = re.search(r'已运行 (\d+) s', post)
    up_end = int(m.group(1)) if m else -1
    os.close(fd)

    runs = RE_RUN.findall(out)
    rend = RE_RENDER.findall(out)
    rmax = [int(x[0]) for x in rend]
    duty = [int(x[1]) for x in rend]
    qsum = [int(x[2]) for x in rend]
    qmax = [int(x[3]) for x in rend]
    loops = [int(x[2]) for x in runs]
    redraws = [int(x[3]) for x in runs]

    result = {
        'label': args.label,
        'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
        'duration_s': args.duration,
        'hz': args.hz,
        'uptime_start': up_start,
        'uptime_end': up_end,
        'uptime_delta': (up_end - up_start) if (up_start >= 0 and up_end >= 0) else None,
        'reset_reason': reset_reason,
        'samples_run': len(runs),
        'samples_render': len(rend),
        'loop_per_s': {'min': min(loops) if loops else None,
                       'max': max(loops) if loops else None,
                       'p50': pct(loops, 0.5)},
        'redraw_per_s': {'min': min(redraws) if redraws else None,
                         'max': max(redraws) if redraws else None,
                         'p50': pct(redraws, 0.5)},
        'render_max_ms': {'min': min(rmax) if rmax else None,
                          'p50': pct(rmax, 0.5), 'p90': pct(rmax, 0.9),
                          'max': max(rmax) if rmax else None},
        'duty_pct': {'p50': pct(duty, 0.5), 'max': max(duty) if duty else None},
        'qspi_max_ms': {'p50': pct(qmax, 0.5), 'max': max(qmax) if qmax else None},
        'spikes_gt80': sum(1 for v in rmax if v > 80),
        'spikes_gt100': sum(1 for v in rmax if v > 100),
        'spikes_gt150': sum(1 for v in rmax if v > 150),
        'per_window': [{'render_max_ms': a, 'duty_pct': b, 'qspi_sum_ms': c, 'qspi_max_ms': d}
                       for a, b, c, d in zip(rmax, duty, qsum, qmax)],
    }

    # 可信度提示：uptime 回退说明测试期间被重新烧录
    delta = result['uptime_delta']
    credible = True
    if delta is not None and delta < args.duration - 30:
        credible = False
    result['credible'] = credible

    print(json.dumps({k: v for k, v in result.items() if k != 'per_window'},
                     ensure_ascii=False, indent=2))
    if not credible:
        print('⚠️ uptime 增量明显小于测试时长 —— 测试期间设备被重启/重新烧录，结果不可信')

    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, 'w', encoding='utf-8') as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        print(f'[测量] 已写入 {args.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
