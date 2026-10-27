/**
 * test_photo_list.c —— 设备端照片清单的 **纯逻辑** 单测（不需要硬件）
 *
 * 覆盖重点：
 *   1. 文件名白名单的每一条（长度 1..63 / 字符集 / 不以 '.' 开头 / 无 ".." /
 *      扩展名 .pgj/.pgi 大小写不敏感）；
 *   2. 增删查 + 容量上限（128）+ 同名/非法字段拒绝；
 *   3. 按文件名升序排序（幂等）—— 排序 == 播放顺序是协议依赖的行为；
 *   4. 清单分页 JSON（`pq` / `pf` / `pz`）逐条断言 **strlen < 180**，
 *      包括"63 字符文件名 + 所有字段取最大值"的最坏情况；
 *   5. 生成 JSON 的容量边界（刚好不够 → -1，刚好够 → 成功）。
 */
#include "../simulator/ui/photo_list.h"

#include <stdio.h>
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

/** 180 B 是 iOS 单次 Notify 的硬上限（协议 §0），所有清单消息都必须满足 */
#define JSON_LIMIT 180

static photo_item_t make_item(const char *name, photo_kind_t kind, uint32_t bytes,
                              uint16_t frames, uint8_t fps, uint16_t w, uint16_t h)
{
    photo_item_t it;
    memset(&it, 0, sizeof(it));
    snprintf(it.name, sizeof(it.name), "%s", name);
    it.kind = (uint8_t)kind;
    it.bytes = bytes;
    it.frames = frames;
    it.fps = fps;
    it.width = w;
    it.height = h;
    return it;
}

/* ── 1. 文件名白名单 ───────────────────────────────────────── */

static void test_name_valid(void)
{
    CHECK(photo_name_valid("p00012.pgj"), "标准设备文件名应合法");
    CHECK(photo_name_valid("p00012.pgi"), ".pgi 应合法");
    CHECK(photo_name_valid("A.PGJ"), "大写扩展名应合法");
    CHECK(photo_name_valid("a.PgI"), "混合大小写扩展名应合法");
    CHECK(photo_name_valid("photo-1_2.pgj"), "数字/下划线/连字符应合法");
    CHECK(photo_name_valid("-a.pgj"), "以 '-' 开头应合法（不以 '.' 开头即可）");
    CHECK(photo_name_valid("_a.pgi"), "以 '_' 开头应合法");
    CHECK(photo_name_valid("a.b.pgj"), "名字里可以出现多个 '.'");
    CHECK(photo_name_valid("0.pgj"), "单字符主名应合法");

    /* 63 字符 = 上限；64 字符 = 超长 */
    char ok_name[PHOTO_NAME_MAX];
    memset(ok_name, 'a', sizeof(ok_name));
    memcpy(ok_name + 59, ".pgj", 4); /* 59 + 4 = 63 */
    ok_name[63] = '\0';
    CHECK(strlen(ok_name) == 63, "构造的边界名应为 63 字符");
    CHECK(photo_name_valid(ok_name), "63 字符文件名应合法");

    char too_long[PHOTO_NAME_MAX + 1];
    memset(too_long, 'a', sizeof(too_long));
    memcpy(too_long + 60, ".pgj", 4); /* 60 + 4 = 64 */
    too_long[64] = '\0';
    CHECK(strlen(too_long) == 64, "构造的超长名应为 64 字符");
    CHECK(!photo_name_valid(too_long), "64 字符文件名应拒绝");

    CHECK(!photo_name_valid(NULL), "NULL 应拒绝");
    CHECK(!photo_name_valid(""), "空名应拒绝");
    CHECK(!photo_name_valid("."), "单个 '.' 应拒绝");
    CHECK(!photo_name_valid(".."), "\"..\" 应拒绝");
    CHECK(!photo_name_valid(".pgj"), "以 '.' 开头应拒绝");
    CHECK(!photo_name_valid(".hidden.pgj"), "隐藏文件应拒绝");
    CHECK(!photo_name_valid("a..pgj"), "含 \"..\" 应拒绝");
    CHECK(!photo_name_valid("a..b.pgj"), "中间含 \"..\" 应拒绝");
    CHECK(!photo_name_valid("../a.pgj"), "路径穿越名应拒绝");
    CHECK(!photo_name_valid("a/b.pgj"), "含 '/' 应拒绝");
    CHECK(!photo_name_valid("a\\b.pgj"), "含 '\\' 应拒绝");
    CHECK(!photo_name_valid("a b.pgj"), "含空格应拒绝");
    CHECK(!photo_name_valid("a.b.pgjx"), "扩展名不是 pgj/pgi 应拒绝");
    CHECK(!photo_name_valid("a.jpg"), ".jpg 应拒绝");
    CHECK(!photo_name_valid("a.pg"), "扩展名不足 3 字符应拒绝");
    CHECK(!photo_name_valid("a.pgjj"), "扩展名超过 3 字符应拒绝");
    CHECK(!photo_name_valid("a.pgj."), "结尾多一个 '.' 应拒绝");
    CHECK(!photo_name_valid("a.pgj "), "结尾空格应拒绝");
    CHECK(!photo_name_valid("a.pgj\n"), "尾部控制字符应拒绝");
    CHECK(!photo_name_valid("noext"), "无扩展名应拒绝");
    CHECK(!photo_name_valid("noext.pgj/"), "尾部斜杠应拒绝");
    CHECK(!photo_name_valid("照片.pgj"), "非 ASCII 应拒绝");
}

/* ── 2. 增删查 / 容量 / 字段校验 ───────────────────────────── */

static void test_add_find_remove(void)
{
    photo_list_t l;
    photo_list_init(&l);
    CHECK(l.count == 0, "init 后 count = 0");

    photo_item_t a = make_item("p00002.pgj", PHOTO_KIND_PGJ, 45210, 1, 0, 466, 466);
    CHECK(photo_list_add(&l, &a), "合法项应能加入");
    CHECK(l.count == 1, "count 应 +1");
    CHECK(photo_list_find(&l, "p00002.pgj") == 0, "应能按名字找到");
    CHECK(photo_list_find(&l, "nope.pgj") == -1, "找不到应返回 -1");
    CHECK(photo_list_find(NULL, "p00002.pgj") == -1, "NULL 清单返回 -1");
    CHECK(photo_list_find(&l, NULL) == -1, "NULL 名字返回 -1");

    photo_item_t b = make_item("p00001.pgi", PHOTO_KIND_PGI, 1024, 3, 10, 300, 300);
    CHECK(photo_list_add(&l, &b), "动图项应能加入");
    CHECK(photo_list_find(&l, "p00001.pgi") == 1, "增补项下标 = 1");

    /* 非法输入 */
    CHECK(!photo_list_add(NULL, &a), "NULL 清单应拒绝");
    CHECK(!photo_list_add(&l, NULL), "NULL 项应拒绝");
    CHECK(!photo_list_add(&l, &a), "同名重复应拒绝");
    photo_item_t bad = a;
    snprintf(bad.name, sizeof(bad.name), "%s", "bad.jpg");
    CHECK(!photo_list_add(&l, &bad), "非法文件名应拒绝");
    bad = a;
    snprintf(bad.name, sizeof(bad.name), "%s", "p00003.pgj");
    bad.kind = 2;
    CHECK(!photo_list_add(&l, &bad), "kind > 1 应拒绝");
    bad.kind = PHOTO_KIND_PGJ;
    bad.frames = 0;
    CHECK(!photo_list_add(&l, &bad), "frames = 0 应拒绝");
    bad.frames = 1;
    bad.width = 0;
    CHECK(!photo_list_add(&l, &bad), "width = 0 应拒绝");
    bad = a;
    snprintf(bad.name, sizeof(bad.name), "%s", "p00003.pgj");
    bad.width = 467;
    CHECK(!photo_list_add(&l, &bad), "width > 466 应拒绝");
    bad.width = 466;
    bad.height = 467;
    CHECK(!photo_list_add(&l, &bad), "height > 466 应拒绝");
    bad.height = 466;
    bad.kind = PHOTO_KIND_PGJ;
    bad.frames = 2;
    CHECK(!photo_list_add(&l, &bad), ".pgj 声明多帧应拒绝");
    bad = make_item("p00003.pgi", PHOTO_KIND_PGI, 1024, 1, 0, 466, 466);
    CHECK(photo_list_add(&l, &bad), ".pgi 单帧（静态）应允许");

    /* 删除：中间/首个/末尾/不存在 */
    CHECK(photo_list_remove(&l, "p00002.pgj"), "删除中间项应成功");
    CHECK(l.count == 2, "count 应 -1");
    CHECK(photo_list_find(&l, "p00002.pgj") == -1, "删掉的项不应再找到");
    CHECK(strcmp(l.items[0].name, "p00001.pgi") == 0, "删除后其余顺序不变");
    CHECK(photo_list_remove(&l, "p00001.pgi"), "删除首项应成功");
    CHECK(strcmp(l.items[0].name, "p00003.pgi") == 0, "首项删除后后续前移");
    CHECK(photo_list_remove(&l, "p00003.pgi"), "删除末项应成功");
    CHECK(l.count == 0, "清空后 count = 0");
    CHECK(!photo_list_remove(&l, "p00003.pgi"), "删除不存在项应返回 false");
    CHECK(!photo_list_remove(NULL, "x.pgj"), "NULL 清单删除应返回 false");
}

static void test_capacity(void)
{
    photo_list_t l;
    photo_list_init(&l);

    char name[PHOTO_NAME_MAX];
    for (int i = 0; i < PHOTO_MAX_FILES; i++) {
        photo_make_name(name, sizeof(name), (uint32_t)i, PHOTO_KIND_PGJ);
        photo_item_t it = make_item(name, PHOTO_KIND_PGJ, 1000 + (uint32_t)i, 1, 0, 16, 16);
        CHECK(photo_list_add(&l, &it), "128 容量内应能加入");
    }
    CHECK(l.count == PHOTO_MAX_FILES, "清单应装满 128 项");

    photo_make_name(name, sizeof(name), 999u, PHOTO_KIND_PGJ);
    photo_item_t extra = make_item(name, PHOTO_KIND_PGJ, 1000, 1, 0, 16, 16);
    CHECK(!photo_list_add(&l, &extra), "第 129 项应拒绝");

    CHECK(photo_list_find(&l, "p00000.pgj") == 0, "首项下标 0");
    CHECK(photo_list_find(&l, "p00127.pgj") == 127, "末项下标 127");
}

/* ── 3. 排序 ───────────────────────────────────────────────── */

static void test_sort(void)
{
    photo_list_t l;
    photo_list_init(&l);

    const char *names[] = { "p00010.pgj", "B.pgj", "p00002.pgi", "a.pgj", "p00001.pgj" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        photo_item_t it = make_item(names[i], PHOTO_KIND_PGJ, 10, 1, 0, 16, 16);
        CHECK(photo_list_add(&l, &it), "排序夹具项应能加入");
    }

    photo_list_sort(&l);
    for (int i = 1; i < l.count; i++) {
        CHECK(strcmp(l.items[i - 1].name, l.items[i].name) < 0,
              "排序后应严格按文件名升序");
    }
    CHECK(strcmp(l.items[0].name, "B.pgj") == 0, "大写字母排在小写之前（strcmp 语义）");
    CHECK(strcmp(l.items[l.count - 1].name, "p00010.pgj") == 0, "p00010 在末位");

    /* 幂等：再排一次结果不变 */
    char before[PHOTO_MAX_FILES][PHOTO_NAME_MAX];
    for (int i = 0; i < l.count; i++) {
        snprintf(before[i], sizeof(before[i]), "%s", l.items[i].name);
    }
    photo_list_sort(&l);
    for (int i = 0; i < l.count; i++) {
        CHECK(strcmp(before[i], l.items[i].name) == 0, "重复排序应幂等");
    }

    /* 空清单 / NULL 不应崩 */
    photo_list_t empty;
    photo_list_init(&empty);
    photo_list_sort(&empty);
    photo_list_sort(NULL);
    CHECK(empty.count == 0, "空清单排序后仍为空");
}

/* ── 4. 文件名生成 ─────────────────────────────────────────── */

static void test_make_name(void)
{
    char name[PHOTO_NAME_MAX];
    CHECK(photo_make_name(name, sizeof(name), 0, PHOTO_KIND_PGJ), "seq 0 应成功");
    CHECK(strcmp(name, "p00000.pgj") == 0, "seq 0 → p00000.pgj");
    CHECK(photo_make_name(name, sizeof(name), 12, PHOTO_KIND_PGJ), "seq 12 应成功");
    CHECK(strcmp(name, "p00012.pgj") == 0, "seq 12 → p00012.pgj（与协议示例一致）");
    CHECK(photo_make_name(name, sizeof(name), 12, PHOTO_KIND_PGI), "kind 1 应成功");
    CHECK(strcmp(name, "p00012.pgi") == 0, "kind 1 → .pgi");
    CHECK(photo_make_name(name, sizeof(name), 99999, PHOTO_KIND_PGJ), "5 位上限应成功");
    CHECK(strcmp(name, "p99999.pgj") == 0, "99999 → p99999.pgj");
    CHECK(photo_make_name(name, sizeof(name), 100000, PHOTO_KIND_PGJ), "6 位序号应成功");
    CHECK(strcmp(name, "p100000.pgj") == 0, "序号自然变长");
    CHECK(photo_name_valid(name), "生成的名字必须通过白名单");

    CHECK(!photo_make_name(NULL, PHOTO_NAME_MAX, 1, PHOTO_KIND_PGJ), "out=NULL 应拒绝");
    CHECK(!photo_make_name(name, PHOTO_NAME_MAX - 1, 1, PHOTO_KIND_PGJ),
          "cap < PHOTO_NAME_MAX 应拒绝");
    CHECK(!photo_make_name(name, sizeof(name), 1, (photo_kind_t)2), "非法 kind 应拒绝");
}

/* ── 5. 清单分页 JSON ──────────────────────────────────────── */

static void test_json(void)
{
    char buf[JSON_LIMIT + 64];

    /* pq */
    CHECK(photo_list_begin_json(buf, sizeof(buf), 0) > 0, "pq 应成功");
    CHECK(strcmp(buf, "{\"t\":\"pq\",\"n\":0}") == 0, "pq(0) 内容正确");
    CHECK(photo_list_begin_json(buf, sizeof(buf), 12) > 0, "pq 应成功");
    CHECK(strcmp(buf, "{\"t\":\"pq\",\"n\":12}") == 0, "pq(12) 与协议示例一致");
    CHECK(photo_list_begin_json(buf, sizeof(buf), 128) > 0, "pq(128) 应成功");
    CHECK(strlen(buf) < JSON_LIMIT, "pq 不应超过 180 B");
    CHECK(photo_list_begin_json(NULL, sizeof(buf), 1) == -1, "pq out=NULL 应 -1");
    CHECK(photo_list_begin_json(buf, 0, 1) == -1, "pq cap=0 应 -1");
    CHECK(photo_list_begin_json(buf, sizeof(buf), -1) == -1, "pq count<0 应 -1");

    /* 容量边界：需要 n+1 字节（含结尾 0） */
    int need = photo_list_begin_json(buf, sizeof(buf), 12);
    CHECK(need > 0, "先拿到 pq 长度");
    char tight[JSON_LIMIT];
    CHECK(photo_list_begin_json(tight, (size_t)need, 12) == -1, "cap = strlen 时应 -1");
    CHECK(photo_list_begin_json(tight, (size_t)need + 1, 12) == need,
          "cap = strlen+1 时应成功");

    /* pz */
    CHECK(photo_list_end_json(buf, sizeof(buf)) > 0, "pz 应成功");
    CHECK(strcmp(buf, "{\"t\":\"pz\"}") == 0, "pz 内容正确");
    CHECK(strlen(buf) < JSON_LIMIT, "pz 不应超过 180 B");
    CHECK(photo_list_end_json(NULL, sizeof(buf)) == -1, "pz out=NULL 应 -1");
    CHECK(photo_list_end_json(buf, 0) == -1, "pz cap=0 应 -1");

    /* pf：与协议 §5 示例逐字节一致 */
    photo_list_t l;
    photo_list_init(&l);
    photo_item_t it = make_item("p00012.pgj", PHOTO_KIND_PGJ, 45210, 1, 0, 466, 466);
    CHECK(photo_list_add(&l, &it), "pf 夹具项应能加入");
    int n = photo_list_item_json(&l, 0, buf, sizeof(buf));
    CHECK(n > 0, "pf 应成功");
    CHECK(strcmp(buf, "{\"t\":\"pf\",\"i\":0,\"id\":\"p00012.pgj\",\"b\":45210,"
                      "\"f\":1,\"k\":0,\"w\":466,\"h\":466,\"fps\":0}") == 0,
          "pf 内容应与协议示例逐字节一致");
    CHECK((size_t)n == strlen(buf), "返回值应等于 strlen");

    /* 动图项：f/k/fps 字段正确 */
    const char *anim =
        "{\"t\":\"pf\",\"i\":1,\"id\":\"p00003.pgi\",\"b\":1024,\"f\":3,\"k\":1,"
        "\"w\":300,\"h\":300,\"fps\":10}";
    photo_item_t it2 = make_item("p00003.pgi", PHOTO_KIND_PGI, 1024, 3, 10, 300, 300);
    CHECK(photo_list_add(&l, &it2), "动图项应能加入");
    CHECK(photo_list_item_json(&l, 1, buf, sizeof(buf)) > 0, "动图 pf 应成功");
    CHECK(strcmp(buf, anim) == 0, "动图 pf 内容正确");
    CHECK(strlen(buf) < JSON_LIMIT, "动图 pf 不应超过 180 B");

    /* 越界 / 坏参数 */
    CHECK(photo_list_item_json(&l, -1, buf, sizeof(buf)) == -1, "负下标应 -1");
    CHECK(photo_list_item_json(&l, 2, buf, sizeof(buf)) == -1, "越界下标应 -1");
    CHECK(photo_list_item_json(NULL, 0, buf, sizeof(buf)) == -1, "NULL 清单应 -1");
    CHECK(photo_list_item_json(&l, 0, NULL, sizeof(buf)) == -1, "out=NULL 应 -1");
    CHECK(photo_list_item_json(&l, 0, buf, 0) == -1, "cap=0 应 -1");

    /* 容量边界 */
    need = photo_list_item_json(&l, 0, buf, sizeof(buf));
    CHECK(need > 0, "先拿到 pf 长度");
    char tight2[JSON_LIMIT + 64];
    CHECK(photo_list_item_json(&l, 0, tight2, (size_t)need) == -1,
          "pf cap = strlen 时应 -1");
    CHECK(photo_list_item_json(&l, 0, tight2, (size_t)need + 1) == need,
          "pf cap = strlen+1 时应成功");

    /* 手工构造的坏清单：名字带引号时不得把 JSON 注坏 */
    photo_list_t raw;
    photo_list_init(&raw);
    snprintf(raw.items[0].name, sizeof(raw.items[0].name), "%s", "x\",\"t\":\"pz");
    raw.items[0].kind = PHOTO_KIND_PGJ;
    raw.items[0].bytes = 1;
    raw.items[0].frames = 1;
    raw.items[0].width = 1;
    raw.items[0].height = 1;
    raw.count = 1;
    CHECK(photo_list_item_json(&raw, 0, buf, sizeof(buf)) == -1,
          "非法名字（注入）应拒绝生成 JSON");
}

/* ── 6. 最坏情况：63 字符名字 + 全字段最大值，仍须 < 180 B ── */

static void test_json_worst_case(void)
{
    char long_name[PHOTO_NAME_MAX];
    memset(long_name, 'w', 59);
    memcpy(long_name + 59, ".pgj", 4);
    long_name[63] = '\0';

    photo_list_t l;
    photo_list_init(&l);

    /* 正常加入后再改大数值（photo_list_add 不限制 bytes/fps 上限，广播才需要） */
    photo_item_t it = make_item(long_name, PHOTO_KIND_PGI, 0xFFFFFFFFu, 65535, 255, 466, 466);
    CHECK(photo_list_add(&l, &it), "最坏字段组合应能加入清单");

    char buf[512];
    int n = photo_list_item_json(&l, 0, buf, sizeof(buf));
    CHECK(n > 0, "最坏情况 pf 应成功");
    CHECK((size_t)n < JSON_LIMIT, "最坏情况 pf 必须 < 180 B");
    CHECK(strlen(buf) < JSON_LIMIT, "最坏情况 strlen 必须 < 180 B");
    CHECK(strstr(buf, long_name) != NULL, "63 字符文件名应完整出现");
    CHECK(strstr(buf, "4294967295") != NULL, "bytes 最大值应完整出现");

    /* index 取最大（127）时再确认一次 */
    photo_list_t big;
    photo_list_init(&big);
    big.items[127] = it;
    big.count = PHOTO_MAX_FILES;
    n = photo_list_item_json(&big, 127, buf, sizeof(buf));
    CHECK(n > 0, "最大下标 pf 应成功");
    CHECK((size_t)n < JSON_LIMIT, "最大下标 + 最长名字仍须 < 180 B");
    CHECK(strstr(buf, "\"i\":127") != NULL, "下标 127 应正确输出");

    /* 所有 128 项的 JSON 都 < 180 B（穷举一遍，防止某天加字段破坏上限） */
    photo_list_t full;
    photo_list_init(&full);
    char name[PHOTO_NAME_MAX];
    for (int i = 0; i < PHOTO_MAX_FILES; i++) {
        photo_make_name(name, sizeof(name), (uint32_t)i, PHOTO_KIND_PGJ);
        photo_item_t x = make_item(name, PHOTO_KIND_PGJ, 512000, 1, 0, 466, 466);
        CHECK(photo_list_add(&full, &x), "全量清单项应能加入");
    }
    int worst = 0;
    for (int i = 0; i < full.count; i++) {
        int len = photo_list_item_json(&full, i, buf, sizeof(buf));
        CHECK(len > 0 && len < JSON_LIMIT, "每一项 pf 都必须 < 180 B");
        if (len > worst) worst = len;
    }
    printf("  清单 pf 最长 %d B（上限 180 B）\n", worst);
}

int main(void)
{
    printf("=== test_photo_list（清单增删排序 / 文件名白名单 / 分页 JSON <180B）===\n");

    test_name_valid();
    test_add_find_remove();
    test_capacity();
    test_sort();
    test_make_name();
    test_json();
    test_json_worst_case();

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
