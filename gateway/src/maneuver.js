/**
 * maneuver.js —— 把高德返回的转向信息映射为本项目的动作枚举
 *
 * ## 我们的动作枚举（与 BLE 协议、固件 ui_nav 一致，**不可随意改动数值**）
 *
 *   0 未知 / 1 直行 / 2 左转 / 3 右转 / 4 掉头 / 5 靠左 / 6 靠右 / 7 到达
 *
 * ## 为什么以"指引文本"为主、数字码为辅
 *
 * 高德的转向信息有两种可能来源：
 *   1. `navi.action` —— 数字码。**不同接口版本/字段集的取值我们尚未实测确认**，
 *      因此**不猜**：码表默认为空，首次用 `npm run probe` 拉到真实响应后按实测填入。
 *   2. `instruction` —— 中文指引文本（如"左转""靠右""到达目的地"）。
 *      语义明确、可直接验证，因此作为**主要**依据。
 *
 * 两者结合：先查码表（若已填且命中），否则退回文本匹配，都失败则返回"未知"。
 * 文本匹配失败**不会**导致错误显示，只会退化为"未知"（安全）。
 */

/** 动作枚举（与固件 simulator/ui/nav.h 的 nav_maneuver_t 必须逐值一致） */
export const MANEUVER = Object.freeze({
  UNKNOWN: 0,
  STRAIGHT: 1,
  LEFT: 2,
  RIGHT: 3,
  U_TURN: 4,
  KEEP_LEFT: 5,
  KEEP_RIGHT: 6,
  ARRIVE: 7,
});

/**
 * 高德 `navi.action` 数字码 → 我们的枚举。
 *
 * ⚠️ **默认为空**。原因：不同接口版本（v3 基础版 / v5 + show_fields=navi）
 * 的 action 取值未能离线核实，**凭印象填写比留空更危险**（会把左转显示成右转）。
 *
 * 填入方法：
 *   1. `npm run probe -- --from <lon,lat> --to <lon,lat>`
 *   2. 观察输出中各 step 的 `action` 与 `instruction` 对应关系
 *   3. 在此表中补全，并同步更新 test/maneuver.test.js
 */
export const ACTION_CODE_MAP = Object.freeze({
  // 例如（**仅为格式示例，数值待实测确认，勿直接启用**）：
  // 1: MANEUVER.LEFT,
  // 2: MANEUVER.RIGHT,
});

/**
 * 文本匹配规则 —— **顺序敏感**，从具体到宽泛。
 *
 * 顺序为什么重要：
 *   - "左转掉头"同时含"左转"和"掉头" → 必须先判掉头，否则会误判成左转；
 *   - "靠左行驶"含"左" → 必须先判"靠左"，否则会误判成左转。
 */
const TEXT_RULES = [
  { re: /掉头|调头|迴转|回转/, m: MANEUVER.U_TURN },
  { re: /靠左|左侧行驶|偏左/, m: MANEUVER.KEEP_LEFT },
  { re: /靠右|右侧行驶|偏右/, m: MANEUVER.KEEP_RIGHT },
  { re: /到达|抵达|终点|目的地/, m: MANEUVER.ARRIVE },
  { re: /左前方|左前方行驶|向左前方/, m: MANEUVER.LEFT },
  { re: /右前方|右前方行驶|向右前方/, m: MANEUVER.RIGHT },
  { re: /左转|向左|左拐/, m: MANEUVER.LEFT },
  { re: /右转|向右|右拐/, m: MANEUVER.RIGHT },
  { re: /直行|沿.*直行|继续.*行驶|保持.*行驶/, m: MANEUVER.STRAIGHT },
];

/**
 * 由中文指引文本推断动作。
 * @param {string} instruction
 * @returns {number} MANEUVER.*；无法判定返回 UNKNOWN
 */
export function maneuverFromInstruction(instruction) {
  if (typeof instruction !== 'string' || instruction.length === 0) {
    return MANEUVER.UNKNOWN;
  }
  for (const { re, m } of TEXT_RULES) {
    if (re.test(instruction)) return m;
  }
  return MANEUVER.UNKNOWN;
}

/**
 * 由高德 action 数字码推断动作（依赖 ACTION_CODE_MAP；未收录返回 null）。
 * @param {number|string} code
 * @returns {number|null} null 表示"码表未收录"，调用方应退回文本匹配
 */
export function maneuverFromActionCode(code) {
  if (code === undefined || code === null || code === '') return null;
  const n = Number(code);
  if (!Number.isFinite(n)) return null;
  return Object.prototype.hasOwnProperty.call(ACTION_CODE_MAP, n)
    ? ACTION_CODE_MAP[n]
    : null;
}

/**
 * 组合推断：优先码表，其次文本，最后未知。
 * @param {{action?: number|string, instruction?: string}} step
 * @returns {number} MANEUVER.*
 */
export function resolveManeuver(step) {
  if (step == null) return MANEUVER.UNKNOWN;
  const byCode = maneuverFromActionCode(step.action);
  if (byCode !== null) return byCode;
  return maneuverFromInstruction(step.instruction);
}

/** 调试用：枚举值 → 稳定标识（与固件 nav_maneuver_name 一致） */
export function maneuverName(m) {
  switch (m) {
    case MANEUVER.STRAIGHT: return 'straight';
    case MANEUVER.LEFT: return 'left';
    case MANEUVER.RIGHT: return 'right';
    case MANEUVER.U_TURN: return 'u_turn';
    case MANEUVER.KEEP_LEFT: return 'keep_left';
    case MANEUVER.KEEP_RIGHT: return 'keep_right';
    case MANEUVER.ARRIVE: return 'arrive';
    default: return 'unknown';
  }
}
