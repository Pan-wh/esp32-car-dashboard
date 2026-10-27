/**
 * test_photo_cmd.c —— `PhotoCtl` 命令解析的 **纯逻辑** 单测（不需要硬件）
 *
 * 覆盖重点：
 *   1. 每个 op 的正常路径 + 协议示例逐字节（字段语义与头文件一致）；
 *   2. 必填/范围校验的**每一条拒绝路径**（缺字段 / 类型错 / 越界 / 未知 op）；
 *   3. 坏 JSON（截断、缺分隔符、未闭合字符串、尾随垃圾、非对象、`\u` 转义）；
 *   4. 未知字段忽略（含嵌套对象/数组/布尔/null）——保证固件比 App 老时不被拒；
 *   5. 长度参数语义（len=0 用 strlen；带长度时容忍 NUL/空白填充）；
 *   6. 所有协议命令 JSON 都 < 180 B（iOS 单次 Notify 上限）。
 */
#include "../simulator/ui/photo_cmd.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, msg);                \
        }                                                                      \
    } while (0)

/** 解析并断言成功，返回命令结构（便于逐字段检查） */
static bool parse_ok(const char *json, photo_cmd_t *out)
{
    bool ok = photo_cmd_parse(json, 0, out);
    if (!ok) return false;
    /* 每条成功解析的命令同时满足 ≤180 B（协议硬约束） */
    CHECK(strlen(json) < 180, "命令 JSON 必须 < 180 B");
    return true;
}

#define PARSE_BAD(json, msg)                                                   \
    do {                                                                       \
        photo_cmd_t c;                                                         \
        CHECK(!photo_cmd_parse((json), 0, &c), msg);                           \
    } while (0)

/* ── 1. begin ──────────────────────────────────────────────── */

static void test_begin(void)
{
    photo_cmd_t c;

    /* 协议 §4 示例（静态） */
    const char *begin_pgj =
        "{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":45210,"
        "\"crc\":282593821,\"w\":466,\"h\":466,\"frames\":1,\"fps\":0}";
    CHECK(parse_ok(begin_pgj, &c), "协议示例 begin 应解析成功");
    CHECK(c.op == PHOTO_OP_BEGIN, "op = PHOTO_OP_BEGIN");
    CHECK(c.kind == 0, "kind = 0");
    CHECK(c.bytes == 45210, "bytes = 45210");
    CHECK(c.crc == 282593821u, "crc = 282593821");
    CHECK(c.width == 466 && c.height == 466, "w/h = 466");
    CHECK(c.frames == 1, "frames = 1");
    CHECK(c.fps == 0, "fps = 0");

    /* 缺 crc / fps 的静态 begin：头文件必填列表不含它们 → 应接受，crc 默认 0 */
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1000,"
                   "\"w\":16,\"h\":16,\"frames\":1}", &c),
          "静态 begin 缺 crc/fps 应接受");
    CHECK(c.crc == 0, "缺省 crc = 0");
    CHECK(c.fps == 0, "缺省 fps = 0");

    /* 动图：kind=1 必须 fps>=1 */
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"begin\",\"kind\":1,\"bytes\":1114112,"
                   "\"crc\":4294967295,\"w\":300,\"h\":300,\"frames\":12,\"fps\":8}", &c),
          "动图 begin 应接受");
    CHECK(c.kind == 1 && c.bytes == 1114112u && c.crc == 4294967295u,
          "动图 begin 字段正确");
    CHECK(c.frames == 12 && c.fps == 8, "动图 frames/fps 正确");

    /* 边界值 */
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":4294967295,"
                   "\"w\":1,\"h\":1,\"frames\":1}", &c),
          "bytes=u32 上限应接受（上限策略由运行时决定）");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,"
                   "\"w\":466,\"h\":466,\"frames\":65535}", &c),
          "frames=65535 边界应接受");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"begin\",\"kind\":1,\"bytes\":1,"
                   "\"w\":1,\"h\":1,\"frames\":1,\"fps\":255}", &c),
          "fps=255 边界应接受");

    /* 未知字段忽略（含嵌套） */
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,"
                   "\"w\":1,\"h\":1,\"frames\":1,"
                   "\"future\":{\"a\":[1,2,{\"b\":null}],\"s\":\"x\",\"t\":true},"
                   "\"extra\":false}", &c),
          "未知嵌套字段应被忽略");

    /* 字段顺序无关 / 重复 key 后者覆盖 */
    CHECK(parse_ok("{\"op\":\"begin\",\"kind\":0,\"bytes\":7,\"w\":2,\"h\":2,"
                   "\"frames\":1,\"t\":\"p\"}", &c),
          "字段顺序无关");
    CHECK(c.bytes == 7 && c.width == 2, "乱序字段值正确");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":5,\"bytes\":6,"
                   "\"w\":1,\"h\":1,\"frames\":1}", &c),
          "重复 key 应接受（后者覆盖）");
    CHECK(c.bytes == 6, "重复 key 取最后一个值");

    /* ── 拒绝路径：缺必填 ── */
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"bytes\":1,\"w\":1,\"h\":1,\"frames\":1}",
              "begin 缺 kind 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"w\":1,\"h\":1,\"frames\":1}",
              "begin 缺 bytes 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"h\":1,\"frames\":1}",
              "begin 缺 w 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":1,\"frames\":1}",
              "begin 缺 h 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":1,\"h\":1}",
              "begin 缺 frames 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":1,\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":3}",
              "动图 begin 缺 fps 应拒绝");

    /* ── 拒绝路径：范围/类型 ── */
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":2,\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":1}", "kind=2 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":-1,\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":1}", "kind=-1 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":\"0\",\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":1}", "kind 为字符串应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":0,\"w\":1,\"h\":1,"
              "\"frames\":1}", "bytes=0 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":-1,\"w\":1,\"h\":1,"
              "\"frames\":1}", "bytes=-1 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":4294967296,"
              "\"w\":1,\"h\":1,\"frames\":1}", "bytes 超过 u32 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":0,\"h\":1,"
              "\"frames\":1}", "w=0 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":467,\"h\":1,"
              "\"frames\":1}", "w>466 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":1,\"h\":0,"
              "\"frames\":1}", "h=0 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":1,\"h\":467,"
              "\"frames\":1}", "h>466 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":0}", "frames=0 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":65536}", "frames>65535 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":1,\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":3,\"fps\":0}", "动图 fps=0 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":1,\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":3,\"fps\":256}", "fps>255 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":1,\"crc\":-1}", "crc=-1 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1,\"w\":1,\"h\":1,"
              "\"frames\":1,\"crc\":4294967296}", "crc 超过 u32 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1.5,\"w\":1,\"h\":1,"
              "\"frames\":1}", "bytes 是小数应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":1e3,\"w\":1,\"h\":1,"
              "\"frames\":1}", "bytes 是指数应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":null,\"w\":1,\"h\":1,"
              "\"frames\":1}", "bytes=null 应拒绝");
}

/* ── 2. 无参 op ────────────────────────────────────────────── */

static void test_noarg_ops(void)
{
    photo_cmd_t c;
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"end\"}", &c), "end 应解析成功");
    CHECK(c.op == PHOTO_OP_END, "op = PHOTO_OP_END");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"abort\"}", &c), "abort 应解析成功");
    CHECK(c.op == PHOTO_OP_ABORT, "op = PHOTO_OP_ABORT");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"list\"}", &c), "list 应解析成功");
    CHECK(c.op == PHOTO_OP_LIST, "op = PHOTO_OP_LIST");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"bench\"}", &c), "bench 应解析成功");
    CHECK(c.op == PHOTO_OP_BENCH, "op = PHOTO_OP_BENCH");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"format\"}", &c), "format 应解析成功");
    CHECK(c.op == PHOTO_OP_FORMAT, "op = PHOTO_OP_FORMAT");

    /* 无参 op 上的未知字段忽略 */
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"end\",\"x\":[1,2,3],\"y\":{\"z\":true}}", &c),
          "无参 op 的未知字段应被忽略");

    PARSE_BAD("{\"op\":\"end\"}", "缺 t 应拒绝");
    PARSE_BAD("{\"t\":\"q\",\"op\":\"end\"}", "t != \"p\" 应拒绝");
    PARSE_BAD("{\"t\":\"p\"}", "缺 op 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"\"}", "空 op 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"unknown\"}", "未知 op 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"BEGIN\"}", "op 大小写敏感");
    PARSE_BAD("{\"t\":\"p\",\"op\":1}", "op 不是字符串应拒绝");
    PARSE_BAD("{\"t\":1,\"op\":\"list\"}", "t 不是字符串应拒绝");
}

/* ── 3. del ────────────────────────────────────────────────── */

static void test_del(void)
{
    photo_cmd_t c;
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"del\",\"id\":\"p00012.pgj\"}", &c), "del 应成功");
    CHECK(c.op == PHOTO_OP_DEL, "op = PHOTO_OP_DEL");
    CHECK(strcmp(c.id, "p00012.pgj") == 0, "id 正确");

    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"del\",\"id\":\"A.PGI\"}", &c),
          "大写扩展名应接受");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"del\",\"id\":\"old-scan_1.pgi\","
                   "\"unknown\":1}", &c),
          "del 带未知字段应接受");

    PARSE_BAD("{\"t\":\"p\",\"op\":\"del\"}", "缺 id 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"del\",\"id\":\"\"}", "空 id 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"del\",\"id\":\"../evil.pgj\"}", "路径穿越应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"del\",\"id\":\"..\"}", "\"..\" 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"del\",\"id\":\"a/b.pgj\"}", "含 '/' 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"del\",\"id\":\"a.txt\"}", "非法扩展名应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"del\",\"id\":\".pgj\"}", "以 '.' 开头应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"del\",\"id\":5}", "id 不是字符串应拒绝");

    /* 64 字符 id 超过白名单上限 → 拒绝（截断后可能变成合法名，必须用 too_long 挡住） */
    char long_id[128];
    memset(long_id, 0, sizeof(long_id));
    memcpy(long_id, "{\"t\":\"p\",\"op\":\"del\",\"id\":\"", 25);
    size_t at = 25;
    memset(long_id + at, 'a', 64);
    at += 64;
    memcpy(long_id + at, ".pgj\"}", 6);
    long_id[at + 6] = '\0';
    photo_cmd_t bad;
    CHECK(!photo_cmd_parse(long_id, 0, &bad), "64 字符 id 应拒绝");
}

/* ── 4. play ───────────────────────────────────────────────── */

static void test_play(void)
{
    photo_cmd_t c;
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"play\",\"i\":3}", &c), "play 应成功");
    CHECK(c.op == PHOTO_OP_PLAY, "op = PHOTO_OP_PLAY");
    CHECK(c.index == 3, "index = 3");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"play\",\"i\":0}", &c), "i=0 应成功");
    CHECK(c.index == 0, "index = 0");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"play\",\"i\":2147483647}", &c),
          "i=INT_MAX 应成功（越界由运行时夹取）");
    CHECK(c.index == INT_MAX, "index = INT_MAX");

    PARSE_BAD("{\"t\":\"p\",\"op\":\"play\"}", "缺 i 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"play\",\"i\":-1}", "i=-1 应拒绝（-1 由调用方内部传）");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"play\",\"i\":2147483648}", "i 超过 int 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"play\",\"i\":\"3\"}", "i 为字符串应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"play\",\"i\":null}", "i=null 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"play\",\"i\":3.0}", "i 为小数应拒绝");
}

/* ── 5. set（任意子集）─────────────────────────────────────── */

static void test_set(void)
{
    photo_cmd_t c;

    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"set\",\"slide\":1,\"intv\":30,\"shift\":1,"
                   "\"soff_s\":1800}", &c),
          "set 全字段（协议示例）应成功");
    CHECK(c.op == PHOTO_OP_SET, "op = PHOTO_OP_SET");
    CHECK(c.has_slide && c.slide == 1, "slide = 1");
    CHECK(c.has_intv && c.intv == 30, "intv = 30");
    CHECK(c.has_shift && c.shift == 1, "shift = 1");
    CHECK(c.has_soff && c.soff_s == 1800, "soff_s = 1800");

    /* 单字段子集：只置对应 has_* */
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"set\",\"slide\":0}", &c), "只带 slide 应成功");
    CHECK(c.has_slide && c.slide == 0, "slide = 0");
    CHECK(!c.has_intv && !c.has_shift && !c.has_soff, "未下发的字段 has_* = false");

    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"set\",\"intv\":1}", &c), "intv=1 边界应成功");
    CHECK(c.has_intv && c.intv == 1, "intv = 1");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"set\",\"intv\":3600}", &c),
          "intv=3600 边界应成功");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"set\",\"shift\":0}", &c), "shift=0 应成功");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"set\",\"soff_s\":0}", &c),
          "soff_s=0（关闭）应成功");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"set\",\"soff_s\":86400}", &c),
          "soff_s=86400 边界应成功");
    CHECK(parse_ok("{\"t\":\"p\",\"op\":\"set\",\"slide\":1,\"future\":{\"a\":1}}", &c),
          "set 带未知字段应成功");

    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\"}", "空 set 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"future\":1}", "只有未知字段的 set 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"slide\":2}", "slide=2 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"slide\":-1}", "slide=-1 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"shift\":2}", "shift=2 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"intv\":0}", "intv=0 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"intv\":3601}", "intv=3601 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"intv\":-5}", "intv=-5 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"soff_s\":-1}", "soff_s=-1 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"soff_s\":86401}", "soff_s=86401 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"set\",\"slide\":\"1\"}", "slide 为字符串应拒绝");
}

/* ── 6. 坏 JSON / 长度语义 ─────────────────────────────────── */

static void test_bad_json(void)
{
    photo_cmd_t c;

    CHECK(!photo_cmd_parse(NULL, 0, &c), "json=NULL 应拒绝");
    CHECK(!photo_cmd_parse("{\"t\":\"p\",\"op\":\"list\"}", 0, NULL), "out=NULL 应拒绝");

    PARSE_BAD("", "空串应拒绝");
    PARSE_BAD("   ", "只有空白应拒绝");
    PARSE_BAD("{}", "空对象应拒绝");
    PARSE_BAD("[]", "数组不是对象应拒绝");
    PARSE_BAD("null", "null 不是对象应拒绝");
    PARSE_BAD("\"list\"", "字符串不是对象应拒绝");
    PARSE_BAD("{", "只有 { 应拒绝");
    PARSE_BAD("{\"t\":\"p\"", "缺 } 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\"", "缺 } 应拒绝");
    PARSE_BAD("{\"t\" \"p\",\"op\":\"list\"}", "缺 ':' 应拒绝");
    PARSE_BAD("{\"t\":\"p\" \"op\":\"list\"}", "缺 ',' 应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\",}", "尾随逗号应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\"} garbage", "尾随垃圾应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\"}}", "多余 } 应拒绝");
    PARSE_BAD("{t:\"p\"}", "key 未加引号应拒绝");
    PARSE_BAD("{\"t\":\"p}", "字符串未闭合应拒绝");
    PARSE_BAD("{\"t\":\"p\\x\",\"op\":\"list\"}", "非法转义应拒绝");
    PARSE_BAD("{\"t\":\"\\u0070\",\"op\":\"list\"}", "\\u 转义应拒绝（协议只用 ASCII）");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\",\"future\":[1,2}", "数组未闭合应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\",\"future\":{\"a\":1}", "对象未闭合应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\",\"future\":}", "缺值应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\",\"future\":tru}", "坏字面量应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\",\"i\":--1}", "双负号应拒绝");
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\",\"i\":+1}", "前导 + 应拒绝");

    /* 长度参数：len=0 → strlen；给定 len 时拒绝截断；容忍尾随 NUL/空白 */
    const char *good = "{\"t\":\"p\",\"op\":\"list\"}";
    CHECK(photo_cmd_parse(good, 0, &c) && c.op == PHOTO_OP_LIST, "len=0 用 strlen");
    CHECK(photo_cmd_parse(good, strlen(good), &c), "len=strlen 应成功");
    CHECK(!photo_cmd_parse(good, strlen(good) - 1, &c), "len 少 1（截断）应拒绝");

    char padded[64];
    memset(padded, 0, sizeof(padded));
    memcpy(padded, good, strlen(good));
    CHECK(photo_cmd_parse(padded, sizeof(padded), &c),
          "定长缓冲 + NUL 填充应成功（BLE 传长固定）");

    char spaced[64];
    memset(spaced, ' ', sizeof(spaced));
    memcpy(spaced, good, strlen(good));
    spaced[sizeof(spaced) - 1] = '\0';
    CHECK(photo_cmd_parse(spaced, sizeof(spaced) - 1, &c), "尾随空白应成功");

    /* 首尾空白 */
    CHECK(photo_cmd_parse("  \t{\"t\":\"p\",\"op\":\"list\"} \r\n", 0, &c),
          "首尾空白应成功");
    CHECK(c.op == PHOTO_OP_LIST, "首尾空白解析结果正确");

    /* 超大未知值嵌套深度限制（>8 层应拒绝，避免病态输入） */
    PARSE_BAD("{\"t\":\"p\",\"op\":\"list\",\"x\":[[[[[[[[[[1]]]]]]]]]]}", "过深嵌套应拒绝");
    /* 8 层以内可以 */
    CHECK(photo_cmd_parse("{\"t\":\"p\",\"op\":\"list\",\"x\":[[[[[[[[1]]]]]]]]}", 0, &c),
          "8 层嵌套内的未知字段应接受");
}

/* ── 7. 结构体默认值 ──────────────────────────────────────── */

static void test_defaults(void)
{
    photo_cmd_t c;
    memset(&c, 0xAA, sizeof(c));
    CHECK(photo_cmd_parse("{\"t\":\"p\",\"op\":\"list\"}", 0, &c), "list 解析成功");
    CHECK(c.index == -1, "非 play 命令 index 默认为 -1（= 无指定）");
    CHECK(c.kind == 0 && c.bytes == 0 && c.crc == 0, "未使用字段清零");
    CHECK(c.width == 0 && c.height == 0 && c.frames == 0 && c.fps == 0, "begin 字段清零");
    CHECK(c.id[0] == '\0', "id 为空串");
    CHECK(!c.has_slide && !c.has_intv && !c.has_shift && !c.has_soff, "set 的 has_* 为 false");
}

/* ── 8. 确定性变异：任意单字节损坏都不得崩溃 ──────────────────
 *
 * BLE 写可能被截断/重传，解析器必须"宁可 false 也不越界"。这里对几条典型命令做
 * 单字节替换 + 所有前缀截断，只要求安全返回；不要求具体 true/false。
 */
static void test_mutation_robustness(void)
{
    const char *seeds[] = {
        "{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":45210,"
        "\"crc\":282593821,\"w\":466,\"h\":466,\"frames\":1,\"fps\":0}",
        "{\"t\":\"p\",\"op\":\"set\",\"slide\":1,\"intv\":30,\"shift\":1,"
        "\"soff_s\":1800}",
        "{\"t\":\"p\",\"op\":\"del\",\"id\":\"p00012.pgj\"}",
        "{\"t\":\"p\",\"op\":\"play\",\"i\":3}",
    };
    static const char repl[] = { '{', '}', '[', ']', ':', '"', '\\', '0', '9', '-', 'e', 'x',
                                 ' ', '\0' };

    int calls = 0;
    for (size_t si = 0; si < sizeof(seeds) / sizeof(seeds[0]); si++) {
        const char *seed = seeds[si];
        const size_t n = strlen(seed);
        char *buf = (char *)malloc(n + 1);
        if (buf == NULL) continue;
        memcpy(buf, seed, n + 1);

        /* 单字节替换（含替换成 NUL：模拟定长缓冲里的提前终止） */
        for (size_t pos = 0; pos < n; pos++) {
            char save = buf[pos];
            for (size_t ri = 0; ri < sizeof(repl); ri++) {
                memcpy(buf, seed, n + 1);
                buf[pos] = repl[ri];
                photo_cmd_t c;
                (void)photo_cmd_parse(buf, n, &c);
                (void)photo_cmd_parse(buf, 0, &c);
                calls += 2;
            }
            buf[pos] = save;
        }

        /* 所有前缀长度（截断） */
        for (size_t cut = 0; cut <= n; cut++) {
            photo_cmd_t c;
            (void)photo_cmd_parse(seed, cut, &c);
            calls++;
        }
        free(buf);
    }
    CHECK(calls > 5000, "变异/截断输入应全部安全返回");
}

int main(void)
{
    printf("=== test_photo_cmd（PhotoCtl JSON：op/必填/范围/坏 JSON/未知字段）===\n");

    test_begin();
    test_noarg_ops();
    test_del();
    test_play();
    test_set();
    test_bad_json();
    test_defaults();
    test_mutation_robustness();

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
