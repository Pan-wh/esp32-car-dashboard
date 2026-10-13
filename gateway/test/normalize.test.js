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
/**
 * 结构夹具：**字段名与层级来自真实响应**（2026-10-13 用真实 Key 核对），
 * 但数值/坐标/路名/折线为**合成数据** —— 高德协议 §3.5 禁止直接存储其服务数据，
 * 而回归保护针对的是"字段名与结构"，用合成值同样有效。
 * 由 gateway/test/make-structural-fixture.js 生成。
 */
const REAL = JSON.parse(readFileSync(join(HERE, 'fixtures/amap-driving-v3.json'), 'utf8'));

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


// ─────────────────────────────────────────────────────────────
// 真实响应回归（用真实 Key 抓取，见 fixtures/amap-driving-real.json）
//
// 这组测试是**字段名的真实保护**：最初的实现按文档猜测使用了
// `step_distance` / `road_name`，而实测是 `distance` / `road`。
// 若上游改字段名，这组测试会失败并直接指出问题。
// ─────────────────────────────────────────────────────────────

test('★ 真实结构：字段名正确解析（distance / road / action）', () => {
  const b = normalizeRoute(REAL, META);
  assert.equal(b.distanceM, 1260, '路径级总距离（合成值：100+110+…+180）');
  assert.equal(b.durationS, 600, '总时长');
  assert.equal(b.steps.length, 9, '步骤数');

  // 每一步都必须有距离。**这是字段名保护的核心**：
  // 最初的实现读的是 step_distance（实际是 distance），会导致这里全是 null。
  const missing = b.steps.filter((s) => s.distanceM === null);
  assert.equal(missing.length, 0, `不应有缺距离的步骤，实际缺 ${missing.length} 个`);

  // 路名来自 steps[].road（不是 road_name）
  assert.equal(b.steps[0].roadName, '示例一路', '路名应从 steps[].road 解析');
  assert.equal(b.steps[0].distanceM, 100);

  // 折线应非空
  assert.ok(b.polyline.length > 100, `折线点应较多，实际 ${b.polyline.length}`);
});

test('★ 真实结构：全部 9 段动作都被识别（无 unknown）', () => {
  const b = normalizeRoute(REAL, META);
  const unknown = b.steps.filter((s) => s.maneuver === MANEUVER.UNKNOWN);
  assert.equal(unknown.length, 0,
    `不应有未识别动作，实际: ${unknown.map((s) => s.instruction).join(' | ')}`);

  // 逐段核对（action 值来自真实响应，未做合成替换）
  assert.deepEqual(b.steps.map((s) => s.maneuver), [
    MANEUVER.LEFT,       // 左转
    MANEUVER.LEFT,       // 左转
    MANEUVER.KEEP_LEFT,  // 靠左
    MANEUVER.RIGHT,      // 向右前方行驶
    MANEUVER.U_TURN,     // 左转调头
    MANEUVER.LEFT,       // 左转
    MANEUVER.KEEP_RIGHT, // 靠右
    MANEUVER.RIGHT,      // 右转
    MANEUVER.ARRIVE,     // action 为空，靠 instruction「到达目的地」
  ]);
});

test('★ 真实结构：step 距离之和与路径总距离量级一致', () => {
  const b = normalizeRoute(REAL, META);
  const sum = b.steps.reduce((a, s) => a + (s.distanceM ?? 0), 0);
  // 高德的路径总距离与各段之和可能有小差异（四舍五入），但量级必须一致
  assert.ok(Math.abs(sum - b.distanceM) / b.distanceM < 0.05,
    `各段之和 ${sum} 与总距离 ${b.distanceM} 差异过大`);
});

test('stepAt 在真实路线上可用', () => {
  const b = normalizeRoute(REAL, META);
  assert.equal(stepAt(b, 0).stepIndex, 0);
  assert.equal(stepAt(b, 0).maneuver, MANEUVER.LEFT);
  // 走完全程 → 到达
  assert.equal(stepAt(b, b.distanceM + 1000).maneuver, MANEUVER.ARRIVE);
});
