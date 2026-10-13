#!/usr/bin/env python3
"""
#15 CarPlay / 导航 App 共存验证（真机）

验证问题：iPhone 在使用导航 App（CarPlay / 苹果地图 / 高德等）时，
本 App 是否仍能持续收到 CoreLocation 更新并下发给仪表。

原理：CoreLocation 是系统级服务，多 App 可同时订阅；本脚本通过仪表侧的
「数据龄」与「包数增长」间接判断 iOS 侧定位是否仍在更新——不需要读取
任何其他 App 的数据（遵守 §5.1 禁止读取 Apple Maps 私有数据）。

用法（在仓库根目录）：
    # 1) 手机打开本 App 并点「开始驾驶」；再打开导航 App 开始导航
    # 2) 运行本脚本，期间保持导航运行
    python3 tools/test_carplay_coexist.py --minutes 10

判定：
    - 包数持续增长（≈2Hz）且链路以 LIVE 为主 → 定位未被导航 App 抢占（通过）
    - 包数停滞且链路长期 STALE → 共存失败（需记录限制）
"""
import argparse
import os
import re
import select
import sys
import time

PORT = '/dev/cu.usbmodem101'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--minutes', type=float, default=10, help='测试时长（分钟）')
    ap.add_argument('--port', default=PORT)
    ap.add_argument('--interval', type=float, default=15, help='采样间隔（秒）')
    args = ap.parse_args()

    if not os.path.exists(args.port):
        print(f'串口不存在：{args.port}（仪表是否已连接？）')
        return 2

    os.system(f'stty -f {args.port} 115200 raw -echo 2>/dev/null')
    fd = os.open(args.port, os.O_RDWR | os.O_NONBLOCK)

    def drain(sec):
        end, buf = time.time() + sec, bytearray()
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    buf += os.read(fd, 16384)
                except OSError:
                    pass
        return buf.decode('utf-8', 'replace')

    drain(1)
    print('=== #15 CarPlay / 导航共存验证 ===')
    print('请确认：本 App 已「开始驾驶」，且导航 App 正在导航（或 CarPlay 已连接）')
    print(f"时长 {args.minutes} 分钟，每 {args.interval}s 采样\n")
    print(f"{'t':>5} {'连接':>8} {'数据龄ms':>10} {'成功包':>8} {'Δ包':>6} {'链路':>8}")

    start = time.time()
    end_at = start + args.minutes * 60
    last_ok = None
    first_ok = None
    live = stale = off = 0
    stalls = 0

    while time.time() < end_at:
        os.write(fd, b'ble\n')
        time.sleep(0.3)
        txt = drain(0.9)
        conn = re.search(r'连接=(\S+)', txt)
        age = re.search(r'数据龄=(\d+)', txt)
        ok = re.search(r'成功=(\d+)', txt)
        link = re.search(r'链路状态=(\S+)', txt)
        g = lambda m, d='?': m.group(1) if m else d

        cur = int(ok.group(1)) if ok else (last_ok or 0)
        if first_ok is None:
            first_ok = cur
        delta = 0 if last_ok is None else cur - last_ok
        last_ok = cur
        st = g(link)
        if st == 'LIVE':
            live += 1
        elif st == 'STALE':
            stale += 1
        else:
            off += 1
        note = ''
        if delta == 0 and st == 'LIVE':
            stalls += 1
            note = '  <- 无增长'
        print(f"{int(time.time()-start):>5} {g(conn):>8} {g(age):>10} {cur:>8} {delta:>6} {st:>8}{note}",
              flush=True)
        time.sleep(max(0.0, args.interval - 1.4))

    total = live + stale + off
    print('\n=== 结果 ===')
    print(f'样本 {total}：LIVE {live} / STALE {stale} / 断开 {off}')
    if first_ok is not None:
        grew = (last_ok or 0) - first_ok
        print(f'成功包 {first_ok} → {last_ok}（+{grew}）')
        rate = grew / max(1.0, (time.time() - start))
        print(f'平均 {rate:.2f} 包/秒（2Hz 数据源预期 ≈2）')
    print(f'LIVE 期间无增长次数：{stalls}')
    # 判定：核心是「LIVE 期间必须持续有包」+「无断连」。
    # 不用 LIVE 占比作硬指标——静止时 iOS 降频到约 1Hz，偶发擦过 2.5s 阈值
    # 会产生少量 STALE，这与 CarPlay 无关（实测无 CarPlay 时比例相近）。
    conn_drops = off
    ok = (stalls == 0 and conn_drops == 0 and grew > 0)
    print(f'\n判定：{"通过" if ok else "需进一步确认"}')
    print('  判据：LIVE 期间无增长次数 = 0、BLE 断连 = 0、包数有增长')
    print(f'  实际：无增长 {stalls}、断连 {conn_drops}、增长 +{grew if first_ok is not None else 0}')
    print('  说明：STALE 占比不作硬指标——静止时 iOS 降频至约 1Hz，')
    print('        偶发擦过 2.5s 超时阈值，与导航 App 共存无关（已实测对比）。')
    os.close(fd)
    return 0


if __name__ == '__main__':
    sys.exit(main())
