/**
 * mapwindow.js —— 周边地图窗口（道路 + 建筑几何）获取
 *
 * ## 为什么需要它
 *
 * 圆屏要画出"眼前的路"，但 ESP32 既存不下整座城市的地图，也没有算力做投影。
 * 参考项目（moto-gps-waveshare）的做法是：**手机按当前位置筛一个约 500 米的小窗口，
 * 把几何矢量通过 BLE 发给圆屏**，圆屏只负责画。本项目沿用同一思路（设计参考，
 * 代码独立实现，见 docs/decisions.md D-009）。
 *
 * ## 数据源：OpenFreeMap 公共矢量瓦片
 *
 * - 地址取自其 style JSON 指向的 TileJSON，形如
 *   `https://tiles.openfreemap.org/planet/<版本>/<z>/<x>/<y>.pbf`
 * - z14 一个瓦片约覆盖 1.8 km（北京纬度），**±500m 窗口只需 1 个瓦片**
 * - 官方允诺：**离线下载每次 ≤3000 瓦片 / ≤50MB，最多 4 并发，需唯一 User-Agent**
 *   本模块据此做**磁盘缓存**（同一瓦片只取一次）并串行化下载
 * - 数据来源为 OpenStreetMap（ODbL）；**必须保留署名**
 *
 * ## 为什么不用高德
 *
 * 高德协议 §3.5 明确禁止"存储、缓存或抓取本服务内部数据、图片"，
 * 故环境图层只能用允许离线的 OSM 系数据。路线/搜索仍走高德。
 *
 * ## 坐标系与投影：**网关端完成**
 *
 * 返回的坐标是**以窗口中心为原点的局部米坐标**（x=东, y=北，int16 足够表示 ±500m）。
 * 这样设备端**不需要任何经纬度/墨卡托数学**，只要按航向旋转、缩放、平移即可 ——
 * 这是刻意的取舍：把复杂度放在有算力的一侧。
 *
 * ## 预算（决定 BLE 能否承载）
 *
 * 参考项目的窗口上限是"24 条路/192 点 + 16 栋楼/128 点"，本模块默认对齐该量级
 * （可用参数覆盖）。超限时**按到中心的距离取最近的**，保证眼前最清楚。
 */

import { VectorTile } from '@mapbox/vector-tile';
import { wgs84ToGcj02 } from './coords.js';
import { PbfReader } from 'pbf';
import fs from 'node:fs';
import path from 'node:path';

/** 瓦片缩放级别：z14 的瓦片约 1.8km（北京纬度），足够覆盖 ±500m 且不至于过大 */
export const TILE_ZOOM = 14;

/** 唯一 User-Agent（OpenFreeMap 要求；勿写成浏览器 UA） */
const USER_AGENT = 'car-dashboard-nav-gateway/0.1 (personal study; non-commercial)';

/**
 * 参与绘制的道路类别（OpenMapTiles 的 `transportation.class`）。
 * 刻意排除 path/footway/steps/cycleway 等 —— 它们量大且对驾驶无参考价值。
 */
const ROAD_CLASSES = new Set([
  'motorway', 'trunk', 'primary', 'secondary', 'tertiary',
  'minor', 'service', 'motorway_construction', 'trunk_construction',
  'primary_construction', 'secondary_construction', 'tertiary_construction',
]);

/**
 * OpenMapTiles 的 `transportation.class` **类名 → 协议整数等级**。
 *
 * ⚠️ 这张表是必须的：协议（docs/ble.md）、App（`MapWindowBundle.Road.c: Int`）、
 *    固件（`nav_map_road_t.cls` + `road_width()`）三处都按 **0..5 的整数**约定，
 *    而瓦片里给的是 `"service"` / `"tertiary"` 这样的**字符串**。
 *    最初漏了这一步，把字符串直接透传 —— App 解码 `Int` 必然失败，
 *    表现是"窗口一直取不回来、圆屏上什么都没有"（实测踩过）。
 *
 * 与 docs/ble.md 的表格一致：0 高速 / 1 主干 / 2 次干 / 3 支路 / 4 辅路 / 5 其它。
 */
const ROAD_CLASS_INT = {
  motorway: 0,
  trunk: 0,
  primary: 1,
  secondary: 2,
  tertiary: 3,
  minor: 3, /* OpenMapTiles 把 residential/unclassified 等归入 minor */
  service: 4,
};

/** 类名 → 整数等级（未知一律 5=其它） */
export function roadClassToInt(name) {
  return ROAD_CLASS_INT[name] ?? 5;
}

/** 默认预算：对齐参考项目的窗口量级 */
export const DEFAULT_BUDGET = {
  maxRoads: 24,
  maxRoadPoints: 192,
  maxBuildings: 16,
  maxBuildingPoints: 128,
  /**
   * **单条要素**的点数上限。
   *
   * 为什么必须有：BLE 是"一条要素一条消息"，单条消息不能超过一个 BLE 包
   * （iOS 实到约 185 字节）。一条 1km 的直路按 8m 抽稀有 125 个点，会被
   * 总数预算直接跳过 —— 而它往往正是穿过中心、最该显示的那条路。
   * 因此改为**每条要素自适应降采样**到这个上限，宁可形状粗一点也不能丢。
   */
  maxPointsPerFeature: 16,
};

/* ---------------- 瓦片坐标与投影 ---------------- */

/** 经纬度 → 瓦片坐标（Web Mercator） */
export function lonLatToTile(lon, lat, z = TILE_ZOOM) {
  const n = 2 ** z;
  const x = Math.floor(((lon + 180) / 360) * n);
  const latRad = (lat * Math.PI) / 180;
  const y = Math.floor(
    ((1 - Math.log(Math.tan(latRad) + 1 / Math.cos(latRad)) / Math.PI) / 2) * n,
  );
  return { z, x, y };
}

/**
 * 经纬度 → 以 `origin` 为原点的局部米坐标（等距圆柱近似）。
 *
 * 500m 量级下该近似的误差可忽略（<0.1m），却比完整墨卡托简单得多。
 *
 * ⚠️ 调用前**必须**保证 `lon/lat` 与 `originLon/originLat` 在**同一坐标系**，
 *    否则会引入数百米的整体偏移（见 `fetchMapWindow` 里的坐标系统一说明）。
 */
export function toLocalMeters(lon, lat, originLon, originLat) {
  const mPerDegLat = 111320;
  const mPerDegLon = 111320 * Math.cos((originLat * Math.PI) / 180);
  return {
    x: (lon - originLon) * mPerDegLon,
    y: (lat - originLat) * mPerDegLat,
  };
}

/**
 * 瓦片内的整数坐标（0..extent）→ 经纬度。
 * MVT 的几何是相对瓦片的整数格点，需要先还原成经纬度再转局部米。
 */
function tilePointToLonLat(px, py, extent, tile) {
  const n = 2 ** tile.z;
  const lon = ((tile.x + px / extent) / n) * 360 - 180;
  const latRad = Math.atan(Math.sinh(Math.PI * (1 - (2 * (tile.y + py / extent)) / n)));
  return { lon, lat: (latRad * 180) / Math.PI };
}

/* ---------------- 瓦片下载与缓存 ---------------- */

function cachePathFor(cacheDir, tile) {
  return path.join(cacheDir, `${tile.z}`, `${tile.x}`, `${tile.y}.pbf`);
}

/**
 * 取瓦片字节。**优先磁盘缓存** —— 同一区域只向上游取一次。
 *
 * 缓存是必须的，不只是优化：OpenFreeMap 的离线额度是"每次 ≤3000 瓦片"，
 * 而导航中窗口会反复请求同一片区域。
 */
export async function loadTile(tile, { cacheDir, fetchImpl = globalThis.fetch, timeoutMs = 8000 }) {
  const file = cachePathFor(cacheDir, tile);
  try {
    const buf = fs.readFileSync(file);
    if (buf.length > 0) return { buf, cached: true };
  } catch {
    /* 未缓存：继续下载 */
  }

  const url = `https://tiles.openfreemap.org/planet/20261011_080001_pt/${tile.z}/${tile.x}/${tile.y}.pbf`;
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), timeoutMs);
  let resp;
  try {
    resp = await fetchImpl(url, { headers: { 'User-Agent': USER_AGENT }, signal: ac.signal });
  } finally {
    clearTimeout(timer);
  }
  if (!resp.ok) throw new Error(`瓦片 HTTP ${resp.status}`);
  const buf = new Uint8Array(await resp.arrayBuffer());

  /* 只在拿到非空瓦片时写盘；空瓦片也写（避免反复请求空白区域） */
  try {
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, buf);
  } catch {
    /* 缓存写失败不影响本次返回 */
  }
  return { buf, cached: false };
}

/* ---------------- 几何提取 ---------------- */

/**
 * 均匀降采样到 `maxN` 个点（**保留首尾**）。
 *
 * 用于"单条要素点数超上限"时：形状会变粗，但要素本身不会丢。
 * 这是刻意的取舍 —— 背景图层的作用是提供路网参照，不是精确测距。
 */
function decimate(points, maxN) {
  if (points.length <= maxN || maxN < 2) return points.slice(0, maxN);
  const out = [points[0]];
  const step = (points.length - 1) / (maxN - 1);
  for (let k = 1; k < maxN - 1; k++) out.push(points[Math.round(k * step)]);
  out.push(points[points.length - 1]);
  return out;
}

/** 折线抽稀：相邻点距离小于 `minM` 时丢弃，减少点数 */
function simplify(points, minM) {
  if (points.length <= 2) return points;
  const out = [points[0]];
  for (let i = 1; i < points.length - 1; i++) {
    const p = points[i];
    const q = out[out.length - 1];
    if (Math.hypot(p.x - q.x, p.y - q.y) >= minM) out.push(p);
  }
  out.push(points[points.length - 1]);
  return out;
}

/** 折线是否与半径圆有交集（任一点在圆内，或跨越圆） */
function withinRadius(points, radiusM) {
  return points.some((p) => Math.hypot(p.x, p.y) <= radiusM);
}

function collectLines(layer, klass, originLon, originLat, tile, radiusM, minPointSpacingM, maxPointsPerFeature) {
  const out = [];
  for (let i = 0; i < layer.length; i++) {
    const f = layer.feature(i);
    const c = f.properties.class;
    if (klass && !klass.has(c)) continue;

    const rings = f.loadGeometry();
    for (const ring of rings) {
      const pts = ring.map((p) => {
        const { lon, lat } = tilePointToLonLat(p.x, p.y, layer.extent, tile);
        /* OSM 是 WGS84 → 先统一到 GCJ-02，再投影到（同样是 GCJ-02 的）原点 */
        const gcj = wgs84ToGcj02({ lon, lat });
        return toLocalMeters(gcj.lon, gcj.lat, originLon, originLat);
      });
      if (pts.length < 2 || !withinRadius(pts, radiusM)) continue;
      const s = decimate(simplify(pts, minPointSpacingM), maxPointsPerFeature);
      /* 最近点距离：用于超预算时按"离我最近"取舍 */
      let nearest = Infinity;
      for (const p of s) nearest = Math.min(nearest, Math.hypot(p.x, p.y));
      out.push({ class: c ?? 'unknown', pts: s, nearest });
    }
  }
  return out;
}

/* ---------------- 对外的窗口获取 ---------------- */

/**
 * 取当前位置周边的地图窗口。
 *
 * @param {object} p
 * @param {number} p.lon            中心经度（WGS84）
 * @param {number} p.lat            中心纬度（WGS84）
 * @param {number} [p.radiusM]      半径（米），默认 500
 * @param {string} p.cacheDir       瓦片缓存目录
 * @param {object} [p.budget]       点数预算，见 DEFAULT_BUDGET
 * @param {Function} [p.fetchImpl]  注入 fetch（测试用）
 * @returns {Promise<{center:{lon,lat}, radiusM:number, roads:Array, buildings:Array,
 *                    truncated:{roads:boolean,buildings:boolean}, attribution:string}>}
 */
export async function fetchMapWindow({
  lon,
  lat,
  radiusM = 500,
  cacheDir,
  budget = DEFAULT_BUDGET,
  fetchImpl = globalThis.fetch,
}) {
  /*
   * ── 坐标系统一（关键，做错整张图偏约 500 米）────────────────────────
   *
   * OSM 数据是 **WGS84**，而导航路线来自高德的 **GCJ-02**。两层要画在同一张
   * 图上就必须统一。这里统一到 **GCJ-02**：
   *
   *   1. 中心点：入参是手机的 WGS84 定位 → 转成 GCJ-02，作为局部坐标原点
   *   2. OSM 要素：逐个 WGS84 → GCJ-02 后再投影到该原点
   *   3. 客户端拿到的 `center` 就是 GCJ-02，因此**App 端一行转换都不用写**
   *      （它本来就有 GCJ-02 的自车位置，直接算偏移即可）
   *
   * 瓦片本身仍按 **WGS84** 定位并缓存（OSM 的固有坐标系）。
   */
  const tile = lonLatToTile(lon, lat);
  const { buf } = await loadTile(tile, { cacheDir, fetchImpl });

  const centerGcj = wgs84ToGcj02({ lon, lat });

  const vt = new VectorTile(new PbfReader(buf));

  /* 道路：折线，抽稀到 ~8m 一点（500m 内足够平滑，点数可控） */
  const roadLayer = vt.layers.transportation;
  let roads = roadLayer
    ? collectLines(roadLayer, ROAD_CLASSES, centerGcj.lon, centerGcj.lat, tile, radiusM, 8, budget.maxPointsPerFeature)
    : [];
  roads.sort((a, b) => a.nearest - b.nearest);

  /* 建筑：多边形轮廓，抽稀更狠（只要形状可辨） */
  const bLayer = vt.layers.building;
  let buildings = bLayer
    ? collectLines(bLayer, null, centerGcj.lon, centerGcj.lat, tile, radiusM, 12, budget.maxPointsPerFeature)
        .map((b) => ({ pts: b.pts, nearest: b.nearest }))
    : [];
  buildings.sort((a, b) => a.nearest - b.nearest);

  /* 按预算裁剪：先按条数，再按点数逐条累加（保证不超总点数） */
  const before = { roads: roads.length, buildings: buildings.length };

  const pickedRoads = [];
  let rp = 0;
  for (const r of roads) {
    if (pickedRoads.length >= budget.maxRoads) break;
    if (rp + r.pts.length > budget.maxRoadPoints) continue; /* 跳过太长的，试下一条 */
    pickedRoads.push(r);
    rp += r.pts.length;
  }

  const pickedBuildings = [];
  let bp = 0;
  for (const b of buildings) {
    if (pickedBuildings.length >= budget.maxBuildings) break;
    if (bp + b.pts.length > budget.maxBuildingPoints) continue;
    pickedBuildings.push(b);
    bp += b.pts.length;
  }

  /* 输出时去掉内部的 nearest 字段，并把坐标取整到米（够用且更小） */
  const round = (pts) => pts.map((p) => [Math.round(p.x), Math.round(p.y)]);

  return {
    /** 入参所用的 WGS84 中心（原样回显，便于排查）*/
    center: { lon, lat },
    /** **局部坐标原点**（GCJ-02）。自车偏移必须相对它计算。*/
    centerGcj: { lon: centerGcj.lon, lat: centerGcj.lat },
    /** 所有几何的坐标系，供客户端自检 */
    crs: 'GCJ-02',
    radiusM,
    /* c 必须是**整数等级**（见 ROAD_CLASS_INT 的说明），不能透传 OSM 类名 */
    roads: pickedRoads.map((r) => ({ c: roadClassToInt(r.class), p: round(r.pts) })),
    buildings: pickedBuildings.map((b) => ({ p: round(b.pts) })),
    truncated: {
      roads: pickedRoads.length < before.roads,
      buildings: pickedBuildings.length < before.buildings,
    },
    attribution: '© OpenStreetMap contributors (ODbL) · 瓦片 OpenFreeMap',
  };
}
