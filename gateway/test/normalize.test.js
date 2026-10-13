/**
 * normalize.test.js —— 高德响应归一化测试
 *
 * 重点：
 *   - 高德把数字返回为**字符串**（"10234"），必须正确转数字；
 *   - 无法解析的距离**不得**变成 0（否则仪表会显示"即将转向"）；
 *   - 业务错误（status!=1）必须显式抛错，不能静默当成空路线；
 *   - 折线解析要容错（坏点跳过）但不产生非法坐标。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import {
  normalizeRoute,
  assertAmapOk,
  parsePolyline,
  toNumberOrNull,
  stepAt,
  AmapError,
} from '../src/normalize.js';
import { MANEUVER } from '../src/maneuver.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const SAMPLE = JSON.parse(readFileSync(join(HERE, 'fixtures/amap-driving-sample.json'), 'utf8'));

const META = {
  origin: { lon: 116.397428, lat: 39.90923 },
  destination: { lon: 116.481028, lat: 39.989643 },
  routeId: 'test-1',
};

test('toNumberOrNull：无法解析返回 null（不是 0）', () => {
  assert.equal(toNumberOrNull('123'), 123);
  assert.equal(toNumberOrNull(123), 123);
  assert.equal(toNumberOrNull(0), 0);
  assert.equal(toNumberOrNull('0'), 0);
  assert.equal(toNumberOrNull(''), null);
  assert.equal(toNumberOrNull(null), null);
  assert.equal(toNumberOrNull(undefined), null);
  assert.equal(toNumberOrNull('abc'), null);
  assert.equal(toNumberOrNull(NaN), null);
  assert.equal(toNumberOrNull(Infinity), null);
});

test('parsePolyline：正常解析', () => {
  const pts = parsePolyline('116.1,39.1;116.2,39.2');
  assert.deepEqual(pts, [
    { lon: 116.1, lat: 39.1 },
    { lon: 116.2, lat: 39.2 },
  ]);
});

test('parsePolyline：坏点跳过但不产生非法坐标', () => {
  const pts = parsePolyline('116.1,39.1;bad;116.2;999,0;116.3,39.3');
  assert.deepEqual(pts, [
    { lon: 116.1, lat: 39.1 },
    { lon: 116.3, lat: 39.3 },
  ]);
  assert.deepEqual(parsePolyline(''), []);
  assert.deepEqual(parsePolyline(null), []);
  assert.deepEqual(parsePolyline(123), []);
});

test('assertAmapOk：成功与失败', () => {
  assert.equal(assertAmapOk({ status: '1' }).status, '1');
  assert.equal(assertAmapOk({ status: 1 }).status, 1);
  assert.throws(() => assertAmapOk({ status: '0', info: 'INVALID_USER_KEY', infocode: '10001' }), (e) => {
    assert.ok(e instanceof AmapError);
    assert.equal(e.info, 'INVALID_USER_KEY');
    assert.equal(e.infocode, '10001');
    return true;
  });
  assert.throws(() => assertAmapOk(null), AmapError);
  assert.throws(() => assertAmapOk('not json'), AmapError);
});

test('normalizeRoute：整条路线（含字符串数字转换）', () => {
  const b = normalizeRoute(SAMPLE, META);
  assert.equal(b.routeId, 'test-1');
  assert.equal(b.distanceM, 10234);
  assert.equal(b.durationS, 1200);
  assert.deepEqual(b.origin, META.origin);
  assert.deepEqual(b.destination, META.destination);
  assert.equal(b.steps.length, 5);
  assert.equal(b.alternativeCount, 0);
});

test('normalizeRoute：步骤动作映射正确（含顺序敏感的掉头）', () => {
  const b = normalizeRoute(SAMPLE, META);
  assert.deepEqual(
    b.steps.map((s) => s.maneuver),
    [
      MANEUVER.RIGHT, // 向东行驶120米右转
      MANEUVER.LEFT, // 左转进入建国门内大街
      MANEUVER.KEEP_RIGHT, // 靠右行驶进入东二环
      MANEUVER.U_TURN, // 左转掉头 ← 必须判为掉头
      MANEUVER.ARRIVE, // 到达目的地
    ],
  );
});

test('normalizeRoute：步骤距离与路名', () => {
  const b = normalizeRoute(SAMPLE, META);
  assert.equal(b.steps[0].distanceM, 120);
  assert.equal(b.steps[1].distanceM, 2400);
  assert.equal(b.steps[1].roadName, '建国门内大街');
  assert.equal(b.steps[0].roadName, null, '空路名应为 null');
});

test('normalizeRoute：全路线折线由各步骤拼接且去重相邻重复点', () => {
  const b = normalizeRoute(SAMPLE, META);
  // 5 个 step 各 2 点，相邻 step 首尾重复 → 去重后应为 6 点
  assert.equal(b.polyline.length, 6);
  assert.deepEqual(b.polyline[0], { lon: 116.397428, lat: 39.90923 });
  assert.deepEqual(b.polyline[5], { lon: 116.481028, lat: 39.989643 });
});

test('normalizeRoute：业务失败必须抛错（不得当成空路线）', () => {
  assert.throws(
    () => normalizeRoute({ status: '0', info: 'DAILY_QUERY_OVER_LIMIT', infocode: '10003' }, META),
    (e) => {
      assert.ok(e instanceof AmapError);
      assert.equal(e.info, 'DAILY_QUERY_OVER_LIMIT');
      return true;
    },
  );
});

test('normalizeRoute：无路径 / 空路径 → 抛错', () => {
  assert.throws(() => normalizeRoute({ status: '1', route: { paths: [] } }, META), AmapError);
  assert.throws(() => normalizeRoute({ status: '1', route: {} }, META), AmapError);
  assert.throws(
    () => normalizeRoute({ status: '1', route: { paths: [{ distance: '0', duration: '0', steps: [] }] } }, META),
    AmapError,
  );
});

test('★ 距离解析失败不得变成 0', () => {
  const json = {
    status: '1',
    route: {
      paths: [
        {
          distance: 'not-a-number',
          duration: null,
          steps: [{ instruction: '直行', step_distance: 'abc', polyline: '116.1,39.1' }],
        },
      ],
    },
  };
  const b = normalizeRoute(json, META);
  // 路径级距离不可解析 → 回退到 step 累加；step 也不可解析 → 0 累加
  assert.equal(b.distanceM, 0);
  assert.equal(b.steps[0].distanceM, null, '单个 step 距离不可解析必须是 null，不是 0');
  assert.equal(b.durationS, null, '时长不可解析必须是 null');
});

test('stepAt：按已行驶距离定位当前步骤', () => {
  const b = normalizeRoute(SAMPLE, META);
  // 累计: 120, 2520, 7520, 9020, 10234
  assert.equal(stepAt(b, 0).stepIndex, 0);
  assert.equal(stepAt(b, 0).maneuver, MANEUVER.RIGHT);
  assert.equal(stepAt(b, 0).distanceToManeuverM, 120);

  assert.equal(stepAt(b, 500).stepIndex, 1);
  assert.equal(stepAt(b, 500).maneuver, MANEUVER.LEFT);
  assert.equal(stepAt(b, 500).distanceToManeuverM, 2020);

  assert.equal(stepAt(b, 9000).stepIndex, 3);
  assert.equal(stepAt(b, 9000).maneuver, MANEUVER.U_TURN);

  // 走完全程 → 到达
  assert.equal(stepAt(b, 20000).maneuver, MANEUVER.ARRIVE);
  assert.equal(stepAt(b, 20000).distanceToManeuverM, 0);
});

test('stepAt：空步骤安全降级', () => {
  const r = stepAt({ steps: [] }, 100);
  assert.equal(r.stepIndex, -1);
  assert.equal(r.maneuver, MANEUVER.UNKNOWN);
  assert.equal(r.distanceToManeuverM, null);
  assert.equal(stepAt(null, 0).maneuver, MANEUVER.UNKNOWN);
});
