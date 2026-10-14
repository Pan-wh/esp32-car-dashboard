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
import { fetchDrivingRoute, fetchGeocode } from './amap.js';
import { normalizeRoute, AmapError } from './normalize.js';
import { validLonLat } from './coords.js';

const MAX_BODY_BYTES = 8 * 1024; // 请求体上限：本接口只需两个坐标

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

  const handler = async (req, res) => {
    const url = new URL(req.url, 'http://localhost');

    if (req.method === 'GET' && url.pathname === '/health') {
      // 只暴露"是否已配置"，绝不回显 Key
      return sendJson(res, 200, { ok: true, hasKey: config.amapKey.length > 0 });
    }

    /* 只放行已知端点；其余一律 404（不暴露内部结构） */
    if (url.pathname !== '/route' && url.pathname !== '/geocode') {
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
  server.listen(config.port, () => {
    // 只打印端口，绝不打印 Key/token
    console.log(`[网关] 已监听 http://127.0.0.1:${config.port}`);
    console.log(`[网关] 上游 ${AMAP_DRIVING_URL}`);
    console.log(`[网关] 限流 ${config.rateLimitPerMin}/分钟；无缓存（协议 §3.5）`);
  });
  const shutdown = () => server.close(() => process.exit(0));
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}
