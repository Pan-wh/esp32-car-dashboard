/**
 * amap.js —— 高德 Web 服务客户端
 *
 * ## 职责边界
 *
 * 本模块**只负责**：拼 URL → 发请求 → 校验业务状态 → 返回原始 JSON。
 * **不做**归一化（那是 normalize.js 的事），也不做缓存（协议 §3.5 禁止）。
 *
 * ## 可测试性
 *
 * `fetchImpl` 可注入，测试时传假 fetch，无需联网、无需真实 Key。
 *
 * ## 坐标
 *
 * 传入**WGS84**（手机定位坐标系），内部自动转 **GCJ-02** 后发给高德；
 * 返回结果中的坐标即 GCJ-02，与手机端后续转换保持一致。
 */
import {
  AMAP_DRIVING_URL,
  AMAP_GEOCODE_URL,
  AMAP_REGEO_URL,
  AMAP_TRAFFIC_RECTANGLE_URL,
  AMAP_PLACE_AROUND_URL,
  AMAP_INPUT_TIPS_URL,
  AMAP_BICYCLING_URL,
} from './config.js';
import { wgs84ToGcj02, gcj02ToWgs84, formatLonLat, parseLonLat } from './coords.js';
import { assertAmapOk, AmapError, parsePolyline } from './normalize.js';

/**
 * 校验响应业务状态，**同时兼容 v3（status）与 v4/骑行（errcode）**。
 *
 * ⚠️ 骑行 v4 用的是 `errcode: 0` 表示成功，**没有 `status` 字段**。
 *    若直接套 `assertAmapOk`（它要求 `status === '1'`），
 *    成功的骑行响应会被判成失败 → 网关恒返回 502。
 *    这是实现时实际踩到的坑，故在此显式分支。
 *
 * @throws {AmapError}
 */
function assertBusinessOk(json) {
  if (json == null || typeof json !== 'object') {
    throw new AmapError('响应不是 JSON 对象', null, json);
  }
  // v4 系列（骑行等）：errcode === 0 表示成功
  if (json.errcode !== undefined && json.errcode !== null) {
    if (Number(json.errcode) === 0) return json;
    throw new AmapError(json.errmsg ?? json.errdetail, String(json.errcode), json);
  }
  // v3 系列：status === "1"
  return assertAmapOk(json);
}

/**
 * 公共请求逻辑：发 GET、超时、校验 HTTP 与业务状态、返回原始 JSON。
 *
 * ⚠️ 错误信息里**绝不带 url**（含 key）—— 这是全模块的安全底线。
 * 抽出来是因为下面每个接口的这段逻辑完全一样，复制容易漏掉某一处。
 *
 * @param {URL} url
 * @param {{timeoutMs?:number, fetchImpl?:Function, what:string}} p
 * @returns {Promise<object>} 已通过业务状态校验的原始响应
 */
async function amapGet(url, { timeoutMs = 8000, fetchImpl = globalThis.fetch, what }) {
  if (typeof fetchImpl !== 'function') throw new Error(`${what}: 无可用 fetch`);

  // 超时用 AbortController：避免上游卡住时把网关连接也拖死
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), timeoutMs);

  let resp;
  try {
    resp = await fetchImpl(url, { signal: ac.signal });
  } catch (e) {
    throw new Error(`${what}失败：${e?.name === 'AbortError' ? '超时' : e?.message ?? e}`);
  } finally {
    clearTimeout(timer);
  }

  if (!resp.ok) throw new Error(`高德 HTTP ${resp.status}`);

  let json;
  try {
    json = await resp.json();
  } catch {
    throw new Error('高德返回的不是合法 JSON');
  }

  assertBusinessOk(json);
  return json;
}

/**
 * 把高德的 "lon,lat" 字符串解析为 **WGS84** 点。
 * 单条不合法返回 null（调用方跳过该条，不让整次请求失败）。
 */
function locationToWgs(loc) {
  if (typeof loc !== 'string' || loc.length === 0) return null;
  let gcj;
  try {
    gcj = parseLonLat(loc);
  } catch {
    return null;
  }
  return gcj02ToWgs84(gcj);
}


/**
 * 调用驾车路径规划。
 *
 * @param {object} p
 * @param {string} p.key                 高德 Web 服务 Key
 * @param {{lon,lat}} p.origin           **WGS84** 起点
 * @param {{lon,lat}} p.destination      **WGS84** 终点
 * @param {number} [p.strategy]          高德策略编号（默认 0=速度优先）
 * @param {number} [p.timeoutMs]
 * @param {Function} [p.fetchImpl]       注入 fetch（测试用）
 * @returns {Promise<{raw:object, originGcj:{lon,lat}, destinationGcj:{lon,lat}}>}
 */
export async function fetchDrivingRoute({
  key,
  origin,
  destination,
  strategy = 0,
  timeoutMs = 8000,
  fetchImpl = globalThis.fetch,
}) {
  if (!key) throw new Error('fetchDrivingRoute: 缺少 key');
  if (typeof fetchImpl !== 'function') throw new Error('fetchDrivingRoute: 无可用 fetch');

  // 手机给的是 WGS84；高德要 GCJ-02
  const originGcj = wgs84ToGcj02(origin);
  const destinationGcj = wgs84ToGcj02(destination);

  const url = new URL(AMAP_DRIVING_URL);
  url.searchParams.set('key', key);
  url.searchParams.set('origin', formatLonLat(originGcj)); // 高德要求 lon,lat 且 ≤6 位小数
  url.searchParams.set('destination', formatLonLat(destinationGcj));
  url.searchParams.set('strategy', String(strategy));
  url.searchParams.set('extensions', 'base');

  // 超时用 AbortController：避免上游卡住时把网关连接也拖死
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), timeoutMs);

  let resp;
  try {
    resp = await fetchImpl(url, { signal: ac.signal });
  } catch (e) {
    // 注意：错误信息里**不要**带 url（含 key）
    throw new Error(`请求高德失败：${e?.name === 'AbortError' ? '超时' : e?.message ?? e}`);
  } finally {
    clearTimeout(timer);
  }

  if (!resp.ok) {
    throw new Error(`高德 HTTP ${resp.status}`);
  }

  let json;
  try {
    json = await resp.json();
  } catch {
    throw new Error('高德返回的不是合法 JSON');
  }

  assertAmapOk(json); // status !== "1" 时抛 AmapError（含 info/infocode）
  return { raw: json, originGcj, destinationGcj };
}

export { AmapError };

/**
 * 调用地理编码：地址文本 → 坐标。
 *
 * @param {object} p
 * @param {string} p.key             高德 Web 服务 Key
 * @param {string} p.address         地址/地名（中文）
 * @param {string} [p.city]          限定城市（可选，能显著提高准确率）
 * @param {number} [p.timeoutMs]
 * @param {Function} [p.fetchImpl]
 * @returns {Promise<{results: Array<{name:string,address:string,lon:number,lat:number}>}>}
 *          坐标**已转成 WGS84**（见 AMAP_GEOCODE_URL 处的说明）
 */
export async function fetchGeocode({
  key,
  address,
  city = '',
  timeoutMs = 8000,
  fetchImpl = globalThis.fetch,
}) {
  if (!key) throw new Error('fetchGeocode: 缺少 key');
  if (!address) throw new Error('fetchGeocode: 缺少 address');
  if (typeof fetchImpl !== 'function') throw new Error('fetchGeocode: 无可用 fetch');

  const url = new URL(AMAP_GEOCODE_URL);
  url.searchParams.set('key', key);
  url.searchParams.set('address', address);
  if (city) url.searchParams.set('city', city);

  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), timeoutMs);

  let resp;
  try {
    resp = await fetchImpl(url, { signal: ac.signal });
  } catch (e) {
    throw new Error(`请求高德失败：${e?.name === 'AbortError' ? '超时' : e?.message ?? e}`);
  } finally {
    clearTimeout(timer);
  }

  if (!resp.ok) throw new Error(`高德 HTTP ${resp.status}`);

  let json;
  try {
    json = await resp.json();
  } catch {
    throw new Error('高德返回的不是合法 JSON');
  }

  assertAmapOk(json);

  const list = Array.isArray(json.geocodes) ? json.geocodes : [];
  const results = [];
  for (const g of list) {
    if (!g || typeof g.location !== 'string') continue;
    let gcj;
    try {
      gcj = parseLonLat(g.location); /* 高德是 "lon,lat" */
    } catch {
      continue; /* 单条坐标不合法就跳过，不让整次请求失败 */
    }
    const wgs = gcj02ToWgs84(gcj);
    results.push({
      name: g.formatted_address ?? g.location,
      address: g.formatted_address ?? '',
      lon: wgs.lon,
      lat: wgs.lat,
    });
  }
  return { results };
}

/**
 * 调用逆地理编码：坐标 → 地址（含所在道路名）。
 *
 * @param {object} p
 * @param {string} p.key         高德 Web 服务 Key
 * @param {{lon,lat}} p.point    **WGS84** 坐标（手机定位）
 * @param {number} [p.radius]    搜索半径（米），0 表示自动
 * @param {number} [p.timeoutMs]
 * @param {Function} [p.fetchImpl]
 * @returns {Promise<{formattedAddress:string, road:string|null, poiName:string|null,
 *                    district:string|null, city:string|null, province:string|null}>}
 */
export async function fetchRegeo({
  key,
  point,
  radius = 0,
  timeoutMs = 8000,
  fetchImpl = globalThis.fetch,
}) {
  if (!key) throw new Error('fetchRegeo: 缺少 key');
  if (!point) throw new Error('fetchRegeo: 缺少 point');

  // 手机给的是 WGS84；高德要 GCJ-02
  const gcj = wgs84ToGcj02(point);

  const url = new URL(AMAP_REGEO_URL);
  url.searchParams.set('key', key);
  url.searchParams.set('location', formatLonLat(gcj));
  url.searchParams.set('extensions', 'base'); // base 已含 addressComponent（道路名在这）
  if (radius > 0) url.searchParams.set('radius', String(Math.round(radius)));

  const json = await amapGet(url, { timeoutMs, fetchImpl, what: '请求高德逆地理' });

  const rc = json.regeocode ?? {};
  const comp = rc.addressComponent ?? {};
  /*
   * 道路名有多处候选，按"离驾驶员最近"的实际含义排序：
   *   - `road`：最近的道路名（最贴近"当前在什么路"）
   *   - `streetNumber.street`：门牌所在道路（可能更精确但常缺失）
   * 高德在无路名时会返回**空数组**（不是空字符串），必须容忍。
   */
  const roadCandidate = [comp.road, comp.streetNumber?.street].find(
    (v) => typeof v === 'string' && v.trim().length > 0,
  );

  return {
    formattedAddress: typeof rc.formatted_address === 'string' ? rc.formatted_address : '',
    road: roadCandidate ?? null,
    poiName: typeof comp.building?.name === 'string' && comp.building.name
      ? comp.building.name
      : (Array.isArray(rc.pois) && rc.pois[0]?.name) || null,
    district: typeof comp.district === 'string' && comp.district ? comp.district : null,
    city: typeof comp.city === 'string' && comp.city ? comp.city : null,
    province: typeof comp.province === 'string' && comp.province ? comp.province : null,
  };
}

/**
 * 调用交通态势（矩形区域）：返回区域内道路的拥堵状态。
 *
 * ⚠️ 这是**拥堵信息，不是红绿灯**。红绿灯不在 Web 服务 API 内。
 *
 * @param {object} p
 * @param {string} p.key
 * @param {{lon,lat}} p.center      **WGS84** 中心点
 * @param {number} [p.halfMeters]   半边长（米），默认 500 → 1km×1km 区域
 * @param {number} [p.level]        道路等级（高德定义，默认 6=所有道路）
 * @param {number} [p.timeoutMs]
 * @param {Function} [p.fetchImpl]
 * @returns {Promise<{status:string, description:string, roads:Array, count:number}>}
 */
export async function fetchTrafficRectangle({
  key,
  center,
  halfMeters = 500,
  level = 6,
  timeoutMs = 8000,
  fetchImpl = globalThis.fetch,
}) {
  if (!key) throw new Error('fetchTrafficRectangle: 缺少 key');
  if (!center) throw new Error('fetchTrafficRectangle: 缺少 center');

  const gcj = wgs84ToGcj02(center);

  /*
   * 高德要的是矩形的**左下-右上**两个角（"lon,lat;lon,lat"）。
   * 由中心点 ± 半边长推角点：纬度 1° ≈ 111320m；
   * 经度 1° 需按纬度做余弦收缩（高纬度处经度间距更小）。
   */
  const dLat = halfMeters / 111320;
  const cosLat = Math.max(Math.cos((gcj.lat * Math.PI) / 180), 1e-6); // 防极地除零
  const dLon = halfMeters / (111320 * cosLat);

  // 纬度做简单夹取，避免跨极点产生非法坐标（实用场景不会发生）
  const lat1 = Math.max(-90, Math.min(90, gcj.lat - dLat));
  const lat2 = Math.max(-90, Math.min(90, gcj.lat + dLat));

  const url = new URL(AMAP_TRAFFIC_RECTANGLE_URL);
  url.searchParams.set('key', key);
  url.searchParams.set('rectangle', `${formatLonLat({ lon: gcj.lon - dLon, lat: lat1 })};${formatLonLat({ lon: gcj.lon + dLon, lat: lat2 })}`);
  url.searchParams.set('level', String(level));
  /*
   * ⚠️ 实测（2026-10-15，真实 Key）：`extensions=base` 时 `roads` **恒为空数组**，
   *    拿不到具体拥堵路段。要"前方哪条路堵"就必须用 `all`。
   *    `all` 的响应更大，但本接口只取前若干条，且不缓存（协议 §3.5）。
   */
  url.searchParams.set('extensions', 'all');

  const json = await amapGet(url, { timeoutMs, fetchImpl, what: '请求高德交通态势' });

  const ev = json.trafficinfo?.evaluation ?? {};
  const roads = Array.isArray(json.trafficinfo?.roads) ? json.trafficinfo.roads : [];

  /*
   * ⚠️ 实测更正（2026-10-15，真实 Key）：
   *   1. `evaluation.status` 是**数字码字符串**（"0".."4"），中文在 `description`。
   *   2. `roads[]` 的字段是：name / status(数字码) / direction / angle / speed / lcodes / polyline。
   *      **没有 `congestion_distance` 字段** —— 原先按印象写它是错的（永远得 null）。
   *      拥堵**里程**改用 `polyline` 折线长度累加得到（见下）。
   */
  const STATUS_TEXT = { 0: '未知', 1: '畅通', 2: '缓行', 3: '拥堵', 4: '严重拥堵' };
  const rawStatus = String(ev.status ?? '').trim();

  return {
    /** 可读的总体拥堵等级（如"拥堵"）；未识别的码原样保留 */
    status: STATUS_TEXT[rawStatus] ?? (rawStatus || '未知'),
    /** 高德给的原始码，便于排查（不要用它做判断，用 status） */
    statusCode: rawStatus || null,
    /** 中文描述（高德自己给的，如"中度拥堵"）*/
    description: typeof ev.description === 'string' ? ev.description : '',
    /** 拥堵路段（extensions=all 时才有内容）*/
    roads: roads.map((r) => {
      const roadStatus = String(r.status ?? '').trim();
      /* 只对该路段的折线求长度；解析不出折线则为 null（不记 0） */
      const pts = parsePolyline(r.polyline);
      const lengthM = pts.length >= 2 ? polylineLengthM(pts) : null;
      return {
        name: typeof r.name === 'string' ? r.name : '',
        /** 可读状态（如"拥堵"）*/
        status: STATUS_TEXT[roadStatus] ?? (roadStatus || '未知'),
        statusCode: roadStatus || null,
        /** 行驶方向描述（如"从北河沿大街到南池子大街"）*/
        direction: typeof r.direction === 'string' ? r.direction : '',
        /** 该路段长度（米，由折线算出）；无法解析记 null */
        lengthM,
        /** 平均车速（km/h）；不把未知记 0 */
        speedKmh: toNumberOrNull(r.speed),
        /** 若该路段处于拥堵状态，其长度即"拥堵里程"；否则为 0 是有意义的 */
        congestedLengthM: lengthM !== null
          ? (roadStatus === '3' || roadStatus === '4' ? lengthM : 0)
          : null,
      };
    }),
    count: roads.length,
  };
}

/**
 * 计算折线总长度（米）。用等距圆柱近似 —— 在几百米~几公里的城市尺度上
 * 误差远小于 GPS 精度，且无需引入任何几何库（保持零依赖）。
 */
function polylineLengthM(pts) {
  const R = 6371008.8; // 地球平均半径（米）
  let total = 0;
  for (let i = 1; i < pts.length; i++) {
    const a = pts[i - 1];
    const b = pts[i];
    const dLat = ((b.lat - a.lat) * Math.PI) / 180;
    const dLon = ((b.lon - a.lon) * Math.PI) / 180;
    const meanLat = (((a.lat + b.lat) / 2) * Math.PI) / 180;
    const x = dLon * Math.cos(meanLat);
    total += Math.sqrt(x * x + dLat * dLat) * R;
  }
  return Math.round(total);
}

/**
 * 宽松转数字，失败返回 null（与 normalize.js 同策略：不把未知当 0）。
 *
 * ⚠️ 必须显式排除**数组与对象**：`Number([])` 是 `0`、`Number([5])` 是 `5`，
 *    高德在字段缺失时确实会返回空数组（实测 `congestion_distance: []`）。
 *    若放任其变成 0，拥堵里程 0 会被上层读成"畅通" —— 属危险误导。
 */
function toNumberOrNull(v) {
  if (v === null || v === undefined || v === '' || typeof v === 'boolean') return null;
  if (typeof v === 'object') return null; // 含数组与普通对象
  const n = Number(v);
  return Number.isFinite(n) ? n : null;
}

/**
 * 调用周边搜索：中心点 + 半径 + 关键词 → POI 列表。
 *
 * @param {object} p
 * @param {string} p.key
 * @param {{lon,lat}} p.center   **WGS84** 中心点
 * @param {string} [p.keywords]  关键词（如"加油站"）；留空则返回全部 POI
 * @param {number} [p.radius]    半径（米），默认 3000
 * @param {number} [p.limit]     最多返回条数（在网关侧截断，保护 BLE 带宽）
 * @param {number} [p.timeoutMs]
 * @param {Function} [p.fetchImpl]
 * @returns {Promise<{pois:Array<{name,type,distanceM,lon,lat,address}>}>}
 *          坐标**已转成 WGS84**
 */
export async function fetchPlaceAround({
  key,
  center,
  keywords = '',
  radius = 3000,
  limit = 20,
  timeoutMs = 8000,
  fetchImpl = globalThis.fetch,
}) {
  if (!key) throw new Error('fetchPlaceAround: 缺少 key');
  if (!center) throw new Error('fetchPlaceAround: 缺少 center');

  const gcj = wgs84ToGcj02(center);

  const url = new URL(AMAP_PLACE_AROUND_URL);
  url.searchParams.set('key', key);
  url.searchParams.set('location', formatLonLat(gcj));
  url.searchParams.set('radius', String(Math.round(radius)));
  url.searchParams.set('offset', String(Math.min(Math.max(limit, 1), 25))); // 高德 offset 上限 25
  url.searchParams.set('page', '1');
  url.searchParams.set('extensions', 'base'); // base 足够（name/type/distance/location）
  if (keywords) url.searchParams.set('keywords', keywords);

  const json = await amapGet(url, { timeoutMs, fetchImpl, what: '请求高德周边搜索' });

  const list = Array.isArray(json.pois) ? json.pois : [];
  const pois = [];
  for (const p of list) {
    if (!p) continue;
    const wgs = locationToWgs(p.location);
    if (!wgs) continue; // 无坐标的 POI 对导航无用，跳过
    pois.push({
      name: typeof p.name === 'string' ? p.name : '',
      type: typeof p.type === 'string' ? p.type : '',
      /** 距中心点距离（米）；无法解析记 null */
      distanceM: toNumberOrNull(p.distance),
      lon: wgs.lon,
      lat: wgs.lat,
      address: typeof p.address === 'string' ? p.address : '',
    });
  }
  return { pois };
}

/**
 * 调用输入提示：关键词前缀 → 候选地址列表（搜索框联想）。
 *
 * @param {object} p
 * @param {string} p.key
 * @param {string} p.keywords
 * @param {string} [p.city]      限定城市（可显著提高准确率）
 * @param {number} [p.limit]
 * @param {number} [p.timeoutMs]
 * @param {Function} [p.fetchImpl]
 * @returns {Promise<{tips:Array<{name,district,adcode,lon,lat}>}>}
 */
export async function fetchInputTips({
  key,
  keywords,
  city = '',
  limit = 10,
  timeoutMs = 8000,
  fetchImpl = globalThis.fetch,
}) {
  if (!key) throw new Error('fetchInputTips: 缺少 key');
  if (!keywords) throw new Error('fetchInputTips: 缺少 keywords');

  const url = new URL(AMAP_INPUT_TIPS_URL);
  url.searchParams.set('key', key);
  url.searchParams.set('keywords', keywords);
  if (city) url.searchParams.set('city', city);
  url.searchParams.set('datatype', 'all');

  const json = await amapGet(url, { timeoutMs, fetchImpl, what: '请求高德输入提示' });

  const list = Array.isArray(json.tips) ? json.tips : [];
  const tips = [];
  for (const t of list) {
    if (!t || typeof t.name !== 'string') continue;
    /*
     * ⚠️ 输入提示的 `location` 可能是**空字符串**（行政区、无坐标 POI），
     *    也可能是空数组（高德某些版本）。此时保留候选但坐标为 null ——
     *    用户在联想列表里仍能看到它（比如搜"北京市"），选中后再走 geocode。
     */
    const wgs = locationToWgs(t.location);
    tips.push({
      name: t.name,
      district: typeof t.district === 'string' ? t.district : '',
      adcode: typeof t.adcode === 'string' ? t.adcode : '',
      lon: wgs ? wgs.lon : null,
      lat: wgs ? wgs.lat : null,
    });
    if (tips.length >= limit) break;
  }
  return { tips };
}

/**
 * 调用骑行路径规划（v4）。
 *
 * ⚠️ 与驾车 v3 的差异：
 *   - 路径在 `data.paths[]`（不是 `route.paths[]`）
 *   - 无 `action` 转向语义
 *   - 距离/时长在 `data.paths[].distance/duration`
 * 故本函数只负责原样取回 raw，归一化交给 normalize.js 的 normalizeBicycling。
 *
 * @param {object} p
 * @param {string} p.key
 * @param {{lon,lat}} p.origin       **WGS84**
 * @param {{lon,lat}} p.destination  **WGS84**
 * @param {number} [p.timeoutMs]
 * @param {Function} [p.fetchImpl]
 * @returns {Promise<{raw:object, originGcj:{lon,lat}, destinationGcj:{lon,lat}}>}
 */
export async function fetchBicyclingRoute({
  key,
  origin,
  destination,
  timeoutMs = 8000,
  fetchImpl = globalThis.fetch,
}) {
  if (!key) throw new Error('fetchBicyclingRoute: 缺少 key');
  if (!origin || !destination) throw new Error('fetchBicyclingRoute: 缺少起终点');

  const originGcj = wgs84ToGcj02(origin);
  const destinationGcj = wgs84ToGcj02(destination);

  const url = new URL(AMAP_BICYCLING_URL);
  url.searchParams.set('key', key);
  url.searchParams.set('origin', formatLonLat(originGcj));
  url.searchParams.set('destination', formatLonLat(destinationGcj));

  const json = await amapGet(url, { timeoutMs, fetchImpl, what: '请求高德骑行路径' });
  return { raw: json, originGcj, destinationGcj };
}

