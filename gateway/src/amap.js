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
import { AMAP_DRIVING_URL, AMAP_GEOCODE_URL } from './config.js';
import { wgs84ToGcj02, gcj02ToWgs84, formatLonLat, parseLonLat } from './coords.js';
import { assertAmapOk, AmapError } from './normalize.js';

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
