/**
 * routes.test.js —— 多方案路线（/routes）的契约测试
 *
 * 这个端点的存在理由值得记下来：高德 v3 驾车 API **一次只返回 1 条**路径
 * （实测 strategy 0/1/2/6/8/35/36/37 全是 paths.length === 1），
 * 但**换 strategy 会得到真正不同的路线**。所以"多方案对比"只能由网关
 * 用多个策略各取一次、再去重合并。
 *
 * 最容易错的是**去重**：实测策略 0/1/8/35/36/37 给出的距离与耗时完全相同
 * （同一条路），不去重的话用户会看到三条一模一样的"选项"。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { mergeRouteCandidates, normalizeRoute } from '../src/normalize.js';

/** 造一条最小可用的高德 v3 驾车响应 */
function amapDriving({ distance, duration, poly }) {
  return {
    status: '1',
    info: 'OK',
    infocode: '10000',
    route: {
      paths: [{
        distance: String(distance),
        duration: String(duration),
        steps: [{
          instruction: '向北行驶',
          road: '某路',
          distance: '100',
          duration: '10',
          polyline: poly,
        }],
        polyline: poly, /* 真实响应这两处都有；normalizePath 优先用 steps[].polyline */
      }],
    },
  };
}

const P1 = '116.397428,39.90923;116.400000,39.920000';
const P2 = '116.397428,39.90923;116.410000,39.910000;116.420000,39.930000';

/** 把一条 amap 响应归一化成 /routes 里用的 path 对象 */
function pathOf(args) {
  return normalizeRoute(amapDriving(args), {});
}

test('多方案：不同策略的不同路线会被保留并按耗时排序', () => {
  const cands = mergeRouteCandidates([
    { strategy: 0, path: pathOf({ distance: 16847, duration: 2350, poly: P1 }) },
    { strategy: 2, path: pathOf({ distance: 15173, duration: 2859, poly: P2 }) },
    { strategy: 6, path: pathOf({ distance: 16056, duration: 2376, poly: P2 }) },
  ]);
  assert.equal(cands.length, 3);
  /* 按耗时升序：用户最关心"哪条更快" */
  assert.ok(cands[0].durationS <= cands[1].durationS);
  assert.ok(cands[1].durationS <= cands[2].durationS);
  assert.equal(cands[0].durationS, 2350);
  assert.equal(cands[2].durationS, 2859);
});

test('多方案：距离与耗时都相同的路线会被去重（实测多个策略给出同一条路）', () => {
  const same = pathOf({ distance: 16847, duration: 2350, poly: P1 });
  const cands = mergeRouteCandidates([
    { strategy: 0, path: same },
    { strategy: 1, path: pathOf({ distance: 16847, duration: 2350, poly: P1 }) },
    { strategy: 8, path: pathOf({ distance: 16847, duration: 2350, poly: P1 }) },
    { strategy: 36, path: pathOf({ distance: 16847, duration: 2350, poly: P1 }) },
  ]);
  assert.equal(cands.length, 1, '四条相同路线应合并成一条，否则用户看到重复选项');
});

test('多方案：距离或耗时任一不同就视为不同路线', () => {
  const a = mergeRouteCandidates([
    { strategy: 0, path: pathOf({ distance: 1000, duration: 100, poly: P1 }) },
    { strategy: 2, path: pathOf({ distance: 1000, duration: 101, poly: P1 }) }, /* 只差耗时 */
    { strategy: 6, path: pathOf({ distance: 1001, duration: 100, poly: P1 }) }, /* 只差距离 */
  ]);
  assert.equal(a.length, 3);
});

test('多方案：失败的策略（null）被跳过，不影响其余候选', () => {
  const cands = mergeRouteCandidates([
    { strategy: 0, path: pathOf({ distance: 16847, duration: 2350, poly: P1 }) },
    { strategy: 2, path: null },   /* 上游报错 / 权限不足 */
    null,                          /* 整体缺失 */
    { strategy: 6, path: pathOf({ distance: 16056, duration: 2376, poly: P2 }) },
  ]);
  assert.equal(cands.length, 2);
  assert.deepEqual(cands.map((c) => c.strategy).sort(), [0, 6]);
});

test('多方案：折线不足两点的候选被丢弃（画不出线）', () => {
  const cands = mergeRouteCandidates([
    { strategy: 0, path: pathOf({ distance: 1000, duration: 100, poly: P1 }) },
    { strategy: 2, path: { distanceM: 900, durationS: 90, steps: [], polyline: [[1, 2]] } },
    { strategy: 6, path: { distanceM: 900, durationS: 90, steps: [], polyline: [] } },
  ]);
  assert.equal(cands.length, 1);
});

test('多方案：候选保留 steps 与 polyline（App 选完就能直接开始导航）', () => {
  const cands = mergeRouteCandidates([
    { strategy: 0, path: pathOf({ distance: 16847, duration: 2350, poly: P2 }) },
  ]);
  assert.equal(cands.length, 1);
  assert.ok(Array.isArray(cands[0].steps) && cands[0].steps.length > 0);
  assert.ok(Array.isArray(cands[0].polyline) && cands[0].polyline.length >= 2);
  assert.equal(cands[0].distanceM, 16847);
});

test('单条路线：normalizeRoute 也能拿到 pathIndex / pathCount（向前兼容）', () => {
  const b = normalizeRoute(amapDriving({ distance: 16847, duration: 2350, poly: P1 }), {});
  assert.equal(b.pathIndex, 0);
  assert.equal(b.pathCount, 1);
  assert.equal(b.alternativeCount, 0);
  assert.deepEqual(b.alternatives, []);
});

test('单条路线：pathIndex 越界时回落到 0，不抛错（"换一条"点到头应原地不动）', () => {
  const b = normalizeRoute(amapDriving({ distance: 1, duration: 1, poly: P1 }), { pathIndex: 99 });
  assert.equal(b.pathIndex, 0);
});
