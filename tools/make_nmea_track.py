#!/usr/bin/env python3
"""
生成合成 NMEA 轨迹日志，用于在没有硬件时验证「NMEA → 解析 → UI」整条链路。

用法：
  python3 tools/make_nmea_track.py                    # 默认输出 tests/data/drive_sample.nmea
  python3 tools/make_nmea_track.py --out xxx.nmea --seconds 90

场景（1 Hz）：
  0-5s    静止（有效定位）
  6-25s   加速 0 → 150 km/h
  26-45s  巡航 150（超速：咬牙表情 + 红色刻度区）
  46-55s  减速到 80（皱眉 → 平静）
  56-65s  隧道：丢失定位（RMC 状态 V / GGA 质量 0）→ UI 应显示 NO FIX
  66-85s  恢复定位，60 → 110 km/h，航向持续变化
  86-89s  减速到 0

语句：RMC（速度/航向/日期时间）+ GGA（质量/卫星/HDOP/海拔）+ GSA（3D 定位）+ VTG（航向/km/h）
"""
import argparse
import math
import os
import random

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sentence(body: str) -> str:
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return f"${body}*{cs:02X}"


def speed_profile(t: int) -> float:
    """返回该秒的速度（km/h）；-1 表示丢失定位"""
    if t <= 5:
        return 0.0
    if t <= 25:                       # 加速
        return (t - 5) / 20.0 * 150.0
    if t <= 45:                       # 巡航
        return 150.0 + math.sin(t / 3.0) * 2.0
    if t <= 55:                       # 减速
        return 150.0 - (t - 45) / 10.0 * 70.0
    if t <= 65:                       # 隧道：断定位
        return -1.0
    if t <= 85:                       # 恢复，60 → 110
        return 60.0 + (t - 66) / 19.0 * 50.0
    return max(0.0, 110.0 - (t - 85) / 4.0 * 110.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "tests", "data", "drive_sample.nmea"))
    ap.add_argument("--seconds", type=int, default=90)
    ap.add_argument("--hdop", type=float, default=0.9)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    random.seed(20261010)

    lines = []
    heading = 45.0
    lat, lon = 30.0, 120.0  # 起点（度，十进制；生成时再转度分）

    for t in range(args.seconds):
        kmh = speed_profile(t)
        lost = kmh < 0
        kmh = max(0.0, kmh)

        if not lost:
            # 运动模糊式推进：位置随速度/航向变化
            heading = (heading + random.uniform(-1.5, 1.5) + (0.8 if 40 < t < 70 else 0)) % 360
            step = kmh / 3600.0 / 111.0  # 每秒移动的度数（约）
            lat += step * math.cos(math.radians(heading))
            lon += step * math.sin(math.radians(heading)) / math.cos(math.radians(lat))

        hh, mm, ss = 12, 0, t % 60
        d, mo, yy = 10, 10, 26

        def dm(v, is_lat):
            dd = int(abs(v))
            mi = (abs(v) - dd) * 60.0
            return f"{dd:02d}{mi:07.4f}", ("N" if v >= 0 else "S") if is_lat else ("E" if v >= 0 else "W")

        lat_s, lat_h = dm(lat, True)
        lon_s, lon_h = dm(lon, False)

        status = "V" if lost else "A"
        knots = kmh / 1.852
        quality = 0 if lost else 1
        sats = 0 if lost else 9
        hdop = 99.9 if lost else args.hdop + random.uniform(-0.15, 0.15)
        alt = 12.0

        lines.append(sentence(
            f"GNRMC,{hh:02d}{mm:02d}{ss:02d}.00,{status},{lat_s},{lat_h},{lon_s},{lon_h},"
            f"{knots:.2f},{heading:.1f},{d:02d}{mo:02d}{yy:02d},,,A"))
        lines.append(sentence(
            f"GNGGA,{hh:02d}{mm:02d}{ss:02d}.00,{lat_s},{lat_h},{lon_s},{lon_h},"
            f"{quality},{sats:02d},{hdop:.1f},{alt:.1f},M,0.0,M,,"))
        lines.append(sentence(
            f"GNGSA,A,{1 if lost else 3},04,05,,09,12,,,24,,,,,2.5,{hdop:.1f},2.1"))
        lines.append(sentence(
            f"GNVTG,{heading:.1f},T,{heading * 0.98:.1f},M,{knots:.2f},N,{kmh:.2f},K"))

    with open(args.out, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")

    print(f"已生成 {args.out}")
    print(f"  共 {len(lines)} 行 / {args.seconds} 秒；含 56-65s 的断定位段（状态 V + 质量 0）")


if __name__ == "__main__":
    main()
