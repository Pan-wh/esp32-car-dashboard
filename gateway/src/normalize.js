/**
 * normalize.js —— 高德响应 → 本项目 RouteBundle（纯逻辑，可单测）
 *
 * ## 设计原则
 *
 * 0. **字段名以实测为准**（2026-10-13 用真实 Key 核对，见 fixtures/amap-driving-real.json）：
 *    `steps[].distance` / `steps[].road` / `steps[].action`（中文文本）
 * 1. **宽容解析**：高德把数字也返回成字符串（如 `"distance": "12345"`），
 *    且不同接口版本字段层级略有差异。这里统一容错，但**绝不把无法解析的值当成 0**
 *    （距离为 0 会让仪表显示"即将转向"，属危险误导）→ 解析失败记为 `null`。
 * 2. **错误显式**：高德用 `status`/`info`/`infocode` 表达失败，必须原样透出，
 *    不能当成"空路线"静默处理。
 * 3. **不缓存**：协议 §3.5 禁止直接存储/缓存服务数据 —— 本模块纯函数，
 *    不落盘不记忆；调用方（server）也不得缓存。
 *
 * ## 输出结构（RouteBundle）
 *
 * ```js
 * {
 *   routeId, distanceM, durationS,
 *   origin: {lon,lat}, destination: {lon,lat},     // GCJ-02
 *   steps: [{ index, maneuver, distanceM, roadName, instruction, polyline }],
 *   polyline: [{lon,lat}, ...]                      // 各 step 拼接
 * }
 * ```
 */
import { resolveManeuver, MANEUVER } from './maneuver.js';

/**
 * 宽松转数字。
 *
 * ⚠️ 与 `Number(x) || 0` 的区别：无法解析时返回 **null** 而不是 0。
 * 距离/时长若被当成 0，会造成"即将转向""剩余 0 分钟"这类误导。
 */
export function toNumberOrNull(v) {
  if (v === null || v === undefined || v === '') return null;
  const n = Number(v);
  return Number.isFinite(n) ? n : null;
}

/** 取第一个非空字符串（用于兼容不同版本的字段名） */
export function firstNonEmptyString(...vals) {
  for (const v of vals) {
    if (typeof v === 'string' && v.trim().length > 0) return v;
  }
  return null;
}

/** 把 "lon,lat;lon,lat;..." 解析为点数组；非法项跳过（不抛错，避免整条路线失败） */
export function parsePolyline(s) {
  if (typeof s !== 'string' || s.length === 0) return [];
  const pts = [];
  for (const seg of s.split(';')) {
    const parts = seg.split(',');
    if (parts.length !== 2) continue;
    const lon = Number(parts[0]);
    const lat = Number(parts[1]);
    if (!Number.isFinite(lon) || !Number.isFinite(lat)) continue;
    if (lon < -180 || lon > 180 || lat < -90 || lat > 90) continue;
    pts.push({ lon, lat });
  }
  return pts;
}

/** 高德业务错误（status !== "1"）时构造的异常 */
export class AmapError extends Error {
  constructor(info, infocode, raw) {
    super(`高德返回错误: ${info ?? '未知'} (infocode=${infocode ?? '无'})`);
    this.name = 'AmapError';
    this.info = info;
    this.infocode = infocode;
    this.raw = raw;
  }
}

/**
 * 校验高德响应的业务状态。
 * @returns {object} 校验通过的原始响应
 * @throws {AmapError} status !== "1"
 */
export function assertAmapOk(json) {
  if (json == null || typeof json !== 'object') {
    throw new AmapError('响应不是 JSON 对象', null, json);
  }
  // 高德成功时 status 为字符串 "1"（个别接口为数字 1，一并接受）
  if (json.status === '1' || json.status === 1) return json;
  throw new AmapError(json.info, json.infocode, json);
}

/** 取路径数组（v3/v5 均为 route.paths[]）；缺失返回空数组 */
function pathsOf(json) {
  const paths = json?.route?.paths;
  return Array.isArray(paths) ? paths : [];
}

/**
 * 归一化一条路径。
 * @param {object} path   高德 route.paths[i]
 * @param {number} index
 */
function normalizePath(path, index) {
  const rawSteps = Array.isArray(path?.steps) ? path.steps : [];

  const steps = rawSteps.map((s, i) => {
    // ⚠️ 字段名以**真实响应实测**为准（2026-10-13，v3 驾车路径规划）：
    //      距离 = steps[].distance      （不是 step_distance）
    //      路名 = steps[].road          （不是 road_name）
    //      动作 = steps[].action        （**中文字符串**，如 "左转"）
    //    同时兼容旧名/其它版本，避免上游小改就整条路线失效。
    const rawAction = s?.navi?.action ?? s?.action;
    return {
      index: i,
      maneuver: resolveManeuver({
        action: rawAction,
        instruction: s?.instruction,
        assistant_action: s?.assistant_action,
      }),
      distanceM: toNumberOrNull(s?.distance ?? s?.step_distance),
      roadName: firstNonEmptyString(s?.road, s?.road_name),
      instruction: firstNonEmptyString(s?.instruction),
      durationS: toNumberOrNull(s?.duration),
      polyline: parsePolyline(s?.polyline),
    };
  });

  // 全路线折线：各 step 拼接（相邻 step 首尾点常重复，去重以免下游误判长度）
  const polyline = [];
  for (const st of steps) {
    for (const p of st.polyline) {
      const last = polyline[polyline.length - 1];
      if (last && last.lon === p.lon && last.lat === p.lat) continue;
      polyline.push(p);
    }
  }

  return {
    index,
    // 距离/时长：优先路径级；缺失时用 step 累加（v5 的分布位置可能不同）
    distanceM:
      toNumberOrNull(path?.distance) ??
      (steps.length > 0
        ? steps.reduce((a, s) => a + (s.distanceM ?? 0), 0)
        : null),
    durationS: toNumberOrNull(path?.duration) ?? toNumberOrNull(path?.cost?.duration),
    steps,
    polyline,
  };
}

/**
 * 归一化高德驾车路径规划响应。
 *
 * @param {object} json  高德原始响应（已 JSON.parse）
 * @param {{origin:{lon,lat}, destination:{lon,lat}, routeId?:string}} meta
 *        起终点应为**已转换到 GCJ-02** 的坐标（用于回填，便于手机端校验）
 * @returns {object} RouteBundle
 * @throws {AmapError} 业务失败；或没有任何可用路径时抛错（不返回空壳）
 */
export function normalizeRoute(json, meta = {}) {
  assertAmapOk(json);

  const paths = pathsOf(json);
  if (paths.length === 0) {
    throw new AmapError('响应中没有可用路径（paths 为空）', json.infocode, json);
  }

  const first = normalizePath(paths[0], 0);

  if (first.polyline.length === 0 && first.steps.length === 0) {
    throw new AmapError('路径既无折线也无步骤，视为无效', json.infocode, json);
  }

  return {
    routeId: meta.routeId ?? null,
    distanceM: first.distanceM,
    durationS: first.durationS,
    origin: meta.origin ?? null,
    destination: meta.destination ?? null,
    steps: first.steps,
    polyline: first.polyline,
    // 备用路线数量（手机端可提供"换一条"）
    alternativeCount: paths.length - 1,
  };
}

/**
 * 当前步骤：给定"已行驶距离"推断处于第几个 step，并返回距下一步的距离。
 *
 * 放在网关侧是因为它是**纯计算**、可单测；实际导航时由手机端用实时位置计算，
 * 此处主要供测试与调试使用（也可作为手机端实现的参考）。
 *
 * @param {object} bundle       normalizeRoute 的结果
 * @param {number} traveledM    已沿路线行驶的距离（米）
 * @returns {{stepIndex:number, maneuver:number, distanceToManeuverM:number|null}}
 */
export function stepAt(bundle, traveledM) {
  const steps = bundle?.steps ?? [];
  if (steps.length === 0) {
    return { stepIndex: -1, maneuver: MANEUVER.UNKNOWN, distanceToManeuverM: null };
  }
  let acc = 0;
  for (let i = 0; i < steps.length; i++) {
    const d = steps[i].distanceM;
    if (d === null) continue; // 缺距离的 step 跳过（不当作 0）
    if (traveledM < acc + d) {
      return {
        stepIndex: i,
        maneuver: steps[i].maneuver,
        distanceToManeuverM: Math.max(0, Math.round(acc + d - traveledM)),
      };
    }
    acc += d;
  }
  // 已走完全部步骤 → 到达
  return { stepIndex: steps.length - 1, maneuver: MANEUVER.ARRIVE, distanceToManeuverM: 0 };
}

/**
 * 归一化高德**骑行**路径规划响应（v4）。
 *
 * ## 与驾车 v3 的差异（这是必须单独写一个函数的原因）
 *
 * | 项 | 驾车 v3 | 骑行 v4 |
 * |----|---------|---------|
 * | 路径位置 | `route.paths[]` | `data.paths[]` |
 * | 状态字段 | `status: "1"` | `errcode: 0` |
 * | 距离 | `paths[].distance` | `data.paths[].distance` |
 * | 时长 | `paths[].duration` | `data.paths[].duration` |
 * | 转向 | `steps[].action`（中文） | `steps[].action`（中文，**同样存在**）|
 *
 * ⚠️ **实测更正（2026-10-15，真实 Key）**：原先以为骑行不给 `action`，
 *    故写死 maneuver=UNKNOWN。真实响应里 `steps[].action` 是 `"右转"` 这样的
 *    中文文本，与驾车一致 —— 因此**复用同一套 `resolveManeuver` 码表**。
 *    （若当初按猜测写死 UNKNOWN，仪表在骑行模式下会完全不显示转向提示。）
 *
 * @param {object} json  高德原始响应（已 JSON.parse）
 * @param {{origin, destination, routeId?}} meta 起终点应为**已转 GCJ-02** 的坐标
 * @returns {object} RouteBundle（带 mode='bicycling' 标记）
 * @throws {AmapError} 业务失败；或没有任何可用路径
 */
export function normalizeBicycling(json, meta = {}) {
  /*
   * ⚠️ 骑行 v4 用 `errcode: 0` 表示成功，**没有 `status` 字段**；
   *    直接调 assertAmapOk 会把成功响应当成失败（实测踩过）。
   */
  if (json?.errcode !== undefined && json?.errcode !== null) {
    if (Number(json.errcode) !== 0) {
      throw new AmapError(json.errmsg ?? json.errdetail, String(json.errcode), json);
    }
  } else {
    assertAmapOk(json);
  }

  const paths = Array.isArray(json?.data?.paths) ? json.data.paths : [];
  if (paths.length === 0) {
    throw new AmapError('骑行响应中没有可用路径（data.paths 为空）', json.infocode, json);
  }

  const p = paths[0];
  const rawSteps = Array.isArray(p?.steps) ? p.steps : [];

  const steps = rawSteps.map((s, i) => ({
    index: i,
    /*
     * 与驾车同样解析 `action`（实测骑行 v4 也返回中文动作文本）。
     * `road` 字段实测存在（如 "北湾子胡同"）。
     */
    maneuver: resolveManeuver({
      action: s?.navi?.action ?? s?.action,
      instruction: s?.instruction,
      assistant_action: s?.assistant_action,
    }),
    distanceM: toNumberOrNull(s?.distance),
    roadName: firstNonEmptyString(s?.road, s?.road_name),
    instruction: firstNonEmptyString(s?.instruction),
    durationS: toNumberOrNull(s?.duration),
    polyline: parsePolyline(s?.polyline),
  }));

  // 拼接完整折线（相邻重复点去重）
  const polyline = [];
  for (const st of steps) {
    for (const pt of st.polyline) {
      const last = polyline[polyline.length - 1];
      if (last && last.lon === pt.lon && last.lat === pt.lat) continue;
      polyline.push(pt);
    }
  }

  if (polyline.length === 0 && steps.length === 0) {
    throw new AmapError('骑行路径既无折线也无步骤，视为无效', json.infocode, json);
  }

  return {
    routeId: meta.routeId ?? null,
    distanceM:
      toNumberOrNull(p?.distance) ??
      (steps.length > 0 ? steps.reduce((a, s) => a + (s.distanceM ?? 0), 0) : null),
    durationS: toNumberOrNull(p?.duration),
    origin: meta.origin ?? null,
    destination: meta.destination ?? null,
    steps,
    polyline,
    alternativeCount: paths.length - 1,
    /** 标记这是骑行路线，便于客户端区分展示（驾车接口不返回此字段） */
    mode: 'bicycling',
  };
}
