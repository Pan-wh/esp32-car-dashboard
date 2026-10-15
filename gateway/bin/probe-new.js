/**
 * probe-new.js —— 用**真实高德 Key** 核实新增 5 个接口的字段名
 *
 * ## 为什么必须有它
 *
 * 本项目的一条硬规矩（见 README）：**字段名以实测为准，不凭印象填写**。
 * normalize/amap 里对上游字段的每处引用，都应该有真实响应佐证，
 * 否则上游一改字段，我们只会得到"静默的空数据"而不是明确报错。
 *
 * 用法：
 *   cd gateway
 *   node --env-file-if-exists=.env bin/probe-new.js
 *
 * ⚠️ 输出**不打印 Key**（经 redact）。仅打印字段清单与关键值，便于人工核对。
 */
import { loadConfig, redact } from '../src/config.js';
import {
  fetchRegeo,
  fetchTrafficRectangle,
  fetchPlaceAround,
  fetchInputTips,
  fetchBicyclingRoute,
} from '../src/amap.js';
import { normalizeBicycling } from '../src/normalize.js';

const config = loadConfig();
const secrets = [config.amapKey];
const red = (s) => redact(s, secrets);

/** 北京天安门附近（WGS84） */
const P = { lon: 116.397428, lat: 39.90923 };
const Q = { lon: 116.481028, lat: 39.989643 };

function line(t) { console.log(`\n${'─'.repeat(64)}\n${t}\n${'─'.repeat(64)}`); }

/** 递归收集响应里出现的字段名（用于人工比对文档） */
function fieldNames(obj, prefix = '', out = new Set()) {
  if (obj === null || typeof obj !== 'object') return out;
  if (Array.isArray(obj)) {
    if (obj.length > 0) fieldNames(obj[0], `${prefix}[]`, out);
    return out;
  }
  for (const [k, v] of Object.entries(obj)) {
    const p = prefix ? `${prefix}.${k}` : k;
    out.add(p);
    if (v && typeof v === 'object') fieldNames(v, p, out);
  }
  return out;
}

const results = {};

/* ── 1. 逆地理编码 ── */
line('1) /regeo 逆地理编码：坐标 → 道路名');
try {
  const r = await fetchRegeo({ key: config.amapKey, point: P });
  results.regeo = r;
  console.log('  归一化结果:', JSON.stringify(r, null, 2));
  console.log('\n  ✅ 核对：road 是否解析出？formattedAddress 是否非空？');
} catch (e) {
  console.log('  ❌ 失败:', red(e?.message));
  if (e?.info) console.log('     info=', e.info, 'infocode=', e.infocode);
}

/* ── 2. 交通态势 ── */
line('2) /traffic 交通态势：周边拥堵（⚠️ 不是红绿灯）');
try {
  const r = await fetchTrafficRectangle({ key: config.amapKey, center: P, halfMeters: 800 });
  results.traffic = r;
  console.log('  归一化结果:', JSON.stringify(r, null, 2));
  console.log('\n  ✅ 核对：status 是否为中文等级？roads 是否有内容？');
} catch (e) {
  console.log('  ❌ 失败:', red(e?.message));
  if (e?.info) console.log('     info=', e.info, 'infocode=', e.infocode);
}

/* ── 3. 周边搜索 ── */
line('3) /around 周边搜索：加油站');
try {
  const r = await fetchPlaceAround({
    key: config.amapKey, center: P, keywords: '加油站', radius: 5000, limit: 5,
  });
  results.around = r;
  console.log(`  返回 ${r.pois.length} 条：`);
  for (const p of r.pois) {
    console.log(`    - ${p.name} | ${p.distanceM}m | WGS84 ${p.lon.toFixed(6)},${p.lat.toFixed(6)} | ${p.type}`);
  }
  console.log('\n  ✅ 核对：name/distanceM/坐标是否合理？坐标应为 WGS84（比 GCJ-02 略偏西/南）');
} catch (e) {
  console.log('  ❌ 失败:', red(e?.message));
  if (e?.info) console.log('     info=', e.info, 'infocode=', e.infocode);
}

/* ── 4. 输入提示 ── */
line('4) /tips 输入提示：搜索联想');
try {
  const r = await fetchInputTips({ key: config.amapKey, keywords: '天安', limit: 5 });
  results.tips = r;
  console.log(`  返回 ${r.tips.length} 条：`);
  for (const t of r.tips) {
    console.log(`    - ${t.name} | ${t.district} | adcode=${t.adcode} | lon=${t.lon} lat=${t.lat}`);
  }
  console.log('\n  ✅ 核对：是否有候选 lon/lat 为 null（无坐标 POI）？有则说明容错生效');
} catch (e) {
  console.log('  ❌ 失败:', red(e?.message));
  if (e?.info) console.log('     info=', e.info, 'infocode=', e.infocode);
}

/* ── 5. 骑行路线 ── */
line('5) /bicycling 骑行路线（v4）');
try {
  const { raw, originGcj, destinationGcj } = await fetchBicyclingRoute({
    key: config.amapKey, origin: P, destination: Q,
  });
  console.log('  原始响应顶层字段:', Object.keys(raw).join(', '));
  console.log('  errcode=', raw.errcode, 'errmsg=', raw.errmsg);
  console.log('  data.paths 数量:', Array.isArray(raw.data?.paths) ? raw.data.paths.length : '(无)');
  const p0 = raw.data?.paths?.[0];
  if (p0) {
    console.log('  首个 path 字段:', Object.keys(p0).join(', '));
    console.log('  distance=', p0.distance, 'duration=', p0.duration);
    if (Array.isArray(p0.steps) && p0.steps[0]) {
      console.log('  首个 step 字段:', Object.keys(p0.steps[0]).join(', '));
      console.log('  首个 step 样例:', JSON.stringify(p0.steps[0]).slice(0, 220));
    }
  }
  const bundle = normalizeBicycling(raw, { origin: originGcj, destination: destinationGcj });
  results.bicycling = {
    distanceM: bundle.distanceM, durationS: bundle.durationS,
    steps: bundle.steps.length, polyline: bundle.polyline.length, mode: bundle.mode,
  };
  console.log('\n  归一化结果:', JSON.stringify(results.bicycling));
  console.log('  ✅ 核对：maneuver 应已按 action 解析（实测骑行 v4 **有**中文 action）');
} catch (e) {
  console.log('  ❌ 失败:', red(e?.message));
  if (e?.info) console.log('     info=', e.info, 'infocode=', e.infocode);
}

line('汇总：字段名清单（供与代码引用比对）');
for (const [k, v] of Object.entries(results)) {
  console.log(`\n[${k}]`);
  const names = [...fieldNames(v)].sort();
  console.log('  ' + names.join('\n  '));
}
