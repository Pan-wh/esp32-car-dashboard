/**
 * maneuver.js —— 把高德返回的转向信息映射为本项目的动作枚举
 *
 * ## 我们的动作枚举（与 BLE 协议、固件 ui_nav 一致，**不可随意改动数值**）
 *
 *   0 未知 / 1 直行 / 2 左转 / 3 右转 / 4 掉头 / 5 靠左 / 6 靠右 / 7 到达
 *
 * ## 高德实际返回什么（**已用真实 Key 实测确认，2026-10-13**）
 *
 * ⚠️ 高德的 `steps[].action` **不是数字码，而是中文字符串**。
 *    实测观察到的取值：
 *      「左转」「右转」「直行」「靠左」「靠右」「左转调头」
 *      「向左前方行驶」「向右前方行驶」「」（空字符串 —— 最后一段到达时为空）
 *
 *    这一点与最初的假设相反（曾以为是数字码并预留了数值映射表）。
 *    验证方式：`npm run probe -- --from <lon,lat> --to <lon,lat>`
 *
 * ## 三级判定（从严到宽）
 *
 *   1. `action` **精确匹配**表（最可靠 —— 高德已把动作规范化过）
 *   2. `action` 正则兜底（应对未收录的新词）
 *   3. `instruction` 长句正则（应对 action 为空的情形，如到达）
 *   全都不中 → **未知**（安全：绝不猜方向）
 */

/** 动作枚举（与固件 simulator/ui/nav.h、Swift NavCore.swift 必须逐值一致） */
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
 * 高德 `action` 文本 → 我们的枚举（**精确匹配**）。
 *
 * 表内每一项都标注了来源：
 *   [实测] = 用真实 Key 观察到过
 *   [推断] = 未观察到，按高德动作命名惯例补全（命中则生效，不命中也不影响）
 */
export const ACTION_TEXT_MAP = Object.freeze({
  // —— [实测] 已用真实响应确认 ——
  '左转': MANEUVER.LEFT,
  '右转': MANEUVER.RIGHT,
  '直行': MANEUVER.STRAIGHT,
  '靠左': MANEUVER.KEEP_LEFT,
  '靠右': MANEUVER.KEEP_RIGHT,
  '左转调头': MANEUVER.U_TURN,
  '向左前方行驶': MANEUVER.LEFT,
  '向右前方行驶': MANEUVER.RIGHT,

  // —— [推断] 按命名惯例补全，待后续实测确认 ——
  '掉头': MANEUVER.U_TURN,
  '右转调头': MANEUVER.U_TURN,
  '向左后方行驶': MANEUVER.LEFT,
  '向右后方行驶': MANEUVER.RIGHT,
  '左前方行驶': MANEUVER.LEFT,
  '右前方行驶': MANEUVER.RIGHT,
  '到达目的地': MANEUVER.ARRIVE,
  '到达': MANEUVER.ARRIVE,
  '直行进入环岛': MANEUVER.STRAIGHT,
});

/**
 * 文本正则规则 —— **顺序敏感**，从具体到宽泛。
 *
 * 顺序为什么重要：
 *   - "左转掉头"同时含"左转"和"掉头" → 必须先判掉头，否则会误判成左转；
 *   - "靠左行驶"含"左" → 必须先判"靠左"，否则会误判成左转。
 *
 * 该规则同时用于 `action` 与 `instruction` 两种输入。
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

/** 用正则规则从任意中文文本推断动作（无法判定返回 UNKNOWN） */
export function maneuverFromText(text) {
  if (typeof text !== 'string' || text.length === 0) return MANEUVER.UNKNOWN;
  const t = text.trim();
  if (t.length === 0) return MANEUVER.UNKNOWN;

  // 1) 精确表（最可靠）
  if (Object.prototype.hasOwnProperty.call(ACTION_TEXT_MAP, t)) {
    return ACTION_TEXT_MAP[t];
  }
  // 2) 正则兜底
  for (const { re, m } of TEXT_RULES) {
    if (re.test(t)) return m;
  }
  return MANEUVER.UNKNOWN;
}

/**
 * 由高德 `action` 字段推断（字符串）。
 * @param {string} action
 * @returns {number} MANEUVER.*；无法判定返回 UNKNOWN
 */
export function maneuverFromAction(action) {
  return maneuverFromText(action);
}

/**
 * 由中文指引长句推断动作（`instruction`）。
 * @param {string} instruction
 */
export function maneuverFromInstruction(instruction) {
  return maneuverFromText(instruction);
}

/**
 * 组合推断：`action` 优先（更规范），其次 `instruction`（应对 action 为空）。
 *
 * @param {{action?: string, instruction?: string, assistant_action?: string[]|string}} step
 * @returns {number} MANEUVER.*
 */
export function resolveManeuver(step) {
  if (step == null) return MANEUVER.UNKNOWN;

  const byAction = maneuverFromText(step.action);
  if (byAction !== MANEUVER.UNKNOWN) return byAction;

  // assistant_action：高德用于"到达目的地"等辅助提示（实测样本中为空数组）
  const assist = step.assistant_action;
  const assistText = Array.isArray(assist) ? assist.join(' ') : assist;
  const byAssist = maneuverFromText(assistText);
  if (byAssist !== MANEUVER.UNKNOWN) return byAssist;

  // 最后退回长句（实测：最后一段 action 为空、instruction 为"…到达目的地"）
  return maneuverFromText(step.instruction);
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
