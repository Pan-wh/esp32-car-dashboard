/**
 * nav_map.h —— 周边地图窗口的接收缓冲与解析（纯 C，可 PC 单测）
 *
 * ## 它解决什么
 *
 * 圆屏要画"眼前的路"（灰路 + 灰楼 + 白路线 + 固定车头箭头），需要周边几何。
 * 数据由手机/网关按"当前位置附近约 500 米"筛好、**投影成局部米坐标**后下发
 * （协议见 docs/ble.md「地图窗口消息」）。
 *
 * ## 关键约定
 *
 * - **坐标是局部米**：以窗口中心为原点，x 向东、y 向北，int16 足够（±1500m）。
 *   设备端**不做任何经纬度/墨卡托运算** —— 投影已在网关完成。
 * - **一条要素一条消息**（`mr` / `mb` / `mp`）：每条都小于一个 BLE 包，
 *   因此**不需要分片、序号、CRC**，直接复用既有的文本消息通路。
 * - **原子切换**：`mw` 开始累积，`me` 才整体切到"待用缓冲"，避免画到一半的中间态。
 *
 * ## 内存
 *
 * 窗口结构约 2.7 KB，双缓冲（累积 + 待用）约 5.4 KB 静态占用。
 * 全部静态分配，不使用堆 —— 避免长期运行产生碎片。
 */

#ifndef NAV_MAP_H
#define NAV_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 背景道路条数上限（与网关侧预算一致） */
#define NAV_MAP_MAX_ROADS 24
/** 背景道路点数上限（跨所有道路） */
#define NAV_MAP_MAX_ROAD_PTS 192
/** 建筑栋数上限 */
#define NAV_MAP_MAX_BLDGS 16
/** 建筑点数上限（跨所有建筑） */
#define NAV_MAP_MAX_BLDG_PTS 128
/**
 * 路线折线点数上限。
 *
 * 与 App 的 `MapWindowCodec.maxRoutePoints` 一致。取值要够大，才能让
 * 手机一次发来"远超屏幕可见范围"的路线段 —— 白线在屏幕中间断掉会被
 * 误解成"路只到那里"（实测反馈）。
 */
#define NAV_MAP_MAX_ROUTE_PTS 96
/**
 * **单条要素**的点数上限。
 * 与网关的 `maxPointsPerFeature` 一致；网关已保证不超，这里是防御性上限。
 */
#define NAV_MAP_PTS_PER_FEATURE 16

/**
 * 累积超时（毫秒）：收到 `mw` 后这么久仍未收到 `me`，则丢弃本次累积。
 * 防止把"两次请求混合的半截几何"画到屏幕上。
 */
#define NAV_MAP_BUILD_TIMEOUT_MS 3000u

/**
 * 窗口过期（毫秒）：这么久没有新窗口，则认为手机断连或无网，
 * 隐藏地图层（路线与转向提示仍来自 `nav` 消息，不受影响）。
 */
#define NAV_MAP_STALE_MS 60000u

/** 局部米坐标点（相对窗口中心；x 向东，y 向北） */
typedef struct {
    int16_t x;
    int16_t y;
} nav_map_pt_t;

/** 一条背景道路 */
typedef struct {
    uint8_t cls;                              /**< 等级 0..5，越小越主要 */
    uint8_t n;                                /**< 实际点数 */
    nav_map_pt_t pts[NAV_MAP_PTS_PER_FEATURE];
} nav_map_road_t;

/** 一栋建筑轮廓（不闭合存储，绘制方负责闭合） */
typedef struct {
    uint8_t n;
    nav_map_pt_t pts[NAV_MAP_PTS_PER_FEATURE];
} nav_map_bldg_t;

/** 一个完整的地图窗口 */
typedef struct {
    bool used;                                /**< 是否已被写入过 */
    uint8_t n_roads;
    uint8_t n_bldgs;
    uint8_t n_route;
    nav_map_road_t roads[NAV_MAP_MAX_ROADS];
    nav_map_bldg_t bldgs[NAV_MAP_MAX_BLDGS];
    nav_map_pt_t route[NAV_MAP_MAX_ROUTE_PTS];
} nav_map_window_t;

/** 复位（清空累积与待用缓冲） */
void nav_map_init(void);

/**
 * 处理一条可能是地图消息的 JSON。
 *
 * @return true 表示**确实是**地图消息并已处理（调用方就不必再尝试遥测解析）；
 *         false 表示不是地图消息，调用方应继续尝试其它解析器。
 */
bool nav_map_handle(const char *json, size_t len);

/**
 * 取当前**可用**的窗口。
 *
 * @return 有已完成且未过期的窗口时返回指针，否则返回 NULL（调用方应隐藏地图层）。
 */
const nav_map_window_t *nav_map_active(void);

/**
 * 周期性维护：处理"累积超时"与"窗口过期"。应在主循环调用。
 *
 * @param now_ms 单调毫秒
 */
void nav_map_tick(uint32_t now_ms);

/**
 * 记录**自车在窗口局部坐标系中的偏移**（米）。
 *
 * 由 `nav` 消息的 `ox`/`oy` 字段带来（见 docs/ble.md）。窗口原点在
 * "手机发起请求时的位置"，而车在动，所以每帧都要用最新偏移把地图整体平移，
 * 使自车始终落在屏幕中心的固定箭头上。
 */
void nav_map_set_vehicle_offset(int ox_m, int oy_m);

/**
 * 取自车偏移。仅在**有可用窗口**时有意义。
 * @return true 表示已设置过
 */
bool nav_map_get_vehicle_offset(int *ox_m, int *oy_m);

/** 统计：收到的地图消息条数（调试用） */
uint32_t nav_map_msg_count(void);
/** 统计：成功切换的窗口数（调试用） */
uint32_t nav_map_window_count(void);
/** 统计：因超时被丢弃的累积次数（调试用） */
uint32_t nav_map_drop_count(void);

#ifdef __cplusplus
}
#endif

#endif /* NAV_MAP_H */
