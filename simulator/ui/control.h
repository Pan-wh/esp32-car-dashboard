/**
 * control.h —— 设备控制命令（BLE 下行，纯 C，模拟器/固件共用）
 *
 * 协议：与遥测上行一致走 JSON，便于 App 端用同一套 Codable 模型。
 *   {"cmd":"brightness","value":128}    亮度 0-255（0 视为最低可见亮度 1）
 *   {"cmd":"screen","value":"off"}      息屏（仅关显示，触摸仍可唤醒由固件决定）
 *   {"cmd":"screen","value":"on"}       亮屏
 *   {"cmd":"face","value":2}            切换表盘（功能③预留）
 *   {"cmd":"face_enabled","value":3,"on":0}  启用/隐藏某表盘（on 也接受 "on"/"off"）
 *   {"cmd":"rotate","value":90}         屏幕方向 0/90/180/270（度）
 *
 * 屏幕方向说明（rotate）：
 *   用**角度**而非"索引"表达，避免与其余命令的 0 基索引混淆。
 *   仅接受 90 的整数倍（其余值判为非法，避免误配方向）；
 *   超出 0-359 的值按 mod 360 归一（如 -90 → 270）。
 *   实现走 LVGL 软件旋转（官方支持，见 06_LVGL_Widgets 示例）；
 *   本屏为**圆形**，旋转后仍是同一个圆，无长宽比/布局副作用。
 *
 * 设计原则：解析层不含任何硬件调用，纯函数 + 结构体输出，便于单测；
 * 硬件副作用（setBrightness / displayOff）由固件侧应用。
 */
#ifndef CONTROL_H
#define CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONTROL_NONE = 0,   /* 无法识别的命令 */
    CONTROL_BRIGHTNESS, /* value = 0..255 */
    CONTROL_SCREEN,     /* value = 0 关屏 / 1 亮屏 */
    CONTROL_FACE,       /* value = 表盘 id（>=0）*/
    CONTROL_FACE_ENABLED, /* value = 表盘 id；value2 = 1 启用 / 0 隐藏 */
    CONTROL_ROTATE,     /* value = 屏幕方向角度：0 / 90 / 180 / 270 */
    CONTROL_DEMO_SPEED, /* value = 演示车速 km/h；< 0 表示退出演示 */
} control_kind_t;

typedef struct {
    control_kind_t kind;
    int32_t value;  /* 主参数，见 control_kind_t 说明 */
    int32_t value2; /* 次参数；目前仅 CONTROL_FACE_ENABLED 使用（1 启用 / 0 隐藏）*/
} control_cmd_t;

/**
 * 解析一条控制命令。
 * @return true 表示解析成功且 kind != CONTROL_NONE。
 *         失败时 out->kind = CONTROL_NONE（不修改其他字段语义）。
 */
bool control_parse(const char *json, size_t len, control_cmd_t *out);

/** 亮度限幅：0 → 1（避免全黑误以为死机），>255 → 255 */
uint8_t control_clamp_brightness(int32_t v);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_H */
