/**
 * new-endpoints.test.js —— 新增 5 个接口的 HTTP 层测试
 *
 * 覆盖：/regeo、/traffic、/around、/tips、/bicycling
 *
 * 全部**离线**：用注入的假 fetch 提供上游响应，不需要真实 Key、不联网。
 * 断言重点是那些**容易出错且后果明确**的地方：
 *   - 坐标是否真的转成了 GCJ-02 再发给高德（转错会偏移数百米）
 *   - 返回给客户端的坐标是否转回了 WGS84（不转会双重转换）
 *   - 参数校验、限幅是否生效
 *   - Key 是否泄露
 *   - 骑行接口是否复用驾车码表（不应复用 —— 骑行不该给转向指令）
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import { createServer } from '../src/server.js';

const TOKEN = 'test-token-123456';

/** 起临时服务；`fake` 为假 fetch 响应体（对象）或函数 */
async function startServer(fake, { config = {} } = {}) {
  const cfg = {
    amapKey: 'FAKE_KEY_FOR_TEST',
    authToken: TOKEN,
    port: 0,
    rateLimitPerMin: 200,
    upstreamTimeoutMs: 2000,
    verboseErrors: false,
    ...config,
  };
  const fetchImpl = typeof fake === 'function'
    ? fake
    : async () => new Response(JSON.stringify(fake), { status: 200 });
  const server = createServer(cfg, { fetchImpl, log: () => {} });
  await new Promise((r) => server.listen(0, '127.0.0.1', r));
  const { port } = server.address();
  return {
    url: `http://127.0.0.1:${port}`,
    close: () => new Promise((r) => server.close(r)),
  };
}

function post(url, path, body, token = TOKEN) {
  return fetch(`${url}${path}`, {
    method: 'POST',
    headers: {
      'content-type': 'application/json',
      ...(token === null ? {} : { authorization: `Bearer ${token}` }),
    },
    body: JSON.stringify(body),
  });
}

/** WGS84 北京某点 */
const P = { lon: 116.397428, lat: 39.90923 };

// ───────────────────────── /regeo ─────────────────────────

const REGEO_OK = {
  status: '1',
  info: 'OK',
  infocode: '10000',
  regeocode: {
    formatted_address: '北京市东城区东长安街',
    addressComponent: {
      province: '北京市',
      city: '北京市',
      district: '东城区',
      road: '东长安街',
      streetNumber: { street: '东长安街', number: '1号' },
    },
  },
};

test('POST /regeo 正常返回道路名与地址', async () => {
  const s = await startServer(REGEO_OK);
  try {
    const r = await post(s.url, '/regeo', P);
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.road, '东长安街');
    assert.equal(j.formattedAddress, '北京市东城区东长安街');
    assert.equal(j.city, '北京市');
    assert.equal(j.district, '东城区');
  } finally {
    await s.close();
  }
});

test('★ /regeo：发给高德的坐标是 GCJ-02（不是原始 WGS84）', async () => {
  let seenUrl = null;
  const s = await startServer(async (u) => {
    seenUrl = new URL(u);
    return new Response(JSON.stringify(REGEO_OK), { status: 200 });
  });
  try {
    await post(s.url, '/regeo', P);
    assert.ok(seenUrl, '应当调用过上游');
    assert.equal(seenUrl.hostname, 'restapi.amap.com');
    const loc = seenUrl.searchParams.get('location');
    const [lon, lat] = loc.split(',').map(Number);
    // 北京地区 GCJ-02 相对 WGS84 偏移量级约 100~700 米（0.001~0.007 度）
    const dLon = Math.abs(lon - P.lon);
    const dLat = Math.abs(lat - P.lat);
    assert.ok(dLon > 0.0005 && dLon < 0.02, `经度偏移应量级正确，实际 ${dLon}`);
    assert.ok(dLat > 0.00005 && dLat < 0.02, `纬度偏移应量级正确，实际 ${dLat}`);
  } finally {
    await s.close();
  }
});

test('/regeo：缺坐标 → 400，且带 expect 说明', async () => {
  const s = await startServer(REGEO_OK);
  try {
    const r = await post(s.url, '/regeo', { lon: 999, lat: 39 });
    assert.equal(r.status, 400);
    const j = await r.json();
    assert.equal(j.error, 'invalid_coordinates');
    assert.ok(j.expect, '应说明期望格式');
  } finally {
    await s.close();
  }
});

test('★ /regeo：road 为**空数组**时（高德无路名的实际返回）降级为 null，不崩', async () => {
  // 高德在无道路名时返回空数组而不是空字符串 —— 这是实测踩过的坑
  const noRoad = {
    status: '1',
    regeocode: {
      formatted_address: '某地',
      addressComponent: { road: [], streetNumber: { street: [] } },
    },
  };
  const s = await startServer(noRoad);
  try {
    const r = await post(s.url, '/regeo', P);
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.road, null, '无路名应为 null，而不是数组或字符串 "undefined"');
  } finally {
    await s.close();
  }
});

// ───────────────────────── /traffic ─────────────────────────

const TRAFFIC_OK = {
  status: '1',
  info: 'OK',
  infocode: '10000',
  trafficinfo: {
    /* ⚠️ 实测（2026-10-15 真实 Key）：status 是**数字码**不是中文，中文在 description */
    evaluation: { status: '3', description: '中度拥堵' },
    /*
     * ⚠️ 实测字段：name/status(码)/direction/angle/speed/lcodes/polyline
     *    **没有 congestion_distance** —— 拥堵里程由 polyline 算出。
     */
    roads: [
      {
        name: '东长安街', status: '3', direction: '从A到B', speed: '5',
        /* 约 111m：纬度 0.001° ≈ 111m */
        polyline: '116.40,39.910;116.40,39.911',
      },
      {
        name: '建国门内大街', status: '2', direction: '从C到D', speed: '25',
        polyline: '116.41,39.910;116.41,39.912',
      },
    ],
  },
};

test('POST /traffic 正常返回拥堵状态', async () => {
  const s = await startServer(TRAFFIC_OK);
  try {
    const r = await post(s.url, '/traffic', P);
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.status, '拥堵', '数字码 3 应映射为「拥堵」');
    assert.equal(j.statusCode, '3', '原始码应保留便于排查');
    assert.equal(j.description, '中度拥堵');
    assert.equal(j.count, 2);
    assert.equal(j.roads[0].name, '东长安街');
    assert.equal(j.roads[0].status, '拥堵');
    assert.equal(j.roads[0].speedKmh, 5);
    assert.equal(j.roads[0].direction, '从A到B');
  } finally {
    await s.close();
  }
});

test('★ /traffic：拥堵里程由 polyline 算出（该接口**没有** congestion_distance 字段）', async () => {
  const s = await startServer(TRAFFIC_OK);
  try {
    const j = await (await post(s.url, '/traffic', P)).json();
    const congested = j.roads[0];
    const smooth = j.roads[1];
    // 纬度差 0.001° ≈ 111m
    assert.ok(
      congested.lengthM > 100 && congested.lengthM < 125,
      `拥堵段长度应约 111m，实际 ${congested.lengthM}`,
    );
    assert.equal(
      congested.congestedLengthM, congested.lengthM,
      'status=拥堵 的路段，其拥堵里程应等于路段长度',
    );
    assert.equal(
      smooth.congestedLengthM, 0,
      'status=缓行 的路段拥堵里程为 0（畅通/缓行不计入拥堵里程）',
    );
  } finally {
    await s.close();
  }
});

test('★ /traffic：未识别的状态码原样保留（不猜成"畅通"）', async () => {
  const weird = {
    status: '1',
    trafficinfo: { evaluation: { status: '9', description: '' }, roads: [] },
  };
  const s = await startServer(weird);
  try {
    const j = await (await post(s.url, '/traffic', P)).json();
    assert.equal(j.status, '9', '未知码应原样保留，绝不能猜成"畅通"（危险误导）');
  } finally {
    await s.close();
  }
});

test('★ /traffic：用 extensions=all 才能拿到具体路段（base 时 roads 恒为空）', async () => {
  let seenUrl = null;
  const s = await startServer(async (u) => {
    seenUrl = new URL(u);
    return new Response(JSON.stringify(TRAFFIC_OK), { status: 200 });
  });
  try {
    await post(s.url, '/traffic', P);
    assert.equal(
      seenUrl.searchParams.get('extensions'), 'all',
      '实测 base 不返回 roads，必须用 all',
    );
  } finally {
    await s.close();
  }
});

test('★ /traffic：rectangle 参数形如 "lon,lat;lon,lat" 且纬度顺序正确（下角在前）', async () => {
  let seenUrl = null;
  const s = await startServer(async (u) => {
    seenUrl = new URL(u);
    return new Response(JSON.stringify(TRAFFIC_OK), { status: 200 });
  });
  try {
    await post(s.url, '/traffic', { ...P, halfMeters: 500 });
    const rect = seenUrl.searchParams.get('rectangle');
    const parts = rect.split(';');
    assert.equal(parts.length, 2, '矩形应为两个角点');
    const [lon1, lat1] = parts[0].split(',').map(Number);
    const [lon2, lat2] = parts[1].split(',').map(Number);
    assert.ok(lat1 < lat2, '左下的纬度必须小于右上（写反高德会报参数错误）');
    assert.ok(lon1 < lon2, '左下的经度必须小于右上');
  } finally {
    await s.close();
  }
});

test('/traffic：halfMeters 超限被夹取（不让上游被超大区域拖死）', async () => {
  let seenUrl = null;
  const s = await startServer(async (u) => {
    seenUrl = new URL(u);
    return new Response(JSON.stringify(TRAFFIC_OK), { status: 200 });
  });
  try {
    await post(s.url, '/traffic', { ...P, halfMeters: 999999 });
    const [lon1, lat1] = seenUrl.searchParams.get('rectangle').split(';')[0].split(',').map(Number);
    const [lon2, lat2] = seenUrl.searchParams.get('rectangle').split(';')[1].split(',').map(Number);
    // 夹到 3000m → 纬度跨度约 0.054°，远超此值说明没夹取
    assert.ok(Math.abs(lat2 - lat1) < 0.1, `纬度跨度应受限于 3000m，实际 ${Math.abs(lat2 - lat1)}`);
  } finally {
    await s.close();
  }
});

test('/traffic：折线缺失 → 长度与拥堵里程为 null（不记 0）', async () => {
  const weird = {
    status: '1',
    trafficinfo: {
      evaluation: { status: '畅通' },
      /* 高德在缺字段时会返回空数组（实测过），必须容忍而不是崩或当成 0 */
      roads: [{ name: 'X路', status: '3', polyline: [] }],
    },
  };
  const s = await startServer(weird);
  try {
    const r = await post(s.url, '/traffic', P);
    const j = await r.json();
    assert.equal(j.roads[0].lengthM, null, '无折线 → 长度未知应为 null');
    assert.equal(
      j.roads[0].congestedLengthM, null,
      '长度未知时拥堵里程也应为 null，绝不能记 0（0 会被读成"畅通"）',
    );
  } finally {
    await s.close();
  }
});

// ───────────────────────── /around ─────────────────────────

const AROUND_OK = {
  status: '1',
  info: 'OK',
  infocode: '10000',
  pois: [
    { name: '中石化加油站', type: '汽车服务;加油站', distance: '450', location: '116.403694,39.910612', address: '东长安街1号' },
    { name: '无坐标POI', type: 'xxx', distance: '500', location: [], address: '' },
  ],
};

test('POST /around 返回 POI，坐标已转回 WGS84', async () => {
  const s = await startServer(AROUND_OK);
  try {
    const r = await post(s.url, '/around', { ...P, keywords: '加油站' });
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.pois.length, 1, '无坐标的 POI 应被跳过');
    const poi = j.pois[0];
    assert.equal(poi.name, '中石化加油站');
    assert.equal(poi.distanceM, 450);
    /*
     * 上游给的是 GCJ-02 (116.403694, 39.910612)；
     * 返回给客户端必须是 WGS84 —— 即**不等于**原值（北京地区偏移明显）。
     */
    assert.notEqual(poi.lon, 116.403694, '必须转回 WGS84，不能透传 GCJ-02');
    assert.ok(Math.abs(poi.lon - 116.403694) < 0.01, '偏移应在合理量级');
  } finally {
    await s.close();
  }
});

test('/around：keywords 过长 → 400', async () => {
  const s = await startServer(AROUND_OK);
  try {
    const r = await post(s.url, '/around', { ...P, keywords: 'x'.repeat(61) });
    assert.equal(r.status, 400);
    const j = await r.json();
    assert.equal(j.error, 'invalid_keywords');
  } finally {
    await s.close();
  }
});

test('/around：不传 keywords 也允许（返回周边全部 POI）', async () => {
  let seenUrl = null;
  const s = await startServer(async (u) => {
    seenUrl = new URL(u);
    return new Response(JSON.stringify(AROUND_OK), { status: 200 });
  });
  try {
    const r = await post(s.url, '/around', P);
    assert.equal(r.status, 200);
    assert.equal(seenUrl.searchParams.get('keywords'), null, '未指定关键词时不应带该参数');
  } finally {
    await s.close();
  }
});

// ───────────────────────── /tips ─────────────────────────

const TIPS_OK = {
  status: '1',
  info: 'OK',
  infocode: '10000',
  tips: [
    { name: '天安门', district: '北京市东城区', adcode: '110101', location: '116.397428,39.90923' },
    { name: '北京市', district: '', adcode: '110000', location: [] },
  ],
};

test('POST /tips 返回联想候选；无坐标的候选保留但坐标为 null', async () => {
  const s = await startServer(TIPS_OK);
  try {
    const r = await post(s.url, '/tips', { keywords: '天安' });
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.tips.length, 2, '无坐标候选应保留（如"北京市"），不能整条丢掉');
    assert.equal(j.tips[0].name, '天安门');
    assert.ok(j.tips[0].lon !== null && j.tips[0].lon !== undefined);
    assert.equal(j.tips[1].name, '北京市');
    assert.equal(j.tips[1].lon, null);
    assert.equal(j.tips[1].lat, null);
  } finally {
    await s.close();
  }
});

test('★ /tips 返回坐标是 WGS84（不透传 GCJ-02）', async () => {
  const s = await startServer(TIPS_OK);
  try {
    const r = await post(s.url, '/tips', { keywords: '天安' });
    const j = await r.json();
    assert.notEqual(j.tips[0].lon, 116.397428, '应与 GCJ-02 原值不同（已转 WGS84）');
  } finally {
    await s.close();
  }
});

test('/tips：空关键词 → 400；带 city 时透传', async () => {
  let seenUrl = null;
  const s = await startServer(async (u) => {
    seenUrl = new URL(u);
    return new Response(JSON.stringify(TIPS_OK), { status: 200 });
  });
  try {
    assert.equal((await post(s.url, '/tips', { keywords: '  ' })).status, 400);
    await post(s.url, '/tips', { keywords: '外滩', city: '上海' });
    assert.equal(seenUrl.searchParams.get('city'), '上海');
  } finally {
    await s.close();
  }
});

// ───────────────────────── /bicycling ─────────────────────────

const BICYCLING_OK = {
  errcode: 0,
  errmsg: 'OK',
  data: {
    paths: [
      {
        distance: 5200,
        duration: 1200,
        steps: [
          {
            instruction: '沿东长安街向西骑行500米右转',
            action: '右转', road: '东长安街', distance: '500', duration: '120',
            polyline: '116.40,39.91;116.41,39.91',
          },
          {
            instruction: '沿南池子大街向南骑行300米左转',
            action: '左转', road: '南池子大街', distance: '300', duration: '80',
            polyline: '116.41,39.91;116.42,39.92',
          },
        ],
      },
    ],
  },
};

test('POST /bicycling 返回 RouteBundle，且标记 mode=bicycling', async () => {
  const s = await startServer(BICYCLING_OK);
  try {
    const r = await post(s.url, '/bicycling', { origin: P, destination: { lon: 116.48, lat: 39.98 } });
    assert.equal(r.status, 200);
    const j = await r.json();
    assert.equal(j.mode, 'bicycling');
    assert.equal(j.distanceM, 5200);
    assert.equal(j.durationS, 1200);
    assert.equal(j.steps.length, 2);
    assert.equal(j.steps[0].roadName, '东长安街');
  } finally {
    await s.close();
  }
});

test('★ /bicycling：action 被正确解析为转向码（实测骑行 v4 **确实**返回中文 action）', async () => {
  const s = await startServer(BICYCLING_OK);
  try {
    const r = await post(s.url, '/bicycling', { origin: P, destination: { lon: 116.48, lat: 39.98 } });
    const j = await r.json();
    /*
     * 实测（2026-10-15 真实 Key）骑行 v4 的 steps[].action 是 "右转" 这样的中文，
     * 与驾车一致 → 必须复用同一码表。若这里为 0(未知)，说明退回了"写死 UNKNOWN"的
     * 旧实现，仪表在骑行模式下会完全不显示转向提示。
     */
    assert.equal(j.steps[0].maneuver, 3, '"右转" 应映射为 3');
    assert.equal(j.steps[1].maneuver, 2, '"左转" 应映射为 2');
    assert.ok(
      j.steps.every((st) => st.maneuver !== 0),
      '实测样本中每段都有 action，不应出现"未知"',
    );
  } finally {
    await s.close();
  }
});

test('★ /bicycling 用的是 v4 地址，且 data.paths 结构被正确解析', async () => {
  let seenUrl = null;
  const s = await startServer(async (u) => {
    seenUrl = new URL(u);
    return new Response(JSON.stringify(BICYCLING_OK), { status: 200 });
  });
  try {
    await post(s.url, '/bicycling', { origin: P, destination: { lon: 116.48, lat: 39.98 } });
    assert.ok(seenUrl.pathname.includes('/v4/direction/bicycling'), `实际路径 ${seenUrl.pathname}`);
  } finally {
    await s.close();
  }
});

test('/bicycling：data.paths 为空 → 502（不返回空壳路线）', async () => {
  const s = await startServer({ errcode: 0, data: { paths: [] } });
  try {
    const r = await post(s.url, '/bicycling', { origin: P, destination: { lon: 116.48, lat: 39.98 } });
    assert.equal(r.status, 502, '空路径应报错，不能让客户端拿到"0 公里"的假路线');
  } finally {
    await s.close();
  }
});

// ───────────────────────── 通用约束 ─────────────────────────

test('★ 新接口同样要求认证：无 token → 401', async () => {
  const s = await startServer(REGEO_OK);
  try {
    for (const [path, body] of [
      ['/regeo', P],
      ['/traffic', P],
      ['/around', P],
      ['/tips', { keywords: '天安' }],
      ['/bicycling', { origin: P, destination: P }],
    ]) {
      const r = await post(s.url, path, body, null);
      assert.equal(r.status, 401, `${path} 必须要求认证`);
    }
  } finally {
    await s.close();
  }
});

test('★ 新接口的响应同样带 cache-control: no-store（协议 §3.5）', async () => {
  const s = await startServer(REGEO_OK);
  try {
    const r = await post(s.url, '/regeo', P);
    assert.equal(r.headers.get('cache-control'), 'no-store');
  } finally {
    await s.close();
  }
});

test('★ 上游业务错误 → 502 且透出 info/infocode，但不含 Key', async () => {
  const amapErr = { status: '0', info: 'INVALID_USER_KEY', infocode: '10001' };
  const s = await startServer(amapErr, { config: { verboseErrors: true } });
  try {
    const r = await post(s.url, '/regeo', P);
    assert.equal(r.status, 502);
    const text = await r.text();
    const j = JSON.parse(text);
    assert.equal(j.error, 'amap_error');
    assert.equal(j.infocode, '10001');
    assert.ok(j.hint, '应给出排查提示');
    assert.ok(!text.includes('FAKE_KEY_FOR_TEST'), '不得泄露 Key');
  } finally {
    await s.close();
  }
});

test('★ /health 的用量计数包含新接口', async () => {
  const s = await startServer(REGEO_OK);
  try {
    await post(s.url, '/regeo', P);
    await post(s.url, '/traffic', P);
    const j = await (await fetch(`${s.url}/health`)).json();
    assert.equal(j.amapCallsToday.regeo, 1);
    assert.equal(j.amapCallsToday.traffic, 1);
    assert.ok(j.amapCallsToday.total >= 2);
  } finally {
    await s.close();
  }
});

test('新接口路径不在白名单 → 404（内部结构不外泄）', async () => {
  const s = await startServer(REGEO_OK);
  try {
    const r = await post(s.url, '/regeox', P);
    assert.equal(r.status, 404);
  } finally {
    await s.close();
  }
});
