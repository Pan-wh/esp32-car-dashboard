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
  maneuverFromActionCode,
  resolveManeuver,
  maneuverName,
  ACTION_CODE_MAP,
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

test('码表默认为空（未实测前不得凭印象填写）', () => {
  // 这条测试是"防呆"：若有人未实测就填了码表，请先确认 probe 结果并更新本测试
  assert.equal(Object.keys(ACTION_CODE_MAP).length, 0, 'ACTION_CODE_MAP 应为空，待实测填入');
  assert.equal(maneuverFromActionCode(1), null, '未收录的码应返回 null（而非猜测）');
  assert.equal(maneuverFromActionCode(undefined), null);
  assert.equal(maneuverFromActionCode('abc'), null);
});

test('resolveManeuver：码表未收录时退回文本匹配', () => {
  assert.equal(resolveManeuver({ instruction: '左转' }), MANEUVER.LEFT);
  assert.equal(resolveManeuver({ action: 999, instruction: '右转' }), MANEUVER.RIGHT);
  assert.equal(resolveManeuver({ action: 999 }), MANEUVER.UNKNOWN);
  assert.equal(resolveManeuver(null), MANEUVER.UNKNOWN);
  assert.equal(resolveManeuver({}), MANEUVER.UNKNOWN);
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
