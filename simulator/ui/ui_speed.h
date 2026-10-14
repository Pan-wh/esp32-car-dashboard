/**
 * ui_speed.h —— 速度表盘页
 *
 * 数据来源：iPhone Core Location 经 BLE 下发（本板**无 GNSS**），
 * 由固件调用 ui_speed_set_speed() / ui_speed_set_fix() 驱动。
 *
 * 设计：**单一「驾驶员表盘」**（外圈刻度 + 指针 + 中央驾驶员插画 + 底部数字）。
 * 驾驶员的疲惫/专注/激动表情随车速变化。
 *
 * ⚠️ 早期曾有 V1–V4 四个候选变体与运行时 `--variant` 切换，
 *    定稿后已**全部移除**（`ui_speed_set_variant` 亦已不存在）。
 *    若在旧文档/脚本里看到 `--variant`，那是过期的。
 */
#ifndef UI_SPEED_H
#define UI_SPEED_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/* 驾驶员插画状态 */
#define UI_DRIVER_AUTO     (-1) /* 按速度自动 */
#define UI_DRIVER_NORMAL   0
#define UI_DRIVER_SPIRITED 1
#define UI_DRIVER_SPEEDING 2

/* 创建速度表盘到 parent（页面容器） */
lv_obj_t *ui_speed_create(lv_obj_t *parent);



/* 设置速度（km/h）；valid=false 时显示无效状态（"--"） */
void ui_speed_set_speed(float kmh);

/* 设置 GNSS 定位状态：valid=是否有定位，sats=卫星数 */
void ui_speed_set_fix(bool valid, int sats);

/* 驾驶员插画状态（UI_DRIVER_AUTO 表示按速度自动切换） */
void ui_speed_set_driver_state(int state);

/* 设置航向（度，0=正北顺时针）；保留数据接口，当前极简表盘不显示。 */
void ui_speed_set_heading(int deg);

/*
 * 行程数据（功能④）：保留统计接口，当前极简表盘不显示。
 */
void ui_speed_set_trip(float distance_km, float max_kmh);

/*
 * 开机扫表动画（#2b）：指针 0 → 满量程 → 回落，类似汽车自检。
 * 动画期间会屏蔽数据驱动的速度写入，结束后自动落位到最新真实值。
 * 指针尚未创建时（表盘未构建）调用为无操作。
 */
void ui_speed_sweep(void);

/* 立即中止扫表（例如表盘被销毁前） */
void ui_speed_sweep_stop(void);

/* 扫表进行中？ */
bool ui_speed_is_sweeping(void);

/* ---- 诊断接口（排查"速度 0 但指针不归零"）---- */
float ui_speed_get_speed(void);   /* 当前速度（km/h）*/
int ui_speed_get_valid(void);     /* 速度是否有效 */
void *ui_speed_get_needle(void);  /* V5 指针对象（NULL = 未创建）*/

/*
 * 数据链路状态（AI_TASK §6.4 五态）：驱动速度页顶部状态标签。
 * 与 ui_speed_set_fix()（控制指针/数字）正交：fix 管数值有效，status 管链路态。
 */
/* ⚠️ 取值与 telemetry.h 的 telemetry_link_t 前五项一一对应（CONNECTING/WAIT_LOC/
 *    INVALID/STALE/OK），固件可直接转换。改动其一必须同步另一个。 */
typedef enum {
    UI_STATUS_CONNECTING = 0, /* BLE 未连接 / 扫描中            → "BT OFF"  */
    UI_STATUS_WAIT_LOC,       /* 已连接，尚未收到定位           → "WAIT GPS"*/
    UI_STATUS_INVALID,        /* 收到数据但定位/速度无效          → "NO FIX"  */
    UI_STATUS_STALE,          /* 数据超时（超过 BLE_STALE_MS）    → "STALE"   */
    UI_STATUS_OK              /* 正常（有新鲜有效数据）           → "LIVE"    */
} ui_speed_status_t;

void ui_speed_set_status(ui_speed_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* UI_SPEED_H */
