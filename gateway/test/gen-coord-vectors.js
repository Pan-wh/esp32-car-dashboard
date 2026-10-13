#!/usr/bin/env node
/**
 * gen-coord-vectors.js —— 生成坐标转换测试向量（供 Swift 侧交叉验证）
 *
 * ## 为什么需要
 *
 * WGS84↔GCJ-02 的实现有**两份**（JS 在网关、Swift 在手机），语言不同无法共享代码。
 * 若两份实现产生分歧，会出现最难查的故障：路线按 GCJ-02 取回，位置却按 WGS84 匹配，
 * 整体偏移数百米，且**两端各自都"看起来正确"**。
 *
 * 本脚本用 JS 实现生成固定向量，Swift 测试读同一文件逐点比对，
 * 从而把"两份实现是否一致"变成一条可自动化的断言。
 *
 * 用法：
 *   node gateway/test/gen-coord-vectors.js > gateway/test/fixtures/coord-vectors.json
 */
import { wgs84ToGcj02, gcj02ToWgs84 } from '../src/coords.js';

/** 覆盖：国内多城市、境外、边境、极端值 */
const WGS84_POINTS = [
  // 国内（应发生偏移）
  { lon: 116.397428, lat: 39.90923, note: '北京天安门' },
  { lon: 121.473701, lat: 31.230416, note: '上海人民广场' },
  { lon: 114.057868, lat: 22.543099, note: '深圳市民中心' },
  { lon: 117.120095, lat: 36.651216, note: '济南' },
  { lon: 87.6168, lat: 43.8256, note: '乌鲁木齐（西部）' },
  { lon: 126.5349, lat: 45.8038, note: '哈尔滨（东北）' },
  { lon: 91.1409, lat: 29.6456, note: '拉萨（高原）' },
  { lon: 113.2644, lat: 23.1291, note: '广州' },
  // 边境附近（判定边界）
  { lon: 73.7, lat: 39.5, note: '西部边界内侧' },
  { lon: 134.9, lat: 48.0, note: '东北边界内侧' },
  { lon: 98.0, lat: 4.0, note: '南部边界内侧' },
  // 境外（应原样返回）
  { lon: -122.4194, lat: 37.7749, note: '旧金山' },
  { lon: 139.6917, lat: 35.6895, note: '东京' },
  { lon: 2.3522, lat: 48.8566, note: '巴黎' },
  { lon: 73.0, lat: 39.5, note: '西部边界外侧' },
  { lon: 136.0, lat: 48.0, note: '东北边界外侧' },
];

const vectors = WGS84_POINTS.map((p) => {
  const gcj = wgs84ToGcj02(p);
  const back = gcj02ToWgs84(gcj);
  return {
    note: p.note,
    wgs84: { lon: p.lon, lat: p.lat },
    gcj02: { lon: gcj.lon, lat: gcj.lat },
    // 往返结果，供 Swift 侧同时验证反变换
    roundTrip: { lon: back.lon, lat: back.lat },
  };
});

process.stdout.write(
  JSON.stringify(
    {
      _comment: [
        '由 gateway/test/gen-coord-vectors.js 生成，请勿手改。',
        '用途：让 Swift 与 JS 两份坐标转换实现逐点比对，防止两者漂移。',
        '重新生成：node gateway/test/gen-coord-vectors.js > gateway/test/fixtures/coord-vectors.json',
      ],
      vectors,
    },
    null,
    2,
  ) + '\n',
);
