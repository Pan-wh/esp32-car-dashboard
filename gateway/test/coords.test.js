/**
 * coords.test.js —— 坐标转换测试（node:test，零依赖）
 *
 * ⚠️ 测试策略说明（诚实标注）：
 * 本算法是**公开的拟合实现**，离线环境无法取得权威基准值，
 * 因此这里用**性质测试**（往返一致性、境外直通、偏移量级、单调性）而非
 * "与官方值逐位比对"。首次真机联调时应抽样比对实际路网（见 nav-backend.md §5）。
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import {
  wgs84ToGcj02,
  gcj02ToWgs84,
  outOfChina,
  distanceM,
  formatLonLat,
  parseLonLat,
  validLonLat,
} from '../src/coords.js';

/** 国内代表点（北京天安门、上海人民广场、深圳市民中心、济南） */
const CHINA_POINTS = [
  { lon: 116.397428, lat: 39.90923 },
  { lon: 121.473701, lat: 31.230416 },
  { lon: 114.057868, lat: 22.543099 },
  { lon: 117.120095, lat: 36.651216 },
];

/** 境外代表点（不应发生偏移） */
const ABROAD_POINTS = [
  { lon: -122.4194, lat: 37.7749 }, // 旧金山
  { lon: 139.6917, lat: 35.6895 }, // 东京
  { lon: 2.3522, lat: 48.8566 }, // 巴黎
];

test('境外点不偏移（原样返回）', () => {
  for (const p of ABROAD_POINTS) {
    assert.ok(outOfChina(p.lon, p.lat), `${p.lon},${p.lat} 应判定为境外`);
    const g = wgs84ToGcj02(p);
    assert.equal(g.lon, p.lon);
    assert.equal(g.lat, p.lat);
  }
});

test('境内点判定正确', () => {
  for (const p of CHINA_POINTS) {
    assert.ok(!outOfChina(p.lon, p.lat), `${p.lon},${p.lat} 应判定为境内`);
  }
});

test('境内偏移量级合理（约 300~700 米）', () => {
  for (const p of CHINA_POINTS) {
    const g = wgs84ToGcj02(p);
    const d = distanceM(p, g);
    assert.ok(d > 100 && d < 1000, `${p.lon},${p.lat} 偏移 ${d.toFixed(0)}m 超出预期区间`);
  }
});

test('往返转换一致（wgs84 → gcj02 → wgs84）', () => {
  for (const p of CHINA_POINTS) {
    const back = gcj02ToWgs84(wgs84ToGcj02(p));
    // 迭代求解，误差应远小于 1e-6 度（约 0.1 米）
    assert.ok(
      Math.abs(back.lon - p.lon) < 1e-7 && Math.abs(back.lat - p.lat) < 1e-7,
      `往返误差过大: ${JSON.stringify(p)} → ${JSON.stringify(back)}`,
    );
  }
});

test('偏移方向稳定：同一区域邻近点偏移量接近', () => {
  // 相邻两点（约 1km）的偏移矢量应非常接近，否则路网会扭曲
  const a = { lon: 116.397428, lat: 39.90923 };
  const b = { lon: 116.407428, lat: 39.90923 };
  const ga = wgs84ToGcj02(a);
  const gb = wgs84ToGcj02(b);
  const dLonA = ga.lon - a.lon;
  const dLonB = gb.lon - b.lon;
  assert.ok(Math.abs(dLonA - dLonB) < 1e-4, `邻近点偏移量差异过大: ${dLonA} vs ${dLonB}`);
});

test('不修改入参（纯函数）', () => {
  const p = { lon: 116.397428, lat: 39.90923 };
  const snapshot = { ...p };
  wgs84ToGcj02(p);
  gcj02ToWgs84(p);
  assert.deepEqual(p, snapshot);
});

test('非法输入抛错（不静默产生 NaN）', () => {
  assert.throws(() => wgs84ToGcj02(null), TypeError);
  assert.throws(() => wgs84ToGcj02({ lon: NaN, lat: 39 }), TypeError);
  assert.throws(() => wgs84ToGcj02({ lon: 200, lat: 39 }), TypeError);
  assert.throws(() => wgs84ToGcj02({ lon: 116, lat: 99 }), TypeError);
  assert.throws(() => gcj02ToWgs84({ lon: '116', lat: 39 }), TypeError);
});

test('validLonLat 边界', () => {
  assert.ok(validLonLat({ lon: 0, lat: 0 }));
  assert.ok(validLonLat({ lon: -180, lat: -90 }));
  assert.ok(validLonLat({ lon: 180, lat: 90 }));
  assert.ok(!validLonLat({ lon: 181, lat: 0 }));
  assert.ok(!validLonLat({ lon: 0, lat: -91 }));
  assert.ok(!validLonLat(undefined));
});

test('formatLonLat：经度在前、6 位小数（高德要求）', () => {
  assert.equal(formatLonLat({ lon: 116.3974281234, lat: 39.9092309876 }), '116.397428,39.909231');
  assert.equal(formatLonLat({ lon: 117.5, lat: 40.417801 }), '117.500000,40.417801');
  assert.throws(() => formatLonLat({ lon: 999, lat: 0 }), TypeError);
});

test('parseLonLat 与 formatLonLat 互逆', () => {
  const p = { lon: 116.397428, lat: 39.90923 };
  assert.deepEqual(parseLonLat(formatLonLat(p)), p);
  assert.throws(() => parseLonLat('116.397428'), TypeError);
  assert.throws(() => parseLonLat('a,b'), TypeError);
  assert.throws(() => parseLonLat('999,0'), TypeError);
  assert.throws(() => parseLonLat(123), TypeError);
});

test('distanceM：已知量级自检', () => {
  // 同一点为 0
  assert.equal(distanceM({ lon: 116, lat: 39 }, { lon: 116, lat: 39 }), 0);
  // 纬度差 1 度 ≈ 111.2 km
  const d = distanceM({ lon: 116, lat: 39 }, { lon: 116, lat: 40 });
  assert.ok(Math.abs(d - 111195) < 500, `1 度纬度应约 111.2km，实际 ${d.toFixed(0)}m`);
  // 对称性
  const a = { lon: 116.1, lat: 39.1 };
  const b = { lon: 116.9, lat: 39.8 };
  assert.equal(distanceM(a, b).toFixed(6), distanceM(b, a).toFixed(6));
});
