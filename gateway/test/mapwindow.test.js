/**
 * mapwindow.test.js —— 周边地图窗口的契约测试
 *
 * 重点锁死一个**实测踩过**的坑：协议约定道路等级 `c` 是 0..5 的**整数**
 * （docs/ble.md / App 的 `Road.c: Int` / 固件 `road_width()` 三处都这么用），
 * 而瓦片里给的是 "service" / "tertiary" 这样的**字符串**。
 * 最初漏了转换直接透传 —— App 解码 Int 必然失败，现象是"窗口一直取不回来、
 * 圆屏上什么都没有"，排查花了很久。下面的断言就是防止它再发生。
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { roadClassToInt, DEFAULT_BUDGET } from '../src/mapwindow.js';

test('道路等级：OSM 类名映射为协议整数', () => {
  /* 对齐 docs/ble.md 的表格：0 高速 / 1 主干 / 2 次干 / 3 支路 / 4 辅路 / 5 其它 */
  assert.equal(roadClassToInt('motorway'), 0);
  assert.equal(roadClassToInt('trunk'), 0);
  assert.equal(roadClassToInt('primary'), 1);
  assert.equal(roadClassToInt('secondary'), 2);
  assert.equal(roadClassToInt('tertiary'), 3);
  assert.equal(roadClassToInt('minor'), 3);
  assert.equal(roadClassToInt('service'), 4);
});

test('道路等级：未知类名与空值一律归为 5（其它），绝不返回字符串', () => {
  for (const name of ['path', 'footway', 'rail', 'unknown', '', undefined, null]) {
    const v = roadClassToInt(name);
    assert.equal(typeof v, 'number', `类名 ${String(name)} 必须映射成数字`);
    assert.equal(v, 5);
  }
});

test('道路等级：取值恒在 0..5 内且为整数（防越界写坏固件缓冲）', () => {
  const names = ['motorway', 'trunk', 'primary', 'secondary', 'tertiary',
                 'minor', 'service', 'track', 'path', 'rail', 'transit', 'ferry', 'x'];
  for (const n of names) {
    const v = roadClassToInt(n);
    assert.ok(Number.isInteger(v), `${n} → ${v} 必须是整数`);
    assert.ok(v >= 0 && v <= 5, `${n} → ${v} 必须在 0..5 内`);
  }
});

test('预算：单条要素点数上限存在且为正值（保证单条消息放得进 BLE 单包）', () => {
  assert.ok(DEFAULT_BUDGET.maxPointsPerFeature >= 2);
  assert.ok(DEFAULT_BUDGET.maxPointsPerFeature <= 32,
            '单条要素点数过多会让消息超过 BLE 单包（约 185 字节）');
});
