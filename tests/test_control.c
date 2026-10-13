/**
 * test_control.c —— 设备控制命令解析单元测试（PC 上运行，不需要硬件）
 *
 * 运行：cd tests && make test_control && ./test_control
 * 覆盖：三种命令、字符串/数字型 value、未知命令、格式错误、缺字段、空白容忍、限幅
 */
#include <stdio.h>
#include <string.h>

#include "control.h"

static int g_pass, g_fail;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                      \
    } while (0)

/* 便利：解析字符串字面量并返回是否成功 */
#define PARSE(lit, cmd) control_parse((lit), strlen(lit), &(cmd))

int main(void)
{
    control_cmd_t c;

    printf("=== 控制命令解析 ===\n");

    /* ---- 亮度 ---- */
    CHECK(PARSE("{\"cmd\":\"brightness\",\"value\":128}", c) && c.kind == CONTROL_BRIGHTNESS &&
              c.value == 128,
          "亮度 128");
    CHECK(PARSE("{\"cmd\":\"brightness\",\"value\":0}", c) && c.kind == CONTROL_BRIGHTNESS &&
              c.value == 0,
          "亮度 0（解析层保留原值，限幅在 control_clamp_brightness）");
    CHECK(PARSE("{\"cmd\":\"brightness\",\"value\":255}", c) && c.value == 255, "亮度 255");
    CHECK(PARSE("{\"cmd\":\"brightness\",\"value\":999}", c) && c.value == 999,
          "亮度越界值由解析层原样返回");

    /* ---- 息屏/亮屏（数字与字符串两种写法）---- */
    CHECK(PARSE("{\"cmd\":\"screen\",\"value\":\"off\"}", c) && c.kind == CONTROL_SCREEN &&
              c.value == 0,
          "screen off（字符串）");
    CHECK(PARSE("{\"cmd\":\"screen\",\"value\":\"on\"}", c) && c.kind == CONTROL_SCREEN &&
              c.value == 1,
          "screen on（字符串）");
    CHECK(PARSE("{\"cmd\":\"screen\",\"value\":0}", c) && c.kind == CONTROL_SCREEN && c.value == 0,
          "screen 0（数字）");
    CHECK(PARSE("{\"cmd\":\"screen\",\"value\":1}", c) && c.kind == CONTROL_SCREEN && c.value == 1,
          "screen 1（数字）");
    CHECK(PARSE("{\"cmd\":\"screen\",\"value\":7}", c) && c.value == 1, "screen 非零归一为 1");

    /* ---- 表盘（功能③预留）---- */
    CHECK(PARSE("{\"cmd\":\"face\",\"value\":3}", c) && c.kind == CONTROL_FACE && c.value == 3,
          "face 3");

    /* ---- 字段顺序无关 ---- */
    CHECK(PARSE("{\"value\":2,\"cmd\":\"face\"}", c) && c.kind == CONTROL_FACE && c.value == 2,
          "字段顺序颠倒仍可解析");

    /* ---- 空白容忍 ---- */
    CHECK(PARSE("{ \"cmd\" : \"face\" , \"value\" : 1 }", c) && c.kind == CONTROL_FACE &&
              c.value == 1,
          "多余空白");

    /* ---- 错误与边界 ---- */
    CHECK(!PARSE("{\"cmd\":\"unknown\",\"value\":1}", c) && c.kind == CONTROL_NONE, "未知命令");
    CHECK(!PARSE("{\"cmd\":\"brightness\"}", c) && c.kind == CONTROL_NONE, "缺 value");
    CHECK(!PARSE("{\"value\":5}", c) && c.kind == CONTROL_NONE, "缺 cmd");
    CHECK(!PARSE("{}", c) && c.kind == CONTROL_NONE, "空对象");
    CHECK(!PARSE("not json", c) && c.kind == CONTROL_NONE, "非 JSON");
    CHECK(!PARSE("{\"cmd\":\"face\",\"value\":-1}", c) && c.kind == CONTROL_NONE, "负表盘 id");
    CHECK(!PARSE("{\"cmd\":\"face\"", c) && c.kind == CONTROL_NONE, "未闭合对象");
    CHECK(!PARSE("{\"cmd\":\"face\",\"value\":}", c) && c.kind == CONTROL_NONE, "缺值");
    CHECK(!control_parse(NULL, 0, &c), "NULL 输入");
    CHECK(!control_parse("{\"cmd\":\"face\",\"value\":1}", 5, &c), "截断输入（长度不足）");

    /* ---- 屏幕方向（rotate）---- */
    printf("=== 屏幕方向命令 ===\n");
    CHECK(PARSE("{\"cmd\":\"rotate\",\"value\":0}", c) && c.kind == CONTROL_ROTATE && c.value == 0,
          "rotate 0°（正常）");
    CHECK(PARSE("{\"cmd\":\"rotate\",\"value\":90}", c) && c.value == 90, "rotate 90°");
    CHECK(PARSE("{\"cmd\":\"rotate\",\"value\":180}", c) && c.value == 180, "rotate 180°");
    CHECK(PARSE("{\"cmd\":\"rotate\",\"value\":270}", c) && c.value == 270, "rotate 270°");

    /* 归一化：超范围按 mod 360 折算 */
    CHECK(PARSE("{\"cmd\":\"rotate\",\"value\":360}", c) && c.value == 0, "360° 归一为 0");
    CHECK(PARSE("{\"cmd\":\"rotate\",\"value\":450}", c) && c.value == 90, "450° 归一为 90");
    CHECK(PARSE("{\"cmd\":\"rotate\",\"value\":-90}", c) && c.value == 270, "-90° 归一为 270");
    CHECK(PARSE("{\"cmd\":\"rotate\",\"value\":-180}", c) && c.value == 180, "-180° 归一为 180");

    /* 非法值必须被明确拒绝（而不是悄悄转错方向）*/
    CHECK(!PARSE("{\"cmd\":\"rotate\",\"value\":45}", c) && c.kind == CONTROL_NONE,
          "45° 非 90 倍数 → 拒绝");
    CHECK(!PARSE("{\"cmd\":\"rotate\",\"value\":1}", c) && c.kind == CONTROL_NONE,
          "1° → 拒绝（避免与 0 基索引混淆）");
    CHECK(!PARSE("{\"cmd\":\"rotate\",\"value\":89}", c) && c.kind == CONTROL_NONE,
          "89° → 拒绝");
    CHECK(!PARSE("{\"cmd\":\"rotate\"}", c) && c.kind == CONTROL_NONE, "缺 value → 拒绝");
    CHECK(!PARSE("{\"cmd\":\"rotate\",\"value\":\"on\"}", c) && c.kind == CONTROL_NONE,
          "字符串 value → 拒绝（rotate 只接受数字）");

    /* ---- 限幅 ---- */
    CHECK(control_clamp_brightness(0) == 1, "亮度 0 → 1（避免全黑）");
    CHECK(control_clamp_brightness(-5) == 1, "负亮度 → 1");
    CHECK(control_clamp_brightness(1) == 1, "亮度 1 保持");
    CHECK(control_clamp_brightness(200) == 200, "亮度 200 保持");
    CHECK(control_clamp_brightness(255) == 255, "亮度 255 保持");
    CHECK(control_clamp_brightness(300) == 255, "亮度越界 → 255");

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
