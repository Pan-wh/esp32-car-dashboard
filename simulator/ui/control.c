/**
 * control.c —— 设备控制命令解析（纯 C，无硬件依赖，可单测）
 *
 * 实现说明：为保持与 telemetry.c 相同的「零依赖、可单测」风格，这里手写一个
 * **只认所需字段**的极简 JSON 扫描器：
 *   - 逐个 key/value 推进，遇到 "cmd" 记下字符串，遇到 "value" 记下数字或字符串
 *   - 不构建 DOM、不动态分配；出错即返回 false
 * 这样的好处是行为可预测（不引入完整 JSON 库），坏处是只支持扁平对象——
 * 而控制命令本来就约定为扁平结构。
 */
#include "control.h"

#include <string.h>

/* ---- 极简扫描器 ---- */

static void skip_ws(const char *s, size_t len, size_t *i)
{
    while (*i < len) {
        char c = s[*i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            (*i)++;
        }
        else {
            break;
        }
    }
}

static bool expect_char(const char *s, size_t len, size_t *i, char c)
{
    skip_ws(s, len, i);
    if (*i >= len || s[*i] != c) return false;
    (*i)++;
    return true;
}

/* 解析带引号字符串到 buf（不做转义处理：控制命令不含转义字符） */
static bool parse_string(const char *s, size_t len, size_t *i, char *buf, size_t buf_sz)
{
    if (!expect_char(s, len, i, '"')) return false;

    size_t n = 0;
    while (*i < len && s[*i] != '"') {
        if (n + 1 < buf_sz) buf[n++] = s[*i];
        (*i)++;
    }
    if (*i >= len) return false; /* 未闭合 */
    (*i)++;                      /* 跳过结尾引号 */
    buf[n] = '\0';
    return true;
}

static bool parse_int(const char *s, size_t len, size_t *i, int32_t *out)
{
    skip_ws(s, len, i);
    bool neg = false;
    if (*i < len && (s[*i] == '-' || s[*i] == '+')) {
        neg = (s[*i] == '-');
        (*i)++;
    }
    if (*i >= len || s[*i] < '0' || s[*i] > '9') return false;

    int32_t v = 0;
    while (*i < len && s[*i] >= '0' && s[*i] <= '9') {
        v = v * 10 + (s[*i] - '0');
        (*i)++;
    }
    *out = neg ? -v : v;
    return true;
}

bool control_parse(const char *json, size_t len, control_cmd_t *out)
{
    if (json == NULL || out == NULL) return false;

    out->kind = CONTROL_NONE;
    out->value = 0;
    out->value2 = 0;

    size_t i = 0;
    if (!expect_char(json, len, &i, '{')) return false;

    char key[24];
    char cmd[24];
    char sval[16];
    bool have_cmd = false, have_num = false, have_str = false;
    bool have_num2 = false;
    int32_t num = 0;
    int32_t num2 = 0;

    for (;;) {
        skip_ws(json, len, &i);
        if (i >= len) return false;

        if (json[i] == '}') break; /* 对象结束 */

        if (!parse_string(json, len, &i, key, sizeof(key))) return false;
        if (!expect_char(json, len, &i, ':')) return false;

        skip_ws(json, len, &i);
        if (i < len && json[i] == '"') {
            if (!parse_string(json, len, &i, sval, sizeof(sval))) return false;
            if (strcmp(key, "cmd") == 0) {
                strncpy(cmd, sval, sizeof(cmd) - 1);
                cmd[sizeof(cmd) - 1] = '\0';
                have_cmd = true;
            }
            else {
                have_str = true; /* 例如 value:"off" */
                /* 字符串型 value 归一成数字：on=1 / off=0 */
                if (strcmp(key, "value") == 0) {
                    if (strcmp(sval, "on") == 0) {
                        num = 1;
                        have_num = true;
                    }
                    else if (strcmp(sval, "off") == 0) {
                        num = 0;
                        have_num = true;
                    }
                }
                /* "on" 键同样接受字符串形式（与 screen 的写法保持一致） */
                else if (strcmp(key, "on") == 0) {
                    if (strcmp(sval, "on") == 0) {
                        num2 = 1;
                        have_num2 = true;
                    }
                    else if (strcmp(sval, "off") == 0) {
                        num2 = 0;
                        have_num2 = true;
                    }
                }
            }
        }
        else {
            int32_t v = 0;
            if (!parse_int(json, len, &i, &v)) return false;
            if (strcmp(key, "value") == 0) {
                num = v;
                have_num = true;
            }
            else if (strcmp(key, "on") == 0) {
                num2 = v;
                have_num2 = true;
            }
        }

        skip_ws(json, len, &i);
        if (i < len && json[i] == ',') {
            i++;
            continue;
        }
        if (i < len && json[i] == '}') break;
        return false;
    }

    if (!have_cmd) return false;

    if (strcmp(cmd, "brightness") == 0) {
        if (!have_num) return false;
        out->kind = CONTROL_BRIGHTNESS;
        out->value = num;
        return true;
    }
    if (strcmp(cmd, "screen") == 0) {
        if (!have_num) return false;
        out->kind = CONTROL_SCREEN;
        out->value = (num != 0) ? 1 : 0;
        return true;
    }
    if (strcmp(cmd, "face") == 0) {
        if (!have_num || num < 0) return false;
        out->kind = CONTROL_FACE;
        out->value = num;
        return true;
    }
    if (strcmp(cmd, "face_enabled") == 0) {
        /*
         * 启用/隐藏某个表盘（App 的分组开关）：
         *   {"cmd":"face_enabled","value":<表盘 id>,"on":1|0}
         * "on" 也接受字符串 "on"/"off"，与 screen 命令的写法一致。
         *
         * 这里只做**语法与取值域**校验（id >= 0、on 视为布尔）；
         * "不能隐藏最后一个表盘"这类**业务规则**由 ui_face_set_enabled 决定，
         * 它会返回 false，固件据此回执失败 —— 解析层不重复业务判断。
         */
        if (!have_num || num < 0) return false;
        if (!have_num2) return false;
        out->kind = CONTROL_FACE_ENABLED;
        out->value = num;
        out->value2 = (num2 != 0) ? 1 : 0;
        return true;
    }
    if (strcmp(cmd, "rotate") == 0) {
        if (!have_num) return false;
        /*
         * 归一化到 [0,360) 后要求是 90 的整数倍。
         * 严格校验的好处：方向配错会被**明确拒绝**（返回 false → 固件可回执错误），
         * 而不是悄悄转到一个意外角度。
         */
        int32_t deg = num % 360;
        if (deg < 0) deg += 360;
        if (deg % 90 != 0) return false;
        out->kind = CONTROL_ROTATE;
        out->value = deg;
        return true;
    }

    /*
     * 照片表盘的设置：每个开关独立一条命令（见 control.h 的说明）。
     * 这里只做**语法**校验（0/1、间隔范围），业务意义上的收敛交给 photo_slide。
     */
    if (strcmp(cmd, "photo_slide") == 0) {
        if (!have_num) return false;
        out->kind = CONTROL_PHOTO_SLIDE;
        out->value = (num != 0) ? 1 : 0;
        return true;
    }
    if (strcmp(cmd, "photo_shift") == 0) {
        if (!have_num) return false;
        out->kind = CONTROL_PHOTO_SHIFT;
        out->value = (num != 0) ? 1 : 0;
        return true;
    }
    if (strcmp(cmd, "photo_screenoff") == 0) {
        if (!have_num) return false;
        out->kind = CONTROL_PHOTO_SCREENOFF;
        out->value = (num != 0) ? 1 : 0;
        return true;
    }
    if (strcmp(cmd, "photo_interval") == 0) {
        /* App 传**秒**；这里只做粗校验，真正的夹取在 photo_settings_sanitize() */
        if (!have_num) return false;
        if (num < 0) num = 0;
        if (num > 86400) num = 86400;
        out->kind = CONTROL_PHOTO_INTERVAL;
        out->value = num;
        return true;
    }
    if (strcmp(cmd, "photo_get") == 0) {
        out->kind = CONTROL_PHOTO_GET;
        out->value = 0;
        return true;
    }
    if (strcmp(cmd, "photo_next") == 0) {
        out->kind = CONTROL_PHOTO_NEXT;
        out->value = 0;
        return true;
    }

    if (strcmp(cmd, "demo_speed") == 0) {
        /*
         * 演示速度（App 的"模拟速度测试"滑块）：
         *   {"cmd":"demo_speed","value":45}    → 演示 45 km/h
         *   {"cmd":"demo_speed","value":-1}    → 退出演示
         *
         * 不做上限校验：真实限幅交给 ui_speed（仪表盘自己的量程），
         * 解析层只管语法 —— 与 face_enabled 的分工一致（业务规则不在这里）。
         */
        if (!have_num) return false;
        out->kind = CONTROL_DEMO_SPEED;
        out->value = num;
        return true;
    }

    (void)have_str;
    return false; /* 未知命令：kind 保持 CONTROL_NONE */
}

uint8_t control_clamp_brightness(int32_t v)
{
    if (v < 0) return 1;
    if (v == 0) return 1; /* 0 会让屏全黑，视为「最低可见」而非关闭 */
    if (v > 255) return 255;
    return (uint8_t)v;
}
