/**
 * config.js —— 环境变量读取与校验
 *
 * ## 安全约定（务必遵守）
 *
 * - **Key 只从环境变量读取**，绝不写入代码、配置文件、日志或 Git。
 * - 缺少 Key 时**启动即失败**（快速失败），不要带着空 Key 跑起来后
 *   在请求时才报错 —— 那样容易误以为是网络问题。
 * - 高德协议 §4.5 要求妥善保管 Key；本模块提供 `redact()` 供日志脱敏。
 */

/** 高德 Web 服务驾车路径规划（v3 基础版） */
export const AMAP_DRIVING_URL = 'https://restapi.amap.com/v3/direction/driving';

/**
 * 高德 Web 服务**地理编码**（v3 基础版）：地址文本 → 坐标。
 *
 * 用途：让用户能输入中文地名当目的地（"北京南站"），而不是只能填经纬度。
 * ⚠️ 返回的 `location` 是 **GCJ-02**，网关必须转回 WGS84 再给客户端 ——
 * 客户端约定"目的地一律 WGS84"（它内部还会再转 GCJ-02），
 * 若这里直接透传 GCJ-02 就会**双重转换、偏移数百米**。
 */
export const AMAP_GEOCODE_URL = 'https://restapi.amap.com/v3/geocode/geo';

/**
 * 高德 Web 服务**逆地理编码**：坐标 → 地址（含"当前所在道路名"）。
 *
 * 用途：仪表显示"当前在XX路" —— 比裸经纬度对驾驶员有意义得多。
 * ⚠️ 与 geocode 对称：**输入坐标是 WGS84**（手机定位坐标系），
 *    内部转 GCJ-02 后发给高德。我们只取地址文本（不含坐标），
 *    故该接口不存在"双重转换"问题。
 */
export const AMAP_REGEO_URL = 'https://restapi.amap.com/v3/geocode/regeo';

/**
 * 高德 Web 服务**交通态势（矩形区域）**：经纬度矩形 → 区域内道路拥堵状态。
 *
 * 用途：仪表提示"前方拥堵 / 缓行"。
 * ⚠️ 返回的是**拥堵信息**（畅通/缓行/拥堵 + 拥堵里程），**不是红绿灯**。
 *    红绿灯相位/倒计时不在任何 Web 服务 API 中（见 docs/nav-backend.md）。
 */
export const AMAP_TRAFFIC_RECTANGLE_URL = 'https://restapi.amap.com/v3/traffic/status/rectangle';

/**
 * 高德 Web 服务**周边搜索**：中心点 + 半径 + 关键词 → POI 列表。
 *
 * 用途：沿途/周边的加油站、服务区。
 * ⚠️ 返回 POI 的 `location` 是 **GCJ-02**，须转回 WGS84 再给客户端
 *    （客户端约定"一律 WGS84"，否则双重转换、偏移数百米）。
 */
export const AMAP_PLACE_AROUND_URL = 'https://restapi.amap.com/v3/place/around';

/**
 * 高德 Web 服务**输入提示**：关键词前缀 → 候选地址列表。
 *
 * 用途：App 搜索框的地址联想（"天安" → "天安门…"）。
 * ⚠️ 返回的 `location` 是 GCJ-02，同样须转回 WGS84。
 *    `location` 可能为空字符串（无坐标的 POI），必须容忍。
 */
export const AMAP_INPUT_TIPS_URL = 'https://restapi.amap.com/v3/assistant/inputtips';

/**
 * 高德 Web 服务**骑行路径规划**（v4）。
 *
 * 用途：骑行路线规划。
 * ⚠️ v4 的响应结构与驾车 v3 **不同**：路径在 `data.paths[]`（不是 `route.paths[]`），
 *    且没有 `action` 转向语义。故不能直接复用 normalizeRoute，
 *    需要单独归一化（见 amap.js 的 fetchBicyclingRoute 与 normalize.js 的 normalizeBicycling）。
 */
export const AMAP_BICYCLING_URL = 'https://restapi.amap.com/v4/direction/bicycling';

/**
 * 从环境变量构建配置。
 * @param {Record<string,string|undefined>} env
 * @param {{requireKey?: boolean}} [opts] 测试时可关闭 Key 必填
 */
export function loadConfig(env = process.env, opts = {}) {
  const requireKey = opts.requireKey !== false;

  const amapKey = (env.AMAP_KEY ?? '').trim();
  if (requireKey && amapKey.length === 0) {
    throw new Error(
      '缺少环境变量 AMAP_KEY。请在启动前设置（例如：AMAP_KEY=xxx npm start）。\n' +
        '申请方式见 docs/nav-backend.md §3.2（服务平台必须选「Web 服务」）。',
    );
  }

  const authToken = (env.GATEWAY_TOKEN ?? '').trim();
  if (requireKey && authToken.length === 0) {
    throw new Error(
      '缺少环境变量 GATEWAY_TOKEN。网关必须要求认证（CORS 不是认证）。\n' +
        '生成方式：node -e "console.log(crypto.randomUUID())"',
    );
  }

  return {
    amapKey,
    authToken,
    /*
     * 监听地址。
     *
     * ⚠️ 默认 `127.0.0.1`（**只回环**）—— 这是刻意的安全默认值：
     *   - 本机开发：直接可用，不暴露到局域网。
     *   - 反向代理（nginx/Caddy 终止 TLS）：反代与网关同机，回环即可，
     *     **公网流量进不来**，网关不可能被绕过反代直连。
     *   - 若要网关直接对外（无 TLS、或网关自己处理 TLS），显式设 HOST=0.0.0.0。
     * 早期版本写死"绑所有网卡"，属默认不安全，已改。
     *
     * ⚠️ 空白值（.env 里写了 `HOST=` 但没填）也回落到默认 ——
     *    否则会变成监听空地址，表现为"服务起不来且原因不明"。
     */
    host: (env.HOST ?? '').trim() || '127.0.0.1',
    port: Number(env.PORT ?? 8080),
    /*
     * 反向代理场景下信任的代理跳数（用于记录真实客户端 IP）。
     * 默认 0 = 不信任任何 X-Forwarded-For（避免被伪造）。
     * 同机反代可设 1。
     */
    trustProxy: Number(env.TRUST_PROXY ?? 0),
    /*
     * 周边地图瓦片的磁盘缓存目录。
     * 必须缓存：OpenFreeMap 的离线额度是"每次 ≤3000 瓦片"，
     * 而导航中同一个区域会被反复请求（实测首次 1.7s、缓存后 7ms）。
     */
    mapCacheDir: (env.MAP_CACHE_DIR ?? '.cache/map-tiles').trim(),
    /** 每秒允许的请求数（自用规模足够；防止误用刷爆高德配额） */
    rateLimitPerMin: Number(env.RATE_LIMIT_PER_MIN ?? 60),
    /** 单次上游请求超时（毫秒） */
    upstreamTimeoutMs: Number(env.UPSTREAM_TIMEOUT_MS ?? 8000),
    /** 是否允许把所有错误细节回给客户端（仅本地开发可开；默认关，避免信息泄露） */
    verboseErrors: env.VERBOSE_ERRORS === '1',
  };
}

/**
 * 日志脱敏：把 Key / token 替换为占位符。
 * 用于任何可能打印 URL 或配置的地方。
 */
export function redact(text, secrets = []) {
  let out = String(text);
  for (const s of secrets) {
    if (typeof s === 'string' && s.length >= 6) {
      out = out.split(s).join('***');
    }
  }
  // 兜底：把形如 key=xxx / token=xxx 的值也遮蔽（防止漏传 secrets）。
  // 注意要覆盖三种起始情形：URL 查询串（?key= / &key=）、
  // 字符串开头（key=）、以及其它以空白分隔的场合（"token=abc ..."）。
  return out.replace(/(^|[?&\s])((?:key|token)=)[^&\s]+/gi, '$1$2***');
}
