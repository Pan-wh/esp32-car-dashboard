/**
 * test_photo_sync.c —— 上传接收状态机的 **纯逻辑** 单测（不需要硬件）
 *
 * 覆盖重点：
 *   1. 六个状态 × busy/active/outstanding 谓词矩阵；
 *   2. begin 的每条结果（OK / BUSY / BAD_ARG / NO_SPACE / UNSUPPORTED / too_large）
 *      与续传（resume>0 → RECEIVING，received=committed=offset）；
 *   3. 分片偏移严格校验（want_off）+ 越过 total + 各非法状态；
 *   4. 落盘进度的单调性 / 不超过 received；
 *   5. end → COMMITTING → ok/fail 的完整迁移（含不完整 end）；
 *   6. pause / tick 超时（含 now_ms 回绕）；
 *   7. credit 判定（1 KB 步长 / 100 ms 间隔 / 无前进不发）。
 *
 * 状态机是"协议正确性"的关键路径：这里用**穷举迁移**而不是只测 happy path，
 * 因为错一次就是"App 以为传完、设备显示半张图"。
 */
#include "../simulator/ui/pgj.h"       /* 只为上限常量（512 KB 载荷 / 466 边长） */
#include "../simulator/ui/photo_sync.h"

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

/* ── 工具 ──────────────────────────────────────────────────── */

static photo_sync_t fresh(void)
{
    photo_sync_t s;
    photo_sync_init(&s);
    return s;
}

static uint32_t free_for(uint32_t bytes)
{
    /* 头文件要求 free > bytes + reserve（严格大于）→ 多给 1 字节 */
    return bytes + PHOTO_SYNC_SPACE_RESERVE + 1u;
}

/** 用合法参数 begin 一次静态上传（文件名/CRC 固定，便于断言） */
static photo_sync_result_t begin_static(photo_sync_t *s, uint32_t bytes, uint32_t now_ms)
{
    return photo_sync_begin(s, (uint8_t)PHOTO_KIND_PGJ, bytes, 0xDEADBEEFu, 466, 466, 1, 0,
                            free_for(bytes), 0, "p00012.pgj", now_ms);
}

static void expect_state(const photo_sync_t *s, photo_sync_state_t want, const char *msg)
{
    CHECK(s->state == want, msg);
}

/* ── 1. 初始化与谓词矩阵 ──────────────────────────────────── */

static void test_init_and_predicates(void)
{
    photo_sync_t s = fresh();
    expect_state(&s, PHOTO_SYNC_IDLE, "init 后应为 IDLE");
    CHECK(s.total_bytes == 0 && s.received_bytes == 0 && s.committed_bytes == 0,
          "init 后计数全 0");
    CHECK(s.name[0] == '\0' && s.err[0] == '\0', "init 后 name/err 为空");
    CHECK(!photo_sync_busy(&s), "IDLE 不 busy");
    CHECK(!photo_sync_active(&s), "IDLE 不 active");
    CHECK(photo_sync_outstanding(&s) == 0, "IDLE outstanding = 0");

    /* 六个状态的谓词矩阵 */
    struct {
        photo_sync_state_t st;
        bool busy;
        bool active;
    } cases[] = {
        { PHOTO_SYNC_IDLE, false, false },
        { PHOTO_SYNC_WAIT_DATA, true, true },
        { PHOTO_SYNC_RECEIVING, true, true },
        { PHOTO_SYNC_COMMITTING, true, false },
        { PHOTO_SYNC_PAUSED, false, false },
        { PHOTO_SYNC_ERROR, false, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        photo_sync_t x = fresh();
        x.state = cases[i].st;
        CHECK(photo_sync_busy(&x) == cases[i].busy, "busy 谓词矩阵");
        CHECK(photo_sync_active(&x) == cases[i].active, "active 谓词矩阵");
    }

    CHECK(!photo_sync_busy(NULL) && !photo_sync_active(NULL), "NULL 谓词安全");
    CHECK(photo_sync_outstanding(NULL) == 0, "NULL outstanding = 0");
    CHECK(!photo_sync_need_credit(NULL, 0, 0, 0), "NULL need_credit = false");
    photo_sync_init(NULL);
    photo_sync_tick(NULL, 0);
    photo_sync_pause(NULL);
    photo_sync_commit_ok(NULL);
    photo_sync_commit_fail(NULL, "crc");
    photo_sync_on_committed(NULL, 0);
    CHECK(!photo_sync_accept_chunk(NULL, 0, 1, NULL, NULL, 0), "NULL accept_chunk = false");
    CHECK(!photo_sync_request_end(NULL, NULL), "NULL request_end = false");
    CHECK(photo_sync_begin(NULL, 0, 1, 0, 1, 1, 1, 0, 2000000, 0, "p1.pgj", 0) ==
              PHOTO_SYNC_BAD_ARG,
          "NULL begin = BAD_ARG");
}

/* ── 2. begin 成功路径 ─────────────────────────────────────── */

static void test_begin_ok(void)
{
    photo_sync_t s = fresh();
    photo_sync_result_t r = photo_sync_begin(
        &s, (uint8_t)PHOTO_KIND_PGJ, 10000, 0x12345678u, 466, 466, 1, 0, free_for(10000), 0,
        "p00012.pgj", 1000);
    CHECK(r == PHOTO_SYNC_OK, "全新 begin 应 OK");
    expect_state(&s, PHOTO_SYNC_WAIT_DATA, "resume=0 → WAIT_DATA");
    CHECK(s.kind == PHOTO_KIND_PGJ, "kind 正确");
    CHECK(s.total_bytes == 10000 && s.crc_expect == 0x12345678u, "bytes/crc 正确");
    CHECK(s.width == 466 && s.height == 466 && s.frames == 1 && s.fps == 0, "几何字段正确");
    CHECK(s.received_bytes == 0 && s.committed_bytes == 0, "计数从 0 开始");
    CHECK(s.started_ms == 1000 && s.last_data_ms == 1000, "时间戳 = now_ms");
    CHECK(strcmp(s.name, "p00012.pgj") == 0, "文件名已保存");
    CHECK(s.err[0] == '\0', "成功时 err 为空");
    CHECK(photo_sync_busy(&s) && photo_sync_active(&s), "WAIT_DATA 占通道且 active");

    /* 续传：resume>0 直接进 RECEIVING，received=committed=offset */
    photo_sync_t r2 = fresh();
    r = photo_sync_begin(&r2, (uint8_t)PHOTO_KIND_PGJ, 10000, 0x12345678u, 466, 466, 1, 0,
                         free_for(10000), 5000, "p00012.pgj", 2000);
    CHECK(r == PHOTO_SYNC_OK, "续传 begin 应 OK");
    expect_state(&r2, PHOTO_SYNC_RECEIVING, "resume>0 → RECEIVING");
    CHECK(r2.received_bytes == 5000 && r2.committed_bytes == 5000,
          "续传时 received=committed=resume");
    CHECK(r2.started_ms == 2000 && r2.last_data_ms == 2000, "续传同样重置时间戳");

    /* resume == bytes：数据已齐，只等 end */
    photo_sync_t r3 = fresh();
    r = photo_sync_begin(&r3, (uint8_t)PHOTO_KIND_PGJ, 1000, 1, 16, 16, 1, 0, free_for(1000), 1000,
                         "p00001.pgj", 0);
    CHECK(r == PHOTO_SYNC_OK, "resume=bytes 应 OK");
    CHECK(r3.received_bytes == r3.total_bytes, "received 已等于 total");

    /* 动图 begin */
    photo_sync_t a = fresh();
    r = photo_sync_begin(&a, (uint8_t)PHOTO_KIND_PGI, 1114112, 1, 300, 300, 12, 8,
                         free_for(1114112), 0, "p00002.pgi", 0);
    CHECK(r == PHOTO_SYNC_OK, "动图 begin 应 OK");
    CHECK(a.kind == PHOTO_KIND_PGI && a.frames == 12 && a.fps == 8, "动图字段正确");

    /* 动图单帧（fps=0）允许：旧素材兼容 */
    photo_sync_t ps = fresh();
    r = photo_sync_begin(&ps, (uint8_t)PHOTO_KIND_PGI, 4096, 1, 300, 300, 1, 0, free_for(4096), 0,
                         "p00003.pgi", 0);
    CHECK(r == PHOTO_SYNC_OK, "单帧 .pgi（fps=0）应 OK");

    /* 失败后再 begin：err 必须被清掉 */
    photo_sync_t e = fresh();
    uint32_t want = 0;
    (void)photo_sync_accept_chunk(&e, 0, 10, &want, NULL, 0); /* IDLE → bad_arg */
    CHECK(e.err[0] != '\0', "失败后 err 非空");
    CHECK(begin_static(&e, 100, 0) == PHOTO_SYNC_OK, "失败后仍可 begin");
    CHECK(e.err[0] == '\0', "begin 成功应清 err");

    /* 从 ERROR / PAUSED 也能重新 begin（错误/暂停不是终态） */
    photo_sync_t err_state = fresh();
    err_state.state = PHOTO_SYNC_ERROR;
    CHECK(begin_static(&err_state, 100, 0) == PHOTO_SYNC_OK, "ERROR 状态可重新 begin");
    photo_sync_t paused = fresh();
    paused.state = PHOTO_SYNC_PAUSED;
    CHECK(begin_static(&paused, 100, 0) == PHOTO_SYNC_OK, "PAUSED 状态可续传 begin");
}

/* ── 3. begin：BUSY ────────────────────────────────────────── */

static void test_begin_busy(void)
{
    photo_sync_state_t busy_states[] = { PHOTO_SYNC_WAIT_DATA, PHOTO_SYNC_RECEIVING,
                                         PHOTO_SYNC_COMMITTING };
    for (size_t i = 0; i < sizeof(busy_states) / sizeof(busy_states[0]); i++) {
        photo_sync_t s = fresh();
        s.state = busy_states[i];
        photo_sync_result_t r = begin_static(&s, 100, 0);
        CHECK(r == PHOTO_SYNC_BUSY, "忙时 begin 应 BUSY");
        CHECK(strcmp(s.err, "busy") == 0, "忙时 err=busy");
        CHECK(s.state == busy_states[i], "忙时状态不变");
    }

    /* busy 优先于参数校验：即使参数非法也回 BUSY（语义更明确） */
    photo_sync_t s = fresh();
    s.state = PHOTO_SYNC_RECEIVING;
    photo_sync_result_t r = photo_sync_begin(&s, 9, 0, 0, 0, 0, 0, 0, 0, 0, NULL, 0);
    CHECK(r == PHOTO_SYNC_BUSY, "忙时非法参数仍回 BUSY");
}

/* ── 4. begin：BAD_ARG / UNSUPPORTED / NO_SPACE / too_large ── */

static void test_begin_rejects(void)
{
    photo_sync_t s;
    photo_sync_result_t r;

    /* 每次都用全新状态，确保失败不会留下半截会话 */
    s = fresh();
    r = photo_sync_begin(&s, 2, 100, 0, 16, 16, 1, 0, free_for(100), 0, "p1.pgj", 0);
    CHECK(r == PHOTO_SYNC_UNSUPPORTED && strcmp(s.err, "unsupported") == 0,
          "kind>1 → UNSUPPORTED");
    expect_state(&s, PHOTO_SYNC_IDLE, "非法 kind 后仍 IDLE");

    s = fresh();
    r = photo_sync_begin(&s, 0, 100, 0, 16, 16, 0, 0, free_for(100), 0, "p1.pgj", 0);
    CHECK(r == PHOTO_SYNC_BAD_ARG && strcmp(s.err, "bad_arg") == 0, "frames=0 → BAD_ARG");

    s = fresh();
    r = photo_sync_begin(&s, 0, 100, 0, 16, 16, 2, 0, free_for(100), 0, "p1.pgj", 0);
    CHECK(r == PHOTO_SYNC_BAD_ARG, ".pgj 多帧 → BAD_ARG");

    s = fresh();
    r = photo_sync_begin(&s, 1, 100, 0, 16, 16, 3, 0, free_for(100), 0, "p1.pgi", 0);
    CHECK(r == PHOTO_SYNC_UNSUPPORTED && strcmp(s.err, "unsupported") == 0,
          "动图 fps=0 → UNSUPPORTED");

    /* 宽高 */
    s = fresh();
    CHECK(photo_sync_begin(&s, 0, 100, 0, 0, 16, 1, 0, free_for(100), 0, "p1.pgj", 0) ==
              PHOTO_SYNC_BAD_ARG, "w=0 → BAD_ARG");
    s = fresh();
    CHECK(photo_sync_begin(&s, 0, 100, 0, 467, 16, 1, 0, free_for(100), 0, "p1.pgj", 0) ==
              PHOTO_SYNC_BAD_ARG, "w=467 → BAD_ARG");
    s = fresh();
    CHECK(photo_sync_begin(&s, 0, 100, 0, 16, 0, 1, 0, free_for(100), 0, "p1.pgj", 0) ==
              PHOTO_SYNC_BAD_ARG, "h=0 → BAD_ARG");
    s = fresh();
    CHECK(photo_sync_begin(&s, 0, 100, 0, 16, 467, 1, 0, free_for(100), 0, "p1.pgj", 0) ==
              PHOTO_SYNC_BAD_ARG, "h=467 → BAD_ARG");

    /* bytes=0 / 超上限 */
    s = fresh();
    CHECK(photo_sync_begin(&s, 0, 0, 0, 16, 16, 1, 0, free_for(1), 0, "p1.pgj", 0) ==
              PHOTO_SYNC_BAD_ARG, "bytes=0 → BAD_ARG");

    const uint32_t pgj_cap = (uint32_t)PGJ_HEADER_BYTES + (uint32_t)PGJ_MAX_JPEG_BYTES;
    s = fresh();
    r = photo_sync_begin(&s, 0, pgj_cap + 1, 0, 16, 16, 1, 0, free_for(pgj_cap + 1), 0,
                         "p1.pgj", 0);
    CHECK(r == PHOTO_SYNC_BAD_ARG && strcmp(s.err, "too_large") == 0,
          ".pgj 超过 512 KB+32 → BAD_ARG + too_large");
    s = fresh();
    r = photo_sync_begin(&s, 0, pgj_cap, 0, 16, 16, 1, 0, free_for(pgj_cap), 0, "p1.pgj", 0);
    CHECK(r == PHOTO_SYNC_OK, ".pgj 恰好到上限应 OK");

    const uint32_t pgi_cap = 8u * 1024u * 1024u;
    s = fresh();
    r = photo_sync_begin(&s, 1, pgi_cap + 1, 0, 16, 16, 1, 0, free_for(pgi_cap + 1), 0,
                         "p1.pgi", 0);
    CHECK(r == PHOTO_SYNC_BAD_ARG && strcmp(s.err, "too_large") == 0,
          ".pgi 超过 8 MB → BAD_ARG + too_large");
    s = fresh();
    r = photo_sync_begin(&s, 1, pgi_cap, 0, 16, 16, 1, 0, free_for(pgi_cap), 0, "p1.pgi", 0);
    CHECK(r == PHOTO_SYNC_OK, ".pgi 恰好到上限应 OK");

    /* 续传偏移非法 */
    s = fresh();
    r = photo_sync_begin(&s, 0, 100, 0, 16, 16, 1, 0, free_for(100), 101, "p1.pgj", 0);
    CHECK(r == PHOTO_SYNC_BAD_ARG, "resume > bytes → BAD_ARG");

    /* 文件名白名单 */
    s = fresh();
    CHECK(photo_sync_begin(&s, 0, 100, 0, 16, 16, 1, 0, free_for(100), 0, NULL, 0) ==
              PHOTO_SYNC_BAD_ARG, "name=NULL → BAD_ARG");
    s = fresh();
    CHECK(photo_sync_begin(&s, 0, 100, 0, 16, 16, 1, 0, free_for(100), 0, "evil.jpg", 0) ==
              PHOTO_SYNC_BAD_ARG, "name 非法扩展名 → BAD_ARG");
    s = fresh();
    CHECK(photo_sync_begin(&s, 0, 100, 0, 16, 16, 1, 0, free_for(100), 0, "../x.pgj", 0) ==
              PHOTO_SYNC_BAD_ARG, "name 路径穿越 → BAD_ARG");

    /* 空间：必须严格大于 bytes + reserve */
    s = fresh();
    r = photo_sync_begin(&s, 0, 100000, 0, 16, 16, 1, 0,
                         100000u + PHOTO_SYNC_SPACE_RESERVE, 0, "p1.pgj", 0);
    CHECK(r == PHOTO_SYNC_NO_SPACE && strcmp(s.err, "no_space") == 0,
          "free == bytes+reserve → NO_SPACE");
    expect_state(&s, PHOTO_SYNC_IDLE, "NO_SPACE 后仍 IDLE");

    s = fresh();
    r = photo_sync_begin(&s, 0, 100000, 0, 16, 16, 1, 0,
                         100000u + PHOTO_SYNC_SPACE_RESERVE + 1u, 0, "p1.pgj", 0);
    CHECK(r == PHOTO_SYNC_OK, "free == bytes+reserve+1 → OK");
}

/* ── 5. accept_chunk ───────────────────────────────────────── */

static void test_accept_chunk(void)
{
    photo_sync_t s = fresh();
    CHECK(begin_static(&s, 1000, 100) == PHOTO_SYNC_OK, "begin 成功");

    uint32_t want = 0xAAAAAAAAu;
    const char *err = NULL;

    /* 偏移不符：给出 want，状态/计数不变 */
    CHECK(!photo_sync_accept_chunk(&s, 10, 178, &want, &err, 200), "错误偏移应拒绝");
    CHECK(strcmp(err, "offset") == 0, "err = offset");
    CHECK(want == 0, "want_off = 已接收偏移 0");
    CHECK(strcmp(s.err, "offset") == 0, "结构体 err 也记录 offset");
    expect_state(&s, PHOTO_SYNC_WAIT_DATA, "偏移错后仍在 WAIT_DATA");
    CHECK(s.received_bytes == 0, "偏移错后 received 不变");

    /* 成功：WAIT_DATA → RECEIVING，刷新 last_data，want 不动 */
    want = 0xAAAAAAAAu;
    CHECK(photo_sync_accept_chunk(&s, 0, 500, &want, &err, 200), "首片应接受");
    expect_state(&s, PHOTO_SYNC_RECEIVING, "首片后 RECEIVING");
    CHECK(s.received_bytes == 500, "received = 500");
    CHECK(s.last_data_ms == 200, "last_data_ms 刷新");
    CHECK(want == 0xAAAAAAAAu, "成功时不写 want_off");
    CHECK(s.err[0] == '\0', "成功后清 err");

    CHECK(photo_sync_accept_chunk(&s, 500, 500, NULL, NULL, 300), "次片（恰好到尾）应接受");
    CHECK(s.received_bytes == 1000, "received = total");
    CHECK(s.committed_bytes == 0, "committed 仍为 0（写盘是主循环的事）");

    /* 超出 total：offset 正确但 len 越界 */
    want = 0xAAAAAAAAu;
    err = NULL;
    CHECK(!photo_sync_accept_chunk(&s, 1000, 1, &want, &err, 400), "越过 total 应拒绝");
    CHECK(strcmp(err, "bad_arg") == 0, "越过 total 的 err = bad_arg");
    CHECK(want == 0xAAAAAAAAu, "非 offset 错不写 want_off");

    /* len=0 */
    CHECK(!photo_sync_accept_chunk(&s, 1000, 0, NULL, &err, 400), "len=0 应拒绝");
    CHECK(strcmp(err, "bad_arg") == 0, "len=0 的 err = bad_arg");

    /* 未 begin / 已 pause / ERROR 状态 */
    photo_sync_t idle = fresh();
    err = NULL;
    CHECK(!photo_sync_accept_chunk(&idle, 0, 1, NULL, &err, 0), "IDLE 收片应拒绝");
    CHECK(strcmp(err, "bad_arg") == 0, "IDLE 的 err = bad_arg");

    photo_sync_t paused = fresh();
    paused.state = PHOTO_SYNC_PAUSED;
    err = NULL;
    CHECK(!photo_sync_accept_chunk(&paused, 0, 1, NULL, &err, 0), "PAUSED 收片应拒绝");
    CHECK(strcmp(err, "bad_arg") == 0, "PAUSED 的 err = bad_arg");

    photo_sync_t errstate = fresh();
    errstate.state = PHOTO_SYNC_ERROR;
    CHECK(!photo_sync_accept_chunk(&errstate, 0, 1, NULL, &err, 0), "ERROR 收片应拒绝");

    /* COMMITTING 期间新分片：必须 busy（photo_rt 注释要求） */
    photo_sync_t committing = fresh();
    committing.state = PHOTO_SYNC_COMMITTING;
    err = NULL;
    CHECK(!photo_sync_accept_chunk(&committing, 0, 1, NULL, &err, 0), "COMMITTING 收片应拒绝");
    CHECK(strcmp(err, "busy") == 0, "COMMITTING 的 err = busy");

    /* 首片之前 state 保持 WAIT_DATA；分片不改变 total/crc */
    CHECK(s.total_bytes == 1000 && s.crc_expect == 0xDEADBEEFu, "分片不改 begin 元数据");
}

/* ── 6. on_committed ───────────────────────────────────────── */

static void test_on_committed(void)
{
    photo_sync_t s = fresh();
    CHECK(begin_static(&s, 1000, 0) == PHOTO_SYNC_OK, "begin 成功");
    CHECK(photo_sync_accept_chunk(&s, 0, 800, NULL, NULL, 10), "收 800 字节");

    photo_sync_on_committed(&s, 500);
    CHECK(s.committed_bytes == 500, "committed = 500");
    CHECK(photo_sync_outstanding(&s) == 300, "outstanding = 300");

    photo_sync_on_committed(&s, 500);
    CHECK(s.committed_bytes == 500, "重复上报同一进度不变");
    photo_sync_on_committed(&s, 300);
    CHECK(s.committed_bytes == 500, "进度不允许倒退");

    photo_sync_on_committed(&s, 900); /* 超过 received → 夹取 */
    CHECK(s.committed_bytes == 800, "committed 不得超过 received，夹取到 800");
    CHECK(photo_sync_outstanding(&s) == 0, "outstanding = 0");

    photo_sync_on_committed(&s, 0);
    CHECK(s.committed_bytes == 800, "0 不大于当前值，忽略");
}

/* ── 7. request_end / commit ───────────────────────────────── */

static void test_end_and_commit(void)
{
    photo_sync_t s = fresh();
    CHECK(begin_static(&s, 100, 0) == PHOTO_SYNC_OK, "begin 成功");
    CHECK(photo_sync_accept_chunk(&s, 0, 99, NULL, NULL, 1), "收 99/100");

    const char *err = "sentinel";
    CHECK(!photo_sync_request_end(&s, &err), "不完整时 request_end 应 false");
    CHECK(err != NULL && strcmp(err, "bad_arg") == 0, "不完整时 err = bad_arg");
    expect_state(&s, PHOTO_SYNC_RECEIVING, "不完整时状态不变");
    CHECK(strcmp(s.err, "bad_arg") == 0, "结构体 err 同步");

    CHECK(photo_sync_accept_chunk(&s, 99, 1, NULL, NULL, 2), "收完最后 1 字节");
    err = "sentinel";
    CHECK(photo_sync_request_end(&s, &err), "完整时 request_end 应 true");
    CHECK(err == NULL, "成功时 err = NULL");
    expect_state(&s, PHOTO_SYNC_COMMITTING, "end 后 COMMITTING");
    CHECK(photo_sync_busy(&s) && !photo_sync_active(&s), "COMMITTING busy 但不 active");

    CHECK(!photo_sync_request_end(&s, NULL), "重复 end 应拒绝（COMMITTING 状态）");

    /* 提交成功 → IDLE；err 清空 */
    photo_sync_commit_ok(&s);
    expect_state(&s, PHOTO_SYNC_IDLE, "commit_ok 后 IDLE");
    CHECK(s.err[0] == '\0', "commit_ok 清 err");

    /* 提交失败 → ERROR + 错误码 */
    photo_sync_t f = fresh();
    CHECK(begin_static(&f, 10, 0) == PHOTO_SYNC_OK, "begin 成功");
    CHECK(photo_sync_accept_chunk(&f, 0, 10, NULL, NULL, 1), "收完");
    CHECK(photo_sync_request_end(&f, NULL), "end 成功");
    photo_sync_commit_fail(&f, "crc");
    expect_state(&f, PHOTO_SYNC_ERROR, "commit_fail 后 ERROR");
    CHECK(strcmp(f.err, "crc") == 0, "err = crc");
    CHECK(!photo_sync_busy(&f) && !photo_sync_active(&f), "ERROR 不占通道");

    /* err=NULL / 空串 → 归一成 io */
    photo_sync_t f2 = fresh();
    f2.state = PHOTO_SYNC_COMMITTING;
    photo_sync_commit_fail(&f2, NULL);
    CHECK(strcmp(f2.err, "io") == 0, "commit_fail(NULL) → io");
    f2.state = PHOTO_SYNC_COMMITTING;
    photo_sync_commit_fail(&f2, "");
    CHECK(strcmp(f2.err, "io") == 0, "commit_fail(\"\") → io");

    /* 错误码超长必须截断（err 只有 16 B） */
    photo_sync_t f3 = fresh();
    f3.state = PHOTO_SYNC_COMMITTING;
    photo_sync_commit_fail(&f3, "a-very-long-error-code-string");
    CHECK(strlen(f3.err) == sizeof(f3.err) - 1, "超长错误码截断到 15 字符");
    CHECK(strncmp(f3.err, "a-very-long-err", 15) == 0, "截断内容为前缀");

    /* 各种状态下 request_end 拒绝 */
    photo_sync_state_t bad_states[] = { PHOTO_SYNC_IDLE, PHOTO_SYNC_COMMITTING,
                                        PHOTO_SYNC_PAUSED, PHOTO_SYNC_ERROR };
    for (size_t i = 0; i < sizeof(bad_states) / sizeof(bad_states[0]); i++) {
        photo_sync_t x = fresh();
        x.state = bad_states[i];
        CHECK(!photo_sync_request_end(&x, NULL), "非接收态 request_end 应拒绝");
    }

    /* begin 时 resume == total → 直接 end 成功 */
    photo_sync_t resume = fresh();
    CHECK(photo_sync_begin(&resume, 0, 500, 1, 16, 16, 1, 0, free_for(500), 500, "p1.pgj", 0) ==
              PHOTO_SYNC_OK,
          "resume=total begin 成功");
    CHECK(photo_sync_request_end(&resume, NULL), "resume=total 可直接 end");
    expect_state(&resume, PHOTO_SYNC_COMMITTING, "resume=total end 后 COMMITTING");
}

/* ── 8. pause ──────────────────────────────────────────────── */

static void test_pause(void)
{
    photo_sync_state_t before[] = { PHOTO_SYNC_IDLE, PHOTO_SYNC_WAIT_DATA,
                                    PHOTO_SYNC_RECEIVING, PHOTO_SYNC_COMMITTING,
                                    PHOTO_SYNC_PAUSED, PHOTO_SYNC_ERROR };
    photo_sync_state_t after[] = { PHOTO_SYNC_IDLE, PHOTO_SYNC_PAUSED, PHOTO_SYNC_PAUSED,
                                   PHOTO_SYNC_COMMITTING, PHOTO_SYNC_PAUSED, PHOTO_SYNC_ERROR };
    for (size_t i = 0; i < sizeof(before) / sizeof(before[0]); i++) {
        photo_sync_t s = fresh();
        s.state = before[i];
        photo_sync_pause(&s);
        CHECK(s.state == after[i], "pause 的状态迁移矩阵");
    }
}

/* ── 9. tick 超时 ──────────────────────────────────────────── */

static void test_tick(void)
{
    /* WAIT_DATA：10 s 内不超时，到点转 PAUSED */
    photo_sync_t s = fresh();
    CHECK(begin_static(&s, 1000, 1000) == PHOTO_SYNC_OK, "begin 成功");
    photo_sync_tick(&s, 1000 + PHOTO_SYNC_TIMEOUT_MS - 1);
    expect_state(&s, PHOTO_SYNC_WAIT_DATA, "差 1 ms 不超时");
    photo_sync_tick(&s, 1000 + PHOTO_SYNC_TIMEOUT_MS);
    expect_state(&s, PHOTO_SYNC_PAUSED, "满 10 s → PAUSED");

    /* RECEIVING：分片刷新计时 */
    photo_sync_t r = fresh();
    CHECK(begin_static(&r, 1000, 0) == PHOTO_SYNC_OK, "begin 成功");
    CHECK(photo_sync_accept_chunk(&r, 0, 100, NULL, NULL, 5000), "收片");
    photo_sync_tick(&r, 5000 + PHOTO_SYNC_TIMEOUT_MS - 1);
    expect_state(&r, PHOTO_SYNC_RECEIVING, "分片后重新计时");
    photo_sync_tick(&r, 5000 + PHOTO_SYNC_TIMEOUT_MS);
    expect_state(&r, PHOTO_SYNC_PAUSED, "分片后 10 s 无数据 → PAUSED");

    /* 非接收态 tick 不改状态 */
    photo_sync_state_t stable[] = { PHOTO_SYNC_IDLE, PHOTO_SYNC_COMMITTING, PHOTO_SYNC_PAUSED,
                                    PHOTO_SYNC_ERROR };
    for (size_t i = 0; i < sizeof(stable) / sizeof(stable[0]); i++) {
        photo_sync_t x = fresh();
        x.state = stable[i];
        x.last_data_ms = 0;
        photo_sync_tick(&x, 1000000);
        CHECK(x.state == stable[i], "非接收态 tick 不改状态");
    }

    /* now_ms 回绕（49 天）：无符号相减必须仍然正确 */
    photo_sync_t w = fresh();
    uint32_t base = 0xFFFFFF00u; /* 距回绕还剩 256 ms */
    CHECK(begin_static(&w, 1000, base) == PHOTO_SYNC_OK, "回绕前 begin");
    photo_sync_tick(&w, base + PHOTO_SYNC_TIMEOUT_MS - 1); /* 回绕后的时间点 */
    expect_state(&w, PHOTO_SYNC_WAIT_DATA, "回绕前后差 < 10 s 不超时");
    photo_sync_tick(&w, base + PHOTO_SYNC_TIMEOUT_MS);
    expect_state(&w, PHOTO_SYNC_PAUSED, "回绕后满 10 s → PAUSED");
}

/* ── 10. need_credit ───────────────────────────────────────── */

static void test_need_credit(void)
{
    photo_sync_t s = fresh();

    /* 无前进：永不发 */
    CHECK(!photo_sync_need_credit(&s, 0, 0, 0), "committed=0 不发");
    CHECK(!photo_sync_need_credit(&s, 0, 0, 100000), "无落盘进度即使超时也不发");

    /* 1 KB 步长：到点立刻发（不看时间） */
    s.committed_bytes = PHOTO_SYNC_CREDIT_STEP_BYTES;
    CHECK(photo_sync_need_credit(&s, 0, 0, 0), "增量 = 1024 → 立即发");
    s.committed_bytes = PHOTO_SYNC_CREDIT_STEP_BYTES - 1;
    CHECK(!photo_sync_need_credit(&s, 0, 0, 0), "增量 = 1023 且时间未到 → 不发");

    /* 时间间隔：>=100 ms 且有前进 → 发 */
    CHECK(!photo_sync_need_credit(&s, 0, 0, PHOTO_SYNC_CREDIT_INTERVAL_MS - 1),
          "增量不足且 99 ms → 不发");
    CHECK(photo_sync_need_credit(&s, 0, 0, PHOTO_SYNC_CREDIT_INTERVAL_MS),
          "增量不足但 100 ms → 发");

    /* 上次 credit 之后没前进：即使超时也不发（避免空 notify） */
    s.committed_bytes = 500;
    CHECK(!photo_sync_need_credit(&s, 500, 0, 100000), "committed 未超过 last_credit_off → 不发");

    /* 新会话（last_credit_off 比 committed 大）不能误触发 */
    s.committed_bytes = 100;
    CHECK(!photo_sync_need_credit(&s, 5000, 0, 100000), "last_credit_off > committed → 不发");

    /* 回绕：now_ms 从 near-max 绕回 0 */
    s.committed_bytes = 100;
    uint32_t last_ms = 0xFFFFFFF0u; /* 距回绕 16 ms */
    CHECK(!photo_sync_need_credit(&s, 0, last_ms, 0x00000000u), "回绕后仅 16 ms → 不发");
    CHECK(photo_sync_need_credit(&s, 0, last_ms, 0x00000000u + 84u),
          "回绕后累计 100 ms → 发");

    /* 常量与协议一致（改动这些数字会破坏与 iOS 的窗口协商） */
    CHECK(PHOTO_SYNC_WINDOW_BYTES == 4096, "win = 4096");
    CHECK(PHOTO_SYNC_CHUNK_HINT == 178, "chunk hint = 178");
    CHECK(PHOTO_SYNC_TIMEOUT_MS == 10000, "timeout = 10 s");
    CHECK(PHOTO_SYNC_SPACE_RESERVE == 1024u * 1024u, "reserve = 1 MB");
    CHECK(PHOTO_SYNC_CREDIT_STEP_BYTES == 1024, "credit step = 1 KB");
    CHECK(PHOTO_SYNC_CREDIT_INTERVAL_MS == 100, "credit interval = 100 ms");
}

/* ── 11. 端到端：全新上传 + 断连续传 ───────────────────────── */

static void test_full_flow(void)
{
    photo_sync_t s = fresh();
    const uint32_t total = 3000;
    CHECK(photo_sync_begin(&s, 0, total, 0x11223344u, 466, 466, 1, 0, free_for(total), 0,
                           "p00012.pgj", 0) == PHOTO_SYNC_OK,
          "begin 成功");

    /* 模拟 App 按 178 B 分片发完，主循环落后写盘；中途 credit 应被触发 */
    uint32_t off = 0;
    uint32_t last_credit_off = 0, last_credit_ms = 0;
    uint32_t now = 0;
    int credits = 0;
    while (off < total) {
        uint32_t n = total - off;
        if (n > PHOTO_SYNC_CHUNK_HINT) n = PHOTO_SYNC_CHUNK_HINT;
        now += 20;
        CHECK(photo_sync_accept_chunk(&s, off, n, NULL, NULL, now), "顺序分片应全部接受");
        off += n;
        /* 主循环每拍最多落盘 1 KB */
        uint32_t target = s.committed_bytes + 1024u;
        if (target > s.received_bytes) target = s.received_bytes;
        photo_sync_on_committed(&s, target);
        if (photo_sync_need_credit(&s, last_credit_off, last_credit_ms, now)) {
            last_credit_off = s.committed_bytes;
            last_credit_ms = now;
            credits++;
        }
    }
    CHECK(off == total && s.received_bytes == total, "全部数据已接收");
    CHECK(s.committed_bytes == total, "数据最终全部落盘");
    CHECK(photo_sync_outstanding(&s) == 0, "outstanding = 0");
    CHECK(credits > 0, "长传输过程中应发过 credit");

    CHECK(photo_sync_request_end(&s, NULL), "end 成功");
    photo_sync_commit_ok(&s);
    expect_state(&s, PHOTO_SYNC_IDLE, "提交成功后 IDLE");
    CHECK(!photo_sync_busy(&s), "完成后不占通道");

    /* ── 断连续传：收一半 → pause → begin(resume) → 收完 ── */
    photo_sync_t r = fresh();
    CHECK(begin_static(&r, 2000, 0) == PHOTO_SYNC_OK, "首次 begin");
    CHECK(photo_sync_accept_chunk(&r, 0, 1000, NULL, NULL, 100), "收 1000");
    photo_sync_on_committed(&r, 1000);
    photo_sync_pause(&r);
    expect_state(&r, PHOTO_SYNC_PAUSED, "断连 → PAUSED");
    CHECK(r.received_bytes == 1000 && r.committed_bytes == 1000, "暂停保留进度");

    /* App 重连后带 resume=1000 重新 begin */
    CHECK(photo_sync_begin(&r, 0, 2000, 0xDEADBEEFu, 466, 466, 1, 0, free_for(2000), 1000,
                           "p00012.pgj", 5000) == PHOTO_SYNC_OK,
          "续传 begin 成功");
    expect_state(&r, PHOTO_SYNC_RECEIVING, "续传后直接 RECEIVING");
    CHECK(r.received_bytes == 1000 && r.committed_bytes == 1000, "续传起点 = 已落盘偏移");

    CHECK(photo_sync_accept_chunk(&r, 1000, 1000, NULL, NULL, 5100), "续传收剩下 1000");
    CHECK(photo_sync_request_end(&r, NULL), "续传后可 end");
    photo_sync_commit_ok(&r);
    expect_state(&r, PHOTO_SYNC_IDLE, "续传完成 → IDLE");
}

int main(void)
{
    printf("=== test_photo_sync（上传状态机：begin/偏移/续传/超时/提交）===\n");

    test_init_and_predicates();
    test_begin_ok();
    test_begin_busy();
    test_begin_rejects();
    test_accept_chunk();
    test_on_committed();
    test_end_and_commit();
    test_pause();
    test_tick();
    test_need_credit();
    test_full_flow();

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
