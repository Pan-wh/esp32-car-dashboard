#!/usr/bin/env node
/**
 * make-structural-fixture.js —— 把真实高德响应"结构保留、数据替换"
 *
 * ## 为什么
 *
 * 高德协议 §3.5 禁止**直接存储**其服务数据。但我们确实需要一份
 * **字段结构与真实响应一致**的夹具，用于防止"字段名改了就整条路线失效"
 * （本项目最初就踩过：按文档猜的 `step_distance`/`road_name` 实际是
 * `distance`/`road`，靠真实探测才发现）。
 *
 * 因此本脚本保留 `action` 词表与全部**字段名/层级**，把
 * 坐标、距离、路名、折线等**服务数据替换为合成值**：
 *   - 回归保护针对的是"字段名与结构"，用合成值同样有效；
 *   - 仓库里不留真实路网数据，符合 §3.5 的最小化原则。
 *
 * 用法：
 *   node gateway/test/make-structural-fixture.js <真实响应.json> <输出.json>
 */
import { readFileSync, writeFileSync } from 'node:fs';

const [, , src, dst] = process.argv;
if (!src || !dst) {
  console.error('用法: node make-structural-fixture.js <真实响应.json> <输出.json>');
  process.exit(1);
}

const real = JSON.parse(readFileSync(src, 'utf8'));

/** 合成路名池（循环取用，保证测试可预期） */
const FAKE_ROADS = ['示例一路', '示例二路', '示例三街', '示例四路', '示例五街', '示例六路'];

// ─────────────────────────────────────────────────────────────
// 合成路线：**必须首尾相连**
//
// 早期版本每段从各自的原点开始生成折线，导致步骤之间**断开** ——
// 这样的"路线"不真实，还会让下游（导航核心的投影/垂距）行为异常
// （实测垂距峰值 195m 而非预期的 80m）。合成数据同样要满足
// "是一条能开的路"这一基本约束。
//
// 做法：维护一个游标与航向，逐点前进；每段走完后航向略作偏转，
// 并把该段的几何长度回填到 `distance`，保证距离与几何自洽。
// ─────────────────────────────────────────────────────────────
const METERS_PER_DEG_LAT = 111320;
let cursor = { lon: 120.0, lat: 30.0 };
let headingDeg = 45; // 0=北，90=东

function metersPerDegLon(lat) {
  return METERS_PER_DEG_LAT * Math.cos((lat * Math.PI) / 180);
}

/** 生成一段折线（点数 = count），段落总长约 targetLen 米；**从 cursor 起** */
function fakePolyline(count, targetLen) {
  const parts = [];
  const segLen = count > 1 ? targetLen / (count - 1) : 0;
  for (let i = 0; i < count; i++) {
    parts.push(`${cursor.lon.toFixed(6)},${cursor.lat.toFixed(6)}`);
    if (i === count - 1) break;
    // 轻微左右偏转，使路线不是一条直线（但保持平滑可导）
    headingDeg += i % 2 === 0 ? 3 : -2;
    const rad = (headingDeg * Math.PI) / 180;
    const dNorth = segLen * Math.cos(rad);
    const dEast = segLen * Math.sin(rad);
    cursor = {
      lat: cursor.lat + dNorth / METERS_PER_DEG_LAT,
      lon: cursor.lon + dEast / metersPerDegLon(cursor.lat),
    };
  }
  return parts.join(';');
}

/** 保留所有键，替换"内容型"值 */
function scrubStep(step, idx) {
  const out = {};
  for (const [k, v] of Object.entries(step)) {
    if (k === 'polyline') {
      const n = typeof v === 'string' ? v.split(';').filter(Boolean).length : 0;
      // 目标长度沿用原距离量级（100/110/…），保证测试期望稳定
      const targetLen = 100 + idx * 10;
      out[k] = fakePolyline(n, targetLen);
    } else if (k === 'road') {
      out[k] = v ? FAKE_ROADS[idx % FAKE_ROADS.length] : '';
    } else if (k === 'instruction') {
      // ⚠️ 必须保留**动作关键词**，否则会破坏"action 为空时退回 instruction"这条路径
      //    （实测最后一段 action 为空、靠 instruction「…到达目的地」判定）。
      //    做法：用合成的动作句式替换真实长句 —— 去掉真实道路名与距离，
      //    但保留可判定的动作词。
      const act = typeof step.action === 'string' ? step.action.trim() : '';
      out[k] = act ? `合成指引${act}` : '合成指引到达目的地';
    } else if (k === 'orientation' || k === 'toll_road') {
      out[k] = Array.isArray(v) ? [] : '';
    } else if (k === 'distance' || k === 'duration') {
      out[k] = String(100 + idx * 10); // 合成数值，仍为字符串（保持类型）
    } else {
      out[k] = v; // action / tolls 等保留（action 是转向词，非路网数据）
    }
  }
  // action 与 assistant_action 必须保留 —— 它们是**转向语义**（如「到达目的地」），
  // 不是路网数据，且正是被测对象。注意实测到的类型差异：
  //   action           : 字符串（如 "左转"）或**空数组**（末段无动作时）
  //   assistant_action : 数组（多数段为空数组）或**字符串**（末段为 "到达目的地"）
  return out;
}

const out = JSON.parse(JSON.stringify(real));
for (const path of out.route?.paths ?? []) {
  (path.steps ?? []).forEach((s, i) => Object.assign(s, scrubStep(s, i)));
  path.distance = String((path.steps ?? []).reduce((a, s) => a + Number(s.distance || 0), 0));
  path.duration = String(600);
}
if (out.route) {
  // 起终点与首尾折线保持一致（避免"起终点与路线对不上"这种不真实数据）
  out.route.origin = '120.000000,30.000000';
  out.route.destination = `${cursor.lon.toFixed(6)},${cursor.lat.toFixed(6)}`;
}

out._comment = [
  '⚠️ 结构夹具：字段名与层级来自**真实响应**，但数值/坐标/路名/折线均为**合成数据**。',
  '这是有意为之 —— 高德协议 §3.5 禁止直接存储其服务数据，而回归保护针对的是',
  '「字段名与结构」，用合成值同样有效。',
  '重新生成：node gateway/test/make-structural-fixture.js <真实响应.json> <本文件>',
];

writeFileSync(dst, JSON.stringify(out, null, 2) + '\n', 'utf8');
console.log(`已写出结构夹具 → ${dst}`);
