/**
 * server.js —— 导航网关 HTTP 服务（Node 内置模块，零依赖）
 *
 * ## 接口
 *
 *   GET  /health          → { ok:true, hasKey:bool }（不泄露 Key 内容）
 *   POST /route           → 路线（RouteBundle）
 *
 * ## 安全设计（对照 docs/nav-backend.md §3.5）
 *
 *   1. **要求 Bearer Token** —— CORS 不是认证；没有 token 一律 401。
 *   2. **Key 只存服务端**，任何错误响应与日志都不包含它（用 redact 兜底）。
 *   3. **限流** —— 防止误用刷爆高德配额。
 *   4. **不缓存** —— 协议 §3.5 禁止存储/缓存服务数据；本服务无任何缓存层。
 *   5. **请求体限制** —— 防止超大 body 打爆内存。
 *
 * ## 可测试性
 *
 * `deps.fetchImpl` 可注入；`createServer` 返回未监听的 http.Server，
 * 测试里 listen(0) 取随机端口即可。
 */
import http from 'node:http';

import { loadConfig, redact, AMAP_DRIVING_URL } from './config.js';
import {
  fetchDrivingRoute,
  fetchGeocode,
  fetchRegeo,
  fetchTrafficRectangle,
  fetchPlaceAround,
  fetchInputTips,
  fetchBicyclingRoute,
} from './amap.js';
import { fetchMapWindow } from './mapwindow.js';
import { normalizeRoute, normalizeBicycling, AmapError } from './normalize.js';
import { validLonLat } from './coords.js';

const MAX_BODY_BYTES = 8 * 1024; // 请求体上限：本接口只需两个坐标

/**
 * 已知端点白名单。**集中一处**定义，避免"加了新分支却忘了放行"
 * （旧写法在两处各列一遍路径，新增接口时极易漏改一处，表现是 404）。
 */
const KNOWN_PATHS = new Set([
  '/route',
  '/geocode',
  '/mapwindow',
  '/regeo',
  '/traffic',
  '/around',
  '/tips',
  '/bicycling',
]);

/** 简易滑窗限流（单进程内存态；自用规模足够） */
function createRateLimiter(perMin) {
  const hits = new Map();
  return function allow(id, now = Date.now()) {
    const win = 60_000;
    const arr = (hits.get(id) ?? []).filter((t) => now - t < win);
    if (arr.length >= perMin) {
      hits.set(id, arr);
      return false;
    }
    arr.push(now);
    hits.set(id, arr);
    return true;
  };
}

function sendJson(res, status, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': Buffer.byteLength(body),
    'cache-control': 'no-store', // 明确禁止中间层缓存（协议 §3.5）
  });
  res.end(body);
}

async function readBody(req) {
  const chunks = [];
  let n = 0;
  for await (const c of req) {
    n += c.length;
    if (n > MAX_BODY_BYTES) {
      const err = new Error('请求体过大');
      err.code = 'BODY_TOO_LARGE';
      throw err;
    }
    chunks.push(c);
  }
  const text = Buffer.concat(chunks).toString('utf8');
  if (text.trim().length === 0) return {};
  try {
    return JSON.parse(text);
  } catch {
    const err = new Error('请求体不是合法 JSON');
    err.code = 'BAD_JSON';
    throw err;
  }
}

/**
 * 创建服务（不监听）。
 * @param {object} config  loadConfig() 的结果
 * @param {object} [deps]  可注入 { fetchImpl, now, log }
 */
export function createServer(config, deps = {}) {
  const fetchImpl = deps.fetchImpl ?? globalThis.fetch;
  const log = deps.log ?? ((...a) => console.log(...a));
  const allow = createRateLimiter(config.rateLimitPerMin);
  const secrets = [config.amapKey, config.authToken];

  /**
   * 统一的上游错误响应。
   *
   * 为什么要抽出来：新增的 5 个接口都要做同样的事 ——
   * 高德业务错误（AmapError）透出 info/infocode/hint，其它错误脱敏后按需透出。
   * 复制五遍必然漏掉某一处的 redact（那等于泄露 Key），故集中一处。
   */
  const sendUpstreamError = (res, e) => {
    if (e instanceof AmapError) {
      log(`[上游错误] ${redact(e.message, secrets)}`);
      return sendJson(res, 502, {
        error: 'amap_error', info: e.info, infocode: e.infocode, hint: hintFor(e.infocode),
      });
    }
    const msg = redact(e?.message ?? String(e), secrets);
    log(`[网关错误] ${msg}`);
    return sendJson(res, 502, {
      error: 'upstream_failed',
      ...(config.verboseErrors ? { detail: msg } : {}),
    });
  };

  /*
   * ── 高德调用计数（用户关心"免费额度够不够用"）──────────────────────
   *
   * **只统计真正打到高德的请求**（路线 + 地理编码）。周边地图走 OpenFreeMap，
   * **完全不消耗高德额度** —— 这是选它做环境图层的原因之一。
   *
   * 按"自然日"分桶，跨天自动归零（高德的日配额也是 0:00 重置）。
   * 通过 GET /health 暴露，方便随时查看自己的消耗。
   */
  const usage = {
    day: '',
    route: 0,
    geocode: 0,
    mapwindow: 0,
    /* 以下为新增接口；均消耗高德额度（mapwindow 不消耗）*/
    regeo: 0,
    traffic: 0,
    around: 0,
    tips: 0,
    bicycling: 0,
  };
  const today = () => new Date().toISOString().slice(0, 10);
  /** 跨天则清零（高德的日配额也是 0:00 重置，对齐它的口径）*/
  const ensureDay = () => {
    const d = today();
    if (usage.day !== d) {
      usage.day = d;
      for (const k of Object.keys(usage)) {
        if (k !== 'day') usage[k] = 0;
      }
    }
    return d;
  };
  const bump = (kind) => {
    ensureDay();
    usage[kind] += 1;
  };

  const handler = async (req, res) => {
    const url = new URL(req.url, 'http://localhost');

    if (req.method === 'GET' && url.pathname === '/health') {
      // 只暴露"是否已配置"，绝不回显 Key
      ensureDay(); /* 跨天先归零，保证显示的是"今天"的量 */
      /* 消耗高德额度的接口（mapwindow 走 OSM，不计入）*/
      const amapTotal =
        usage.route + usage.geocode + usage.regeo + usage.traffic +
        usage.around + usage.tips + usage.bicycling;
      return sendJson(res, 200, {
        ok: true,
        hasKey: config.amapKey.length > 0,
        /* 今日高德实际消耗（地图窗口不消耗高德额度）*/
        amapCallsToday: {
          date: usage.day,
          route: usage.route,
          geocode: usage.geocode,
          regeo: usage.regeo,
          traffic: usage.traffic,
          around: usage.around,
          tips: usage.tips,
          bicycling: usage.bicycling,
          total: amapTotal,
        },
        /* 仅作参考：本网关自身处理的请求数 */
        mapWindowRequestsToday: usage.mapwindow,
      });
    }

    /* 只放行已知端点；其余一律 404（不暴露内部结构） */
    if (!KNOWN_PATHS.has(url.pathname)) {
      return sendJson(res, 404, { error: 'not_found' });
    }
    if (req.method !== 'POST') {
      return sendJson(res, 405, { error: 'method_not_allowed' });
    }

    // ---- 认证：CORS 不是认证 ----
    const auth = req.headers['authorization'] ?? '';
    const token = auth.startsWith('Bearer ') ? auth.slice(7) : '';
    if (token !== config.authToken || config.authToken.length === 0) {
      return sendJson(res, 401, { error: 'unauthorized' });
    }

    // ---- 限流 ----
    if (!allow(token)) {
      return sendJson(res, 429, { error: 'rate_limited', limitPerMin: config.rateLimitPerMin });
    }

    // ---- 解析请求体 ----
    let body;
    try {
      body = await readBody(req);
    } catch (e) {
      const code = e.code === 'BODY_TOO_LARGE' ? 413 : 400;
      return sendJson(res, code, { error: e.code ?? 'bad_request' });
    }

    // ---- 周边地图窗口：当前位置 ±500m 的道路/建筑几何（画圆屏底图用）----
    if (url.pathname === '/mapwindow') {
      const lon = body.lon;
      const lat = body.lat;
      if (!Number.isFinite(lon) || !Number.isFinite(lat) ||
          lon < -180 || lon > 180 || lat < -90 || lat > 90) {
        return sendJson(res, 400, {
          error: 'invalid_center',
          expect: { lon: 'WGS84 经度', lat: 'WGS84 纬度', radiusM: '可选，默认 500' },
        });
      }
      const radiusM = Number.isFinite(body.radiusM)
        ? Math.min(Math.max(body.radiusM, 100), 1500) : 500;
      try {
        bump('mapwindow'); /* 不消耗高德额度 */
        const w = await fetchMapWindow({
          lon, lat, radiusM, cacheDir: config.mapCacheDir, fetchImpl,
        });
        return sendJson(res, 200, w);
      } catch (e) {
        const msg = redact(e?.message ?? String(e), secrets);
        log(`[地图窗口错误] ${msg}`);
        return sendJson(res, 502, {
          error: 'mapwindow_failed',
          ...(config.verboseErrors ? { detail: msg } : {}),
        });
      }
    }

    // ---- 地理编码：地址文本 → 坐标（让目的地能填中文地名）----
    if (url.pathname === '/geocode') {
      const address = typeof body.address === 'string' ? body.address.trim() : '';
      if (address.length === 0 || address.length > 120) {
        return sendJson(res, 400, {
          error: 'invalid_address',
          expect: { address: '非空字符串（≤120 字）', city: '可选，限定城市' },
        });
      }
      try {
        bump('geocode');
        const { results } = await fetchGeocode({
          key: config.amapKey,
          address,
          city: typeof body.city === 'string' ? body.city.trim() : '',
          timeoutMs: config.upstreamTimeoutMs,
          fetchImpl,
        });
        // 坐标已在 amap.js 里转成 **WGS84**（客户端约定）
        return sendJson(res, 200, { results });
      } catch (e) {
        if (e instanceof AmapError) {
          log(`[上游错误] ${redact(e.message, secrets)}`);
          return sendJson(res, 502, {
            error: 'amap_error', info: e.info, infocode: e.infocode, hint: hintFor(e.infocode),
          });
        }
        const msg = redact(e?.message ?? String(e), secrets);
        log(`[网关错误] ${msg}`);
        return sendJson(res, 502, {
          error: 'upstream_failed',
          ...(config.verboseErrors ? { detail: msg } : {}),
        });
      }
    }

    // ---- 逆地理编码：坐标 → 当前道路名（仪表显示"在XX路"）----
    if (url.pathname === '/regeo') {
      if (!validLonLat(body)) {
        return sendJson(res, 400, {
          error: 'invalid_coordinates',
          expect: { lon: 'WGS84 经度', lat: 'WGS84 纬度', radius: '可选，米' },
        });
      }
      try {
        bump('regeo');
        const info = await fetchRegeo({
          key: config.amapKey,
          point: { lon: body.lon, lat: body.lat },
          radius: Number.isFinite(body.radius) ? body.radius : 0,
          timeoutMs: config.upstreamTimeoutMs,
          fetchImpl,
        });
        return sendJson(res, 200, info);
      } catch (e) {
        return sendUpstreamError(res, e);
      }
    }

    // ---- 交通态势：当前位置周边拥堵情况（⚠️ 不是红绿灯）----
    if (url.pathname === '/traffic') {
      if (!validLonLat(body)) {
        return sendJson(res, 400, {
          error: 'invalid_coordinates',
          expect: {
            lon: 'WGS84 经度', lat: 'WGS84 纬度',
            halfMeters: '可选，默认 500（即 1km×1km 区域）',
            level: '可选，高德道路等级，默认 6=所有道路',
          },
        });
      }
      /* 半边长限幅：太大既没意义又会拖慢上游；太小可能取不到任何路 */
      const halfMeters = Number.isFinite(body.halfMeters)
        ? Math.min(Math.max(body.halfMeters, 100), 3000) : 500;
      try {
        bump('traffic');
        const info = await fetchTrafficRectangle({
          key: config.amapKey,
          center: { lon: body.lon, lat: body.lat },
          halfMeters,
          level: Number.isFinite(body.level) ? body.level : 6,
          timeoutMs: config.upstreamTimeoutMs,
          fetchImpl,
        });
        return sendJson(res, 200, info);
      } catch (e) {
        return sendUpstreamError(res, e);
      }
    }

    // ---- 周边搜索：加油站 / 服务区等 ----
    if (url.pathname === '/around') {
      if (!validLonLat(body)) {
        return sendJson(res, 400, {
          error: 'invalid_coordinates',
          expect: {
            lon: 'WGS84 经度', lat: 'WGS84 纬度',
            keywords: '可选，如「加油站」「服务区」',
            radius: '可选，默认 3000 米',
          },
        });
      }
      const keywords = typeof body.keywords === 'string' ? body.keywords.trim() : '';
      if (keywords.length > 60) {
        return sendJson(res, 400, { error: 'invalid_keywords', expect: { keywords: '≤60 字' } });
      }
      try {
        bump('around');
        const { pois } = await fetchPlaceAround({
          key: config.amapKey,
          center: { lon: body.lon, lat: body.lat },
          keywords,
          radius: Number.isFinite(body.radius)
            ? Math.min(Math.max(body.radius, 100), 50000) : 3000,
          limit: Number.isFinite(body.limit) ? Math.min(Math.max(body.limit, 1), 25) : 20,
          timeoutMs: config.upstreamTimeoutMs,
          fetchImpl,
        });
        return sendJson(res, 200, { pois });
      } catch (e) {
        return sendUpstreamError(res, e);
      }
    }

    // ---- 输入提示：搜索框地址联想 ----
    if (url.pathname === '/tips') {
      const keywords = typeof body.keywords === 'string' ? body.keywords.trim() : '';
      if (keywords.length === 0 || keywords.length > 60) {
        return sendJson(res, 400, {
          error: 'invalid_keywords',
          expect: { keywords: '非空字符串（≤60 字）', city: '可选，限定城市' },
        });
      }
      try {
        bump('tips');
        const { tips } = await fetchInputTips({
          key: config.amapKey,
          keywords,
          city: typeof body.city === 'string' ? body.city.trim() : '',
          limit: Number.isFinite(body.limit) ? Math.min(Math.max(body.limit, 1), 20) : 10,
          timeoutMs: config.upstreamTimeoutMs,
          fetchImpl,
        });
        return sendJson(res, 200, { tips });
      } catch (e) {
        return sendUpstreamError(res, e);
      }
    }

    // ---- 骑行路线（结构与驾车不同，单独归一化；maneuver 恒为「未知」）----
    if (url.pathname === '/bicycling') {
      if (!validLonLat(body.origin) || !validLonLat(body.destination)) {
        return sendJson(res, 400, {
          error: 'invalid_coordinates',
          expect: { origin: '{lon,lat}', destination: '{lon,lat}', note: 'WGS84 坐标' },
        });
      }
      try {
        bump('bicycling');
        const { raw, originGcj, destinationGcj } = await fetchBicyclingRoute({
          key: config.amapKey,
          origin: body.origin,
          destination: body.destination,
          timeoutMs: config.upstreamTimeoutMs,
          fetchImpl,
        });
        const bundle = normalizeBicycling(raw, {
          origin: originGcj,
          destination: destinationGcj,
          routeId: typeof body.routeId === 'string' ? body.routeId : null,
        });
        return sendJson(res, 200, bundle);
      } catch (e) {
        return sendUpstreamError(res, e);
      }
    }

    const origin = body.origin;
    const destination = body.destination;
    if (!validLonLat(origin) || !validLonLat(destination)) {
      // 明确说明期望格式，避免调用方猜
      return sendJson(res, 400, {
        error: 'invalid_coordinates',
        expect: { origin: '{lon,lat}', destination: '{lon,lat}', note: 'WGS84 坐标' },
      });
    }

    // ---- 调用上游 ----
    try {
      bump('route'); /* 真正打到高德了才计数 */
      const { raw, originGcj, destinationGcj } = await fetchDrivingRoute({
        key: config.amapKey,
        origin,
        destination,
        strategy: Number.isFinite(body.strategy) ? body.strategy : 0,
        timeoutMs: config.upstreamTimeoutMs,
        fetchImpl,
      });

      const bundle = normalizeRoute(raw, {
        origin: originGcj,
        destination: destinationGcj,
        routeId: typeof body.routeId === 'string' ? body.routeId : null,
      });

      return sendJson(res, 200, bundle);
    } catch (e) {
      // 上游业务错误：把 info/infocode 原样透出，便于排查（不含 Key）
      if (e instanceof AmapError) {
        log(`[上游错误] ${redact(e.message, secrets)}`);
        return sendJson(res, 502, {
          error: 'amap_error',
          info: e.info,
          infocode: e.infocode,
          hint: hintFor(e.infocode),
        });
      }
      const msg = redact(e?.message ?? String(e), secrets);
      log(`[网关错误] ${msg}`);
      return sendJson(res, 502, {
        error: 'upstream_failed',
        ...(config.verboseErrors ? { detail: msg } : {}),
      });
    }
  };

  return http.createServer((req, res) => {
    handler(req, res).catch((e) => {
      log(`[未捕获] ${redact(e?.message ?? String(e), secrets)}`);
      if (!res.headersSent) sendJson(res, 500, { error: 'internal' });
      else res.end();
    });
  });
}

/** 常见高德错误码的排查提示（降低踩坑成本） */
export function hintFor(infocode) {
  switch (String(infocode)) {
    case '10001':
      return 'Key 不正确或已停用：确认用的是「Web 服务」类型的 Key（docs/nav-backend.md §3.2）';
    case '10003':
      return '配额已用尽：查看高德控制台用量，或提高 RATE_LIMIT_PER_MIN 之外的配额';
    case '10004':
      return '访问过于频繁（QPS 超限）：降低调用频率';
    case '10009':
      return 'Key 与绑定的平台不匹配：该 Key 必须绑定「Web 服务」';
    case '10012':
      return '权限不足：可能是 Key 绑定的服务不含路径规划';
    case '20003':
      return '参数错误：检查 origin/destination 是否为 lon,lat 且不超过 6 位小数';
    default:
      return '详见高德错误码说明：https://lbs.amap.com/api/webservice/guide/tools/info';
  }
}

/* 直接运行时启动服务；被 import 时不启动（便于测试） */
const isMain = process.argv[1] && import.meta.url === `file://${process.argv[1]}`;
if (isMain) {
  const config = loadConfig();
  const server = createServer(config);
  server.listen(config.port, config.host, () => {
    // 只打印监听地址与端口，绝不打印 Key/token
    console.log(`[网关] 已监听 http://${config.host}:${config.port}`);
    /*
     * 直白提示当前暴露面 —— 这是最容易被忽略的安全项：
     * 绑 0.0.0.0 意味着任何能路由到本机的人都可访问（需 token，但仍是暴露）。
     */
    if (config.host === '0.0.0.0' || config.host === '::') {
      console.log('[网关] ⚠️ 正在监听所有网卡（HOST=0.0.0.0）—— 请确认这是你要的暴露面');
    } else {
      console.log(`[网关] 仅本机可访问（HOST=${config.host}）；如需外部访问请用反向代理`);
    }
    console.log(`[网关] 上游 ${AMAP_DRIVING_URL}`);
    console.log(`[网关] 限流 ${config.rateLimitPerMin}/分钟；无缓存（协议 §3.5）`);
  });
  const shutdown = () => server.close(() => process.exit(0));
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}
