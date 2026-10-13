/**
 * maneuver.test.js —— 动作映射测试
 *
 * 重点覆盖**顺序敏感**的用例（"左转掉头"必须判为掉头、"靠左"不能判成左转），
 * 这类 bug 一旦发生，仪表会给出**方向相反**的指引 —— 比"未知"危险得多。
 */
import test from 'node:test';
import assert from 'node:assert/strict';

import {
  MANEUVER,
  maneuverFromInstruction,
  maneuverFromAction,
  maneuverFromText,
  resolveManeuver,
  maneuverName,
  ACTION_TEXT_MAP,
} from '../src/maneuver.js';

test('枚举值与固件 nav.h 逐值一致（改这里必须同步改固件）', () => {
  assert.equal(MANEUVER.UNKNOWN, 0);
  assert.equal(MANEUVER.STRAIGHT, 1);
  assert.equal(MANEUVER.LEFT, 2);
  assert.equal(MANEUVER.RIGHT, 3);
  assert.equal(MANEUVER.U_TURN, 4);
  assert.equal(MANEUVER.KEEP_LEFT, 5);
  assert.equal(MANEUVER.KEEP_RIGHT, 6);
  assert.equal(MANEUVER.ARRIVE, 7);
});

test('文本匹配：基本动作', () => {
  assert.equal(maneuverFromInstruction('左转'), MANEUVER.LEFT);
  assert.equal(maneuverFromInstruction('右转'), MANEUVER.RIGHT);
  assert.equal(maneuverFromInstruction('直行'), MANEUVER.STRAIGHT);
  assert.equal(maneuverFromInstruction('掉头'), MANEUVER.U_TURN);
  assert.equal(maneuverFromInstruction('靠左行驶'), MANEUVER.KEEP_LEFT);
  assert.equal(maneuverFromInstruction('靠右行驶'), MANEUVER.KEEP_RIGHT);
  assert.equal(maneuverFromInstruction('到达目的地'), MANEUVER.ARRIVE);
});

test('★ 顺序敏感：掉头优先于左/右转', () => {
  // "左转掉头" 同时含"左转"和"掉头"，必须先判掉头，否则显示左转（严重误导）
  assert.equal(maneuverFromInstruction('左转掉头'), MANEUVER.U_TURN);
  assert.equal(maneuverFromInstruction('右转掉头'), MANEUVER.U_TURN);
  assert.equal(maneuverFromInstruction('前方掉头'), MANEUVER.U_TURN);
  assert.equal(maneuverFromInstruction('调头'), MANEUVER.U_TURN);
});

test('★ 顺序敏感：靠左/靠右优先于左/右转', () => {
  // "靠左行驶" 含"左"，若不先判靠左会误判成左转（方向不同）
  assert.equal(maneuverFromInstruction('靠左行驶'), MANEUVER.KEEP_LEFT);
  assert.equal(maneuverFromInstruction('前方靠左'), MANEUVER.KEEP_LEFT);
  assert.equal(maneuverFromInstruction('靠右行驶'), MANEUVER.KEEP_RIGHT);
  assert.equal(maneuverFromInstruction('前方靠右'), MANEUVER.KEEP_RIGHT);
});

test('左前方/右前方归入左转/右转（当前显示只有 8 种动作）', () => {
  assert.equal(maneuverFromInstruction('向左前方行驶'), MANEUVER.LEFT);
  assert.equal(maneuverFromInstruction('向右前方行驶'), MANEUVER.RIGHT);
});

test('到达类文本', () => {
  assert.equal(maneuverFromInstruction('到达目的地'), MANEUVER.ARRIVE);
  assert.equal(maneuverFromInstruction('抵达终点'), MANEUVER.ARRIVE);
});

test('无法判定 → 未知（安全降级，绝不能猜成某个方向）', () => {
  assert.equal(maneuverFromInstruction('进入环岛'), MANEUVER.UNKNOWN);
  assert.equal(maneuverFromInstruction(''), MANEUVER.UNKNOWN);
  assert.equal(maneuverFromInstruction(null), MANEUVER.UNKNOWN);
  assert.equal(maneuverFromInstruction(undefined), MANEUVER.UNKNOWN);
  assert.equal(maneuverFromInstruction(123), MANEUVER.UNKNOWN);
});

test('★ 实测动作词表：真实响应中出现过的 action 值全部正确映射', () => {
  // 这些取值来自用真实 Key 拉到的响应（见 fixtures/amap-driving-real.json 及
  // 多路线探测记录）。若高德改词，这条测试会失败 —— 正是我们想要的提醒。
  const observed = {
    '左转': MANEUVER.LEFT,
    '右转': MANEUVER.RIGHT,
    '直行': MANEUVER.STRAIGHT,
    '靠左': MANEUVER.KEEP_LEFT,
    '靠右': MANEUVER.KEEP_RIGHT,
    '左转调头': MANEUVER.U_TURN,
    '向左前方行驶': MANEUVER.LEFT,
    '向右前方行驶': MANEUVER.RIGHT,
  };
  for (const [text, expect] of Object.entries(observed)) {
    assert.equal(maneuverFromAction(text), expect, `action "${text}" 应映射为 ${expect}`);
  }
  // 词表必须真的收录了这些实测值（防止有人误删）
  for (const text of Object.keys(observed)) {
    assert.ok(Object.prototype.hasOwnProperty.call(ACTION_TEXT_MAP, text),
      `ACTION_TEXT_MAP 应包含实测值 "${text}"`);
  }
});

test('★ action 是高德给的**规范化文本**，应优先于 instruction 长句', () => {
  // 实测：action 为规范短词，instruction 是长句。
  // 若二者"看起来冲突"，应以 action 为准（它是高德规范过的）。
  assert.equal(resolveManeuver({ action: '左转', instruction: '沿某某路向东行驶200米右转' }),
    MANEUVER.LEFT, 'action 优先');
  assert.equal(resolveManeuver({ action: '靠左', instruction: '向东行驶4.6千米靠左沿主路行驶' }),
    MANEUVER.KEEP_LEFT);
});

test('resolveManeuver：action 为空时退回 instruction（实测到达段即如此）', () => {
  // 实测：最后一段 action 为空字符串，靠 instruction "…到达目的地" 判定
  assert.equal(resolveManeuver({ action: '', instruction: '向北行驶162米到达目的地' }),
    MANEUVER.ARRIVE);
  assert.equal(resolveManeuver({ action: '   ', instruction: '右转' }), MANEUVER.RIGHT);
  assert.equal(resolveManeuver({ action: undefined, instruction: '左转' }), MANEUVER.LEFT);
});

test('★ 实测类型差异：action 可能是字符串或空数组，assistant_action 可能是数组或字符串', () => {
  // 实测（真实响应末段）：action = []（空**数组**），
  //                        assistant_action = "到达目的地"（**字符串**）
  // 而多数段：action = "左转"（字符串），assistant_action = []（空数组）
  // 两侧类型都可能互换，故解析必须对两种类型都健壮。
  assert.equal(maneuverFromAction([]), MANEUVER.UNKNOWN, 'action 为空数组 → 未知（不崩溃）');
  assert.equal(maneuverFromAction('左转'), MANEUVER.LEFT, 'action 为字符串 → 正常');
  assert.equal(maneuverFromAction({}), MANEUVER.UNKNOWN, 'action 为对象 → 未知');

  assert.equal(resolveManeuver({ action: [], assistant_action: '到达目的地' }), MANEUVER.ARRIVE,
    '实测末段形态：action=[] + assistant_action="到达目的地"');
  assert.equal(resolveManeuver({ action: [], assistant_action: ['到达目的地'] }), MANEUVER.ARRIVE,
    'assistant_action 为数组同样支持');
  assert.equal(resolveManeuver({ action: [], assistant_action: [] }), MANEUVER.UNKNOWN);
  assert.equal(resolveManeuver({ action: [], assistant_action: '', instruction: '到达目的地' }),
    MANEUVER.ARRIVE, 'assistant_action 为空时继续退回 instruction');
});

test('resolveManeuver：全都无法判定 → 未知（绝不猜方向）', () => {
  assert.equal(resolveManeuver({ action: '进入环岛', instruction: '进入环岛' }), MANEUVER.UNKNOWN,
    '环岛无对应枚举，应为未知而非猜一个方向');
  assert.equal(resolveManeuver({ action: '上桥', instruction: '沿匝道上桥' }), MANEUVER.UNKNOWN);
  assert.equal(resolveManeuver(null), MANEUVER.UNKNOWN);
  assert.equal(resolveManeuver({}), MANEUVER.UNKNOWN);
  assert.equal(resolveManeuver({ action: 123 }), MANEUVER.UNKNOWN, '数字（旧假设）应安全降级');
});

test('maneuverFromText：空白与非法输入的边界', () => {
  assert.equal(maneuverFromText(''), MANEUVER.UNKNOWN);
  assert.equal(maneuverFromText('   '), MANEUVER.UNKNOWN);
  assert.equal(maneuverFromText(null), MANEUVER.UNKNOWN);
  assert.equal(maneuverFromText(undefined), MANEUVER.UNKNOWN);
  assert.equal(maneuverFromText(42), MANEUVER.UNKNOWN);
  // 前后空格应被容忍
  assert.equal(maneuverFromText('  左转  '), MANEUVER.LEFT);
});

test('maneuverName 与固件 nav_maneuver_name 一致', () => {
  assert.equal(maneuverName(MANEUVER.STRAIGHT), 'straight');
  assert.equal(maneuverName(MANEUVER.LEFT), 'left');
  assert.equal(maneuverName(MANEUVER.RIGHT), 'right');
  assert.equal(maneuverName(MANEUVER.U_TURN), 'u_turn');
  assert.equal(maneuverName(MANEUVER.KEEP_LEFT), 'keep_left');
  assert.equal(maneuverName(MANEUVER.KEEP_RIGHT), 'keep_right');
  assert.equal(maneuverName(MANEUVER.ARRIVE), 'arrive');
  assert.equal(maneuverName(MANEUVER.UNKNOWN), 'unknown');
  assert.equal(maneuverName(99), 'unknown');
});
