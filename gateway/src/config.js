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
    port: Number(env.PORT ?? 8080),
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
