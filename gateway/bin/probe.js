#!/usr/bin/env node
/**
 * probe.js —— 拉一次**真实**高德响应，用于核对字段名与动作码
 *
 * ## 为什么需要这个工具
 *
 * 网关的字段映射（normalize.js）与动作码表（maneuver.js 的 ACTION_CODE_MAP）
 * 都依赖高德的真实返回结构。我等**不能凭印象填写** ——
 * 把"左转"映射成"右转"比映射成"未知"危险得多。
 *
 * 本工具把原始响应的**字段清单**打印出来，让你照着填/改，而不是猜。
 *
 * ## 用法
 *
 *   export AMAP_KEY=你的Web服务Key
 *   npm run probe -- --from 116.397428,39.90923 --to 116.481028,39.989643
 *   npm run probe -- --from ... --to ... --save test/fixtures/amap-driving-real.json
 *
 * ⚠️ 坐标是 **WGS84**（手机定位坐标系），脚本会先转 GCJ-02 再请求。
 * ⚠️ 输出**不会**打印 Key。
 */
import { writeFileSync } from 'node:fs';

import { redact } from '../src/config.js';
import { fetchDrivingRoute } from '../src/amap.js';
import { normalizeRoute } from '../src/normalize.js';
import { parseLonLat } from '../src/coords.js';
import { maneuverName } from '../src/maneuver.js';

function parseArgs(argv) {
  const out = { from: null, to: null, save: null, strategy: 0 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--from') out.from = argv[++i];
    else if (a === '--to') out.to = argv[++i];
    else if (a === '--save') out.save = argv[++i];
    else if (a === '--strategy') out.strategy = Number(argv[++i]);
    else if (a === '--help' || a === '-h') out.help = true;
  }
  return out;
}

const USAGE = `用法：
  export AMAP_KEY=你的Web服务Key
  npm run probe -- --from <lon,lat> --to <lon,lat> [--strategy N] [--save <file>]

说明：
  --from/--to 使用 **WGS84** 坐标（手机定位坐标系），脚本会自动转 GCJ-02。
  输出包含：字段清单（用于核对字段名）、原始响应、归一化结果。`;

/** 递归收集一个 JSON 的字段路径 → 类型，便于核对字段名 */
function inventory(obj, prefix = '', acc = new Map()) {
  if (obj === null || typeof obj !== 'object') {
    acc.set(prefix || '(root)', typeof obj);
    return acc;
  }
  if (Array.isArray(obj)) {
    acc.set(`${prefix}[]`, `array(${obj.length})`);
    if (obj.length > 0) inventory(obj[0], `${prefix}[]`, acc);
    return acc;
  }
  for (const [k, v] of Object.entries(obj)) {
    const path = prefix ? `${prefix}.${k}` : k;
    if (v !== null && typeof v === 'object' && !Array.isArray(v)) inventory(v, path, acc);
    else if (Array.isArray(v)) inventory(v, path, acc);
    else acc.set(path, `${typeof v}${typeof v === 'string' ? ` "${String(v).slice(0, 40)}"` : ` = ${v}`}`);
  }
  return acc;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help || !args.from || !args.to) {
    console.log(USAGE);
    process.exit(args.help ? 0 : 1);
  }

  const key = (process.env.AMAP_KEY ?? '').trim();
  if (!key) {
    console.error('缺少环境变量 AMAP_KEY。见 docs/nav-backend.md §3.2。');
    process.exit(1);
  }

  let origin, destination;
  try {
    origin = parseLonLat(args.from);
    destination = parseLonLat(args.to);
  } catch (e) {
    console.error(`坐标解析失败：${e.message}`);
    process.exit(1);
  }

  console.log(`[探测] 起点(WGS84) ${args.from}`);
  console.log(`[探测] 终点(WGS84) ${args.to}`);

  let result;
  try {
    result = await fetchDrivingRoute({ key, origin, destination, strategy: args.strategy });
  } catch (e) {
    // 错误信息经 redact，避免任何路径泄露 Key
    console.error(`[失败] ${redact(e?.message ?? String(e), [key])}`);
    if (e?.info) console.error(`       高德 info=${e.info} infocode=${e.infocode}`);
    process.exit(2);
  }

  const { raw, originGcj, destinationGcj } = result;
  console.log(`[探测] 起点已转 GCJ-02: ${originGcj.lon.toFixed(6)},${originGcj.lat.toFixed(6)}`);
  console.log(`[探测] 终点已转 GCJ-02: ${destinationGcj.lon.toFixed(6)},${destinationGcj.lat.toFixed(6)}`);

  // ---- 1) 字段清单（对照 normalize.js 使用的字段名）----
  console.log('\n=== 字段清单（核对字段名是否与 normalize.js 一致）===');
  const inv = inventory(raw);
  for (const [path, desc] of inv) {
    console.log(`  ${path.padEnd(48)} ${desc}`);
  }

  // ---- 2) 各 step 的转向信息（用于填 ACTION_CODE_MAP）----
  console.log('\n=== 各 step 的转向字段（action ↔ instruction）===');
  const steps = raw?.route?.paths?.[0]?.steps ?? [];
  if (steps.length === 0) {
    console.log('  ⚠️ 未找到 route.paths[0].steps —— 字段结构与假设不符，请据上面清单调整 normalize.js');
  } else {
    steps.slice(0, 12).forEach((s, i) => {
      const action = s?.navi?.action ?? s?.action ?? '(无)';
      console.log(
        `  [${String(i).padStart(2)}] action=${String(action).padEnd(6)} ` +
          `dist=${String(s?.step_distance ?? '?').padEnd(7)} instruction=${s?.instruction ?? '(无)'}`,
      );
    });
    if (steps.length > 12) console.log(`  … 另有 ${steps.length - 12} 个 step`);
  }

  // ---- 3) 归一化结果（本网关实际会返回给 App 的内容）----
  console.log('\n=== 归一化结果（App 将收到的内容）===');
  try {
    const bundle = normalizeRoute(raw, { origin: originGcj, destination: destinationGcj });
    console.log(`  总距离 ${bundle.distanceM} m，总时长 ${bundle.durationS} s，` +
      `步骤 ${bundle.steps.length} 个，折线点 ${bundle.polyline.length} 个`);
    bundle.steps.slice(0, 12).forEach((s) => {
      console.log(
        `  [${String(s.index).padStart(2)}] ${maneuverName(s.maneuver).padEnd(11)} ` +
          `${String(s.distanceM ?? '?').padStart(6)} m  ${s.roadName ?? ''}`,
      );
    });
    const unknown = bundle.steps.filter((s) => s.maneuver === 0).length;
    if (unknown > 0) {
      console.log(`\n  ⚠️ 有 ${unknown} 个步骤未能识别动作（显示为"未知"）。`);
      console.log('     若上面的 action 有值，请据实测填入 src/maneuver.js 的 ACTION_CODE_MAP；');
      console.log('     若无 action 字段，请检查 instruction 文本是否需要补充匹配规则。');
    }
  } catch (e) {
    console.log(`  归一化失败：${redact(e?.message ?? String(e), [key])}`);
  }

  // ---- 4) 原始响应 ----
  console.log('\n=== 原始响应 ===');
  console.log(redact(JSON.stringify(raw, null, 2), [key]));

  // ---- 5) 可选：存为夹具 ----
  if (args.save) {
    writeFileSync(args.save, JSON.stringify(raw, null, 2) + '\n', 'utf8');
    console.log(`\n[已保存] 原始响应 → ${args.save}`);
    console.log('         用它替换 test/fixtures/ 中的样例，使测试基于真实字段。');
  }
}

main().catch((e) => {
  console.error(`[异常] ${e?.stack ?? e}`);
  process.exit(3);
});
