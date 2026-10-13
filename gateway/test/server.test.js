/**
 * server.test.js —— 网关 HTTP 层测试（无需联网、无需真实 Key）
 *
 * 用假 fetch 注入上游响应，覆盖：
 *   认证、限流、参数校验、请求体限制、上游错误透传、Key 不泄露。
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { createServer, hintFor } from '../src/server.js';
import { loadConfig, redact } from '../src/config.js';

const HERE = dirname(fileURLToPath(import.meta.url));
const SAMPLE = JSON.parse(readFileSync(join(HERE, 'fixtures/amap-driving-sample.json'), 'utf8'));

const TOKEN = 'test-token-123456';

/** 起一个临时服务，返回 { url, close } */
async function startServer({ config = {}, fetchImpl, log } = {}) {
  const cfg = {
    amapKey: 'FAKE_KEY_FOR_TEST',
    authToken: TOKEN,
    port: 0,
    rateLimitPerMin: 60,
    upstreamTimeoutMs: 2000,
    verboseErrors: false,
    ...config,
  };
  const server = createServer(cfg, {
    fetchImpl: fetchImpl ?? (async () => new Response(JSON.stringify(SAMPLE), { status: 200 })),
    log: log ?? (() => {}),
  });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const { port } = server.address();
  return {
    url: `http://127.0.0.1:${port}`,
    close: () => new Promise((r) => server.close(r)),
  };
}

const GOOD_BODY = {
  origin: { lon: 116.397428, lat: 39.90923 },
  destination: { lon: 116.481028, lat: 39.989643 },
};

function post(url, body, token = TOKEN) {
  return fetch(`${url}/route`, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      ...(token === null ? {} : { authorization: `Bearer ${token}` }),
    },
    body: typeof body === 'string' ? body : JSON.stringify(body),
  });
}

test('GET /health 正常，且不回显 Key', async () => {
  const s = await startServer();
  try {
    const r = await fetch(`${s.url}/health`);
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.ok, true);
    assert.equal(j.hasKey, true);
    assert.ok(!JSON.stringify(j).includes('FAKE_KEY'), 'health 不得泄露 Key');
  } finally {
    await s.close();
  }
});

test('★ 无 token / 错 token → 401（CORS 不是认证）', async () => {
  const s = await startServer();
  try {
    assert.equal((await post(s.url, GOOD_BODY, null)).status, 401);
    assert.equal((await post(s.url, GOOD_BODY, 'wrong')).status, 401);
    assert.equal((await post(s.url, GOOD_BODY, '')).status, 401);
  } finally {
    await s.close();
  }
});

test('POST /route 成功返回 RouteBundle', async () => {
  const s = await startServer();
  try {
    const r = await post(s.url, GOOD_BODY);
    assert.equal(r.status, 200);
    const b = await r.json();
    assert.equal(b.distanceM, 10234);
    assert.equal(b.durationS, 1200);
    assert.equal(b.steps.length, 5);
    // 回填的起终点应是 GCJ-02（与请求的 WGS84 不同）
    assert.notDeepEqual(b.origin, GOOD_BODY.origin, 'origin 应已转换为 GCJ-02');
    assert.ok(Math.abs(b.origin.lon - GOOD_BODY.origin.lon) < 0.01, '偏移量应合理');
  } finally {
    await s.close();
  }
});

test('响应带 cache-control: no-store（协议 §3.5 不缓存）', async () => {
  const s = await startServer();
  try {
    const r = await post(s.url, GOOD_BODY);
    assert.match(r.headers.get('cache-control') ?? '', /no-store/);
  } finally {
    await s.close();
  }
});

test('★ 发送给高德的坐标是 GCJ-02（不是原始 WGS84）', async () => {
  let seenUrl = null;
  const s = await startServer({
    fetchImpl: async (u) => {
      seenUrl = u;
      return new Response(JSON.stringify(SAMPLE), { status: 200 });
    },
  });
  try {
    await post(s.url, GOOD_BODY);
    assert.ok(seenUrl, '应调用上游');
    assert.equal(seenUrl.hostname, 'restapi.amap.com');
    const origin = seenUrl.searchParams.get('origin');
    assert.ok(origin, '应带 origin 参数');
    const [lon, lat] = origin.split(',').map(Number);
    // 与原始 WGS84 不同（说明转了），但偏移在合理范围（~百米级 → 0.00x 度）
    assert.notEqual(lon.toFixed(6), GOOD_BODY.origin.lon.toFixed(6));
    assert.ok(Math.abs(lon - GOOD_BODY.origin.lon) < 0.01);
    assert.ok(Math.abs(lat - GOOD_BODY.origin.lat) < 0.01);
    // 小数位不超过 6 位（高德要求）
    assert.match(origin, /^-?\d+\.\d{1,6},-?\d+\.\d{1,6}$/);
  } finally {
    await s.close();
  }
});

test('参数校验：缺坐标 / 非法坐标 → 400', async () => {
  const s = await startServer();
  try {
    assert.equal((await post(s.url, {})).status, 400);
    assert.equal((await post(s.url, { origin: { lon: 116, lat: 39 } })).status, 400);
    assert.equal(
      (await post(s.url, { origin: { lon: 999, lat: 39 }, destination: GOOD_BODY.destination })).status,
      400,
    );
    const j = await (await post(s.url, {})).json();
    assert.equal(j.error, 'invalid_coordinates');
    assert.ok(j.expect, '应说明期望格式');
  } finally {
    await s.close();
  }
});

test('请求体：坏 JSON → 400；过大 → 413', async () => {
  const s = await startServer();
  try {
    assert.equal((await post(s.url, '{not json')).status, 400);
    const huge = JSON.stringify({ origin: GOOD_BODY.origin, destination: GOOD_BODY.destination, pad: 'x'.repeat(9000) });
    assert.equal((await post(s.url, huge)).status, 413);
  } finally {
    await s.close();
  }
});

test('★ 上游业务错误 → 502 且透出 info/infocode（不含 Key）', async () => {
  const s = await startServer({
    fetchImpl: async () =>
      new Response(JSON.stringify({ status: '0', info: 'INVALID_USER_KEY', infocode: '10001' }), {
        status: 200,
      }),
  });
  try {
    const r = await post(s.url, GOOD_BODY);
    assert.equal(r.status, 502);
    const j = await r.json();
    assert.equal(j.error, 'amap_error');
    assert.equal(j.info, 'INVALID_USER_KEY');
    assert.equal(j.infocode, '10001');
    assert.ok(j.hint, '应给出排查提示');
    assert.ok(!JSON.stringify(j).includes('FAKE_KEY'), '错误响应不得泄露 Key');
  } finally {
    await s.close();
  }
});

test('上游网络异常 → 502，且默认不暴露细节', async () => {
  const s = await startServer({
    fetchImpl: async () => {
      throw new Error('connect ECONNREFUSED 1.2.3.4:443');
    },
  });
  try {
    const r = await post(s.url, GOOD_BODY);
    assert.equal(r.status, 502);
    const j = await r.json();
    assert.equal(j.error, 'upstream_failed');
    assert.equal(j.detail, undefined, '默认不应回传内部错误细节');
  } finally {
    await s.close();
  }
});

test('verboseErrors=1 时才回传细节', async () => {
  const s = await startServer({
    config: { verboseErrors: true },
    fetchImpl: async () => {
      throw new Error('boom');
    },
  });
  try {
    const j = await (await post(s.url, GOOD_BODY)).json();
    // fetchDrivingRoute 会把底层错误包装成「请求高德失败：…」，故用包含判断
    assert.match(j.detail ?? '', /boom/, 'verboseErrors=1 时应回传底层错误内容');
  } finally {
    await s.close();
  }
});

test('★ 日志里不出现 Key（redact 生效）', async () => {
  const logs = [];
  const s = await startServer({
    log: (...a) => logs.push(a.join(' ')),
    fetchImpl: async () => {
      throw new Error('upstream down: https://restapi.amap.com/v3/direction/driving?key=FAKE_KEY_FOR_TEST');
    },
  });
  try {
    await post(s.url, GOOD_BODY);
    const all = logs.join('\n');
    assert.ok(!all.includes('FAKE_KEY_FOR_TEST'), `日志泄露了 Key: ${all}`);
    assert.ok(all.includes('***'), '应出现脱敏占位符');
  } finally {
    await s.close();
  }
});

test('限流：超过阈值 → 429', async () => {
  const s = await startServer({ config: { rateLimitPerMin: 3 } });
  try {
    const codes = [];
    for (let i = 0; i < 5; i++) codes.push((await post(s.url, GOOD_BODY)).status);
    assert.equal(codes.filter((c) => c === 200).length, 3, `前 3 次应通过，实际 ${codes}`);
    assert.equal(codes[4], 429);
    const j = await (await post(s.url, GOOD_BODY)).json();
    assert.equal(j.error, 'rate_limited');
  } finally {
    await s.close();
  }
});

test('404 / 405 路由行为', async () => {
  const s = await startServer();
  try {
    assert.equal((await fetch(`${s.url}/nope`)).status, 404);
    assert.equal(
      (await fetch(`${s.url}/route`, { method: 'GET', headers: { authorization: `Bearer ${TOKEN}` } })).status,
      405,
    );
  } finally {
    await s.close();
  }
});

test('loadConfig：缺 Key / 缺 token → 启动即失败（快速失败）', () => {
  assert.throws(() => loadConfig({ GATEWAY_TOKEN: 'x' }, {}), /AMAP_KEY/);
  assert.throws(() => loadConfig({ AMAP_KEY: 'x' }, {}), /GATEWAY_TOKEN/);
  const c = loadConfig({ AMAP_KEY: 'k', GATEWAY_TOKEN: 't', PORT: '9999' });
  assert.equal(c.port, 9999);
  assert.equal(c.rateLimitPerMin, 60, '默认限流');
  // 测试场景可关闭必填
  assert.equal(loadConfig({}, { requireKey: false }).amapKey, '');
});

test('redact：遮蔽 key= / token= 参数（即便未显式传入 secrets）', () => {
  // URL 查询串
  assert.ok(!redact('https://x/y?key=SECRET123&a=1').includes('SECRET123'));
  assert.ok(!redact('https://x/y?a=1&token=SECRET456').includes('SECRET456'));
  // 字符串开头 / 空白之后（非 URL 场景）
  assert.ok(!redact('key=SECRET789').includes('SECRET789'));
  assert.ok(!redact('failed token=SECRET000 now').includes('SECRET000'));
  // 显式传入 secrets 时按原样替换（长度 >= 6 才替换，避免误伤）
  assert.ok(!redact('token=abcdef', ['abcdef']).includes('abcdef'));
  // 无秘密时原样返回
  assert.equal(redact('nothing', []), 'nothing');
});

test('hintFor：常见错误码有提示', () => {
  assert.match(hintFor('10001'), /Web 服务/);
  assert.match(hintFor('10003'), /配额/);
  assert.match(hintFor('99999'), /错误码说明/);
  assert.match(hintFor(10001), /Web 服务/, '数字码也应识别');
});
