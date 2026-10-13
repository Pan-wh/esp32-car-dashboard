/**
 * coords.js —— WGS84 ↔ GCJ-02 坐标转换（纯逻辑，无网络/无依赖）
 *
 * ## 为什么需要
 *
 * - iOS 的 CoreLocation 给出的是 **WGS84**（GPS 原始坐标）；
 * - 高德使用 **GCJ-02**（俗称火星坐标，中国境内经过非线性偏移）。
 *
 * 两者在**中国大陆境内**相差约 **300~700 米**，直接混用会让导航整体偏移。
 * 境外不做偏移（算法本身对境外原样返回）。
 *
 * ## 实现说明
 *
 * 采用公开的偏移量拟合算法（克拉索夫斯基椭球 + 多项式扰动），
 * 这是业界广泛使用的实现方式。**注意**：
 *   - 该算法是**拟合**，不是官方实现，误差在米级；
 *   - 本模块的测试用**往返一致性 + 性质检查**（见 test/coords.test.js），
 *     而非与某个"官方值"比对 —— 因为无法在离线环境取得权威基准值。
 *     首次真机联调时应抽样比对实际路网位置（见 docs/nav-backend.md §5）。
 *
 * ## 坐标系约定
 *
 * 本模块对外一律使用 **对象形式 { lon, lat }**，且 `lon` 在前、`lat` 在后，
 * 与高德 URL 参数 `origin=lon,lat` 的顺序一致 —— 避免"经纬度写反"这类
 * 难以察觉的错误（写反后地图会跑到南极附近，且不报错）。
 */

const PI = Math.PI;
/** 克拉索夫斯基椭球长半轴（米） */
const A = 6378245.0;
/** 第一偏心率平方 */
const EE = 0.00669342162296594323;

/**
 * 是否在中国大陆境外（境外不做偏移）。
 * 采用业界常用的粗略矩形范围；港澳台与部分边境区域不在偏移区，
 * 本项目为个人开发用途，不做更精细的边界处理。
 */
export function outOfChina(lon, lat) {
  return !(lon > 73.66 && lon < 135.05 && lat > 3.86 && lat < 53.55);
}

function transformLat(x, y) {
  let ret =
    -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * Math.sqrt(Math.abs(x));
  ret += ((20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0) / 3.0;
  ret += ((20.0 * Math.sin(y * PI) + 40.0 * Math.sin((y / 3.0) * PI)) * 2.0) / 3.0;
  ret += ((160.0 * Math.sin((y / 12.0) * PI) + 320 * Math.sin((y * PI) / 30.0)) * 2.0) / 3.0;
  return ret;
}

function transformLon(x, y) {
  let ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * Math.sqrt(Math.abs(x));
  ret += ((20.0 * Math.sin(6.0 * x * PI) + 20.0 * Math.sin(2.0 * x * PI)) * 2.0) / 3.0;
  ret += ((20.0 * Math.sin(x * PI) + 40.0 * Math.sin((x / 3.0) * PI)) * 2.0) / 3.0;
  ret += ((150.0 * Math.sin((x / 12.0) * PI) + 300.0 * Math.sin((x / 30.0) * PI)) * 2.0) / 3.0;
  return ret;
}

/** 校验坐标是否为有限数且在合法经纬度范围内 */
export function validLonLat(p) {
  return (
    p != null &&
    Number.isFinite(p.lon) &&
    Number.isFinite(p.lat) &&
    p.lon >= -180 &&
    p.lon <= 180 &&
    p.lat >= -90 &&
    p.lat <= 90
  );
}

/**
 * WGS84 → GCJ-02。
 * @param {{lon:number, lat:number}} p
 * @returns {{lon:number, lat:number}} 新对象（不修改入参）
 */
export function wgs84ToGcj02(p) {
  if (!validLonLat(p)) throw new TypeError('wgs84ToGcj02: 非法经纬度');
  const { lon, lat } = p;
  if (outOfChina(lon, lat)) return { lon, lat };

  const dLat = transformLat(lon - 105.0, lat - 35.0);
  const dLon = transformLon(lon - 105.0, lat - 35.0);

  const radLat = (lat / 180.0) * PI;
  let magic = Math.sin(radLat);
  magic = 1 - EE * magic * magic;
  const sqrtMagic = Math.sqrt(magic);

  const mgLat = (dLat * 180.0) / (((A * (1 - EE)) / (magic * sqrtMagic)) * PI);
  const mgLon = (dLon * 180.0) / ((A / sqrtMagic) * Math.cos(radLat) * PI);

  return { lon: lon + mgLon, lat: lat + mgLat };
}

/**
 * GCJ-02 → WGS84（迭代逼近）。
 *
 * 该变换没有解析反函数，用迭代求解：反复用"把当前估计值当 WGS84 转成 GCJ-02"
 * 的方式逼近，直到残差小于 1e-9 度（约 0.1 毫米级），通常 2~3 次即收敛。
 *
 * @param {{lon:number, lat:number}} p
 * @param {number} [maxIter] 最大迭代次数（默认 8）
 */
export function gcj02ToWgs84(p, maxIter = 8) {
  if (!validLonLat(p)) throw new TypeError('gcj02ToWgs84: 非法经纬度');
  if (outOfChina(p.lon, p.lat)) return { lon: p.lon, lat: p.lat };

  let guess = { lon: p.lon, lat: p.lat };
  for (let i = 0; i < maxIter; i++) {
    const conv = wgs84ToGcj02(guess);
    const dLon = conv.lon - p.lon;
    const dLat = conv.lat - p.lat;
    guess = { lon: guess.lon - dLon, lat: guess.lat - dLat };
    if (Math.abs(dLon) < 1e-9 && Math.abs(dLat) < 1e-9) break;
  }
  return guess;
}

/** 两点球面距离（米）——Haversine。用于自检与偏航判断。 */
export function distanceM(a, b) {
  const R = 6371008.8; // 平均地球半径
  const toRad = (d) => (d * PI) / 180;
  const dLat = toRad(b.lat - a.lat);
  const dLon = toRad(b.lon - a.lon);
  const la1 = toRad(a.lat);
  const la2 = toRad(b.lat);
  const h =
    Math.sin(dLat / 2) ** 2 + Math.cos(la1) * Math.cos(la2) * Math.sin(dLon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(h)));
}

/**
 * 格式化为高德要求的 `lon,lat` 字符串。
 * 规则（官方文档）：经度在前、逗号分隔、**小数点不超过 6 位**。
 */
export function formatLonLat(p) {
  if (!validLonLat(p)) throw new TypeError('formatLonLat: 非法经纬度');
  return `${p.lon.toFixed(6)},${p.lat.toFixed(6)}`;
}

/** 解析 `lon,lat` 字符串为对象（解析失败抛错，避免静默产生 NaN 坐标） */
export function parseLonLat(s) {
  if (typeof s !== 'string') throw new TypeError('parseLonLat: 需要字符串');
  const parts = s.split(',');
  if (parts.length !== 2) throw new TypeError(`parseLonLat: 格式应为 "lon,lat"，实际 "${s}"`);
  const lon = Number(parts[0]);
  const lat = Number(parts[1]);
  const p = { lon, lat };
  if (!validLonLat(p)) throw new TypeError(`parseLonLat: 经纬度非法 "${s}"`);
  return p;
}
