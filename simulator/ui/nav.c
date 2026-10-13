/**
 * nav.c —— 导航指引卡片纯逻辑实现（无 LVGL / 无硬件依赖）
 *
 * 有意保持零依赖：只有整数运算与 snprintf，便于在 PC 上单测。
 * UI 与协议层都复用同一份格式化规则，避免"固件一份、测试一份"的漂移。
 */
#include "nav.h"

#include <stdio.h>
#include <string.h>

const char *nav_maneuver_name(nav_maneuver_t m)
{
    switch (m) {
        case NAV_MANEUVER_STRAIGHT:   return "straight";
        case NAV_MANEUVER_LEFT:       return "left";
        case NAV_MANEUVER_RIGHT:      return "right";
        case NAV_MANEUVER_U_TURN:     return "u_turn";
        case NAV_MANEUVER_KEEP_LEFT:  return "keep_left";
        case NAV_MANEUVER_KEEP_RIGHT: return "keep_right";
        case NAV_MANEUVER_ARRIVE:     return "arrive";
        default:                      return "unknown";
    }
}

const char *nav_state_name(nav_state_t st)
{
    switch (st) {
        case NAV_STATE_IDLE:      return "idle";
        case NAV_STATE_ACTIVE:    return "active";
        case NAV_STATE_REROUTING: return "rerouting";
        case NAV_STATE_ARRIVED:   return "arrived";
        case NAV_STATE_STALE:     return "stale";
        default:                  return "no_data";
    }
}

const char *nav_state_text(nav_state_t st)
{
    switch (st) {
        case NAV_STATE_IDLE:      return "NO NAV";
        case NAV_STATE_ACTIVE:    return "NAV";
        case NAV_STATE_REROUTING: return "REROUTE";
        case NAV_STATE_ARRIVED:   return "ARRIVED";
        case NAV_STATE_STALE:     return "STALE";
        default:                  return "NO DATA";
    }
}

bool nav_shows_maneuver(nav_state_t st)
{
    /*
     * 显示箭头区域的状态：
     *   ACTIVE / REROUTING —— 正常显示下一步动作
     *   ARRIVED            —— 显示"目的地"符号（UI 会强制换成 arrive 图标）
     *
     * 不显示（安全优先，§6.3「不得用旧值冒充当前值」）：
     *   IDLE     —— 用户没在导航，显示"未开始"即可
     *   STALE    —— 数据过期，**绝不能**继续显示旧指引
     *   NO_DATA  —— 无数据
     */
    return (st == NAV_STATE_ACTIVE) || (st == NAV_STATE_REROUTING) || (st == NAV_STATE_ARRIVED);
}

nav_maneuver_t nav_clamp_maneuver(int raw)
{
    if (raw < 0 || raw >= NAV_MANEUVER_COUNT) return NAV_MANEUVER_UNKNOWN;
    return (nav_maneuver_t)raw;
}

nav_state_t nav_clamp_state(int raw)
{
    if (raw < 0 || raw >= NAV_STATE_COUNT) return NAV_STATE_NO_DATA;
    return (nav_state_t)raw;
}

/* 距离是否可用于显示 */
static bool distance_usable(int meters)
{
    return (meters >= 0) && (meters <= NAV_DISTANCE_MAX_M);
}

void nav_format_distance_value(int meters, char *buf, size_t sz)
{
    if (buf == NULL || sz == 0) return;

    if (!distance_usable(meters)) {
        snprintf(buf, sz, "--");
        return;
    }

    if (meters < 1000) {
        snprintf(buf, sz, "%d", meters);
    }
    else if (meters < 10000) {
        /* 1000..9999 → 一位小数公里：1250 → "1.2"（四舍五入到 0.1km）*/
        int tenths = (meters + 50) / 100; /* 0.1km 为单位，四舍五入 */
        snprintf(buf, sz, "%d.%d", tenths / 10, tenths % 10);
    }
    else {
        /* >= 10000 → 整数公里：12500 → "12"（四舍五入）*/
        int km = (meters + 500) / 1000;
        snprintf(buf, sz, "%d", km);
    }
}

const char *nav_distance_unit(int meters)
{
    if (!distance_usable(meters)) return "";
    if (meters < 1000) return "m";
    return "km";
}

void nav_format_distance(int meters, char *buf, size_t sz)
{
    if (buf == NULL || sz == 0) return;

    char num[16];
    nav_format_distance_value(meters, num, sizeof(num));
    const char *unit = nav_distance_unit(meters);

    if (unit[0] == '\0') {
        /* 无效：只显示 "--"，不拼单位，避免出现 "500 km" 这类误导 */
        snprintf(buf, sz, "%s", num);
    }
    else {
        snprintf(buf, sz, "%s %s", num, unit);
    }
}
