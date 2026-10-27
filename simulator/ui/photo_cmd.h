/**
 * photo_cmd.h —— `PhotoCtl` 命令解析（纯逻辑）
 *
 * 输入是 App 发来的紧凑 JSON（≤180 B，见 `docs/photo-protocol.md` §4），
 * 输出是 `photo_cmd_t`。**只做语法/字段校验，不做业务判断**（空间、能力位等由运行时决定）。
 *
 * 设计约定（与仓库既有 `control.c` 一致）：
 *   - 手写扫描，不引入 JSON 库（内存/栈都要小）；
 *   - 未知字段忽略；未知 `op` / 缺必填字段 / 类型不对 → 返回 false；
 *   - 数值范围做保守夹取/拒绝，避免把脏数据传到 SD/BLE 层。
 */
#ifndef PHOTO_CMD_H
#define PHOTO_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "photo_list.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHOTO_OP_NONE = 0,
    PHOTO_OP_BEGIN,  /* {"t":"p","op":"begin","kind":0,"bytes":...,"crc":...,"w":466,"h":466,"frames":1,"fps":0} */
    PHOTO_OP_END,    /* end */
    PHOTO_OP_ABORT,  /* abort */
    PHOTO_OP_DEL,    /* {"op":"del","id":"p00012.pgj"} */
    PHOTO_OP_PLAY,   /* {"op":"play","i":3} */
    PHOTO_OP_SET,    /* {"op":"set","slide":1,"intv":30,"shift":1,"soff_s":1800} */
    PHOTO_OP_LIST,   /* list */
    PHOTO_OP_BENCH,  /* bench */
    PHOTO_OP_FORMAT  /* 格式化仪表 SD 卡（危险操作，App 必须二次确认）*/
} photo_op_t;

typedef struct {
    photo_op_t op;

    /* begin */
    uint8_t  kind;    /* 0 = .pgj 静态，1 = .pgi 动图 */
    uint32_t bytes;   /* 文件总字节数（>0） */
    uint32_t crc;     /* 整个文件 CRC32 */
    uint16_t width;   /* 1..466 */
    uint16_t height;  /* 1..466 */
    uint16_t frames;  /* >=1 */
    uint8_t  fps;     /* kind=1 时 >0；kind=0 时忽略 */

    /* del */
    char id[PHOTO_NAME_MAX];

    /* play */
    int index; /* >= -1；-1 = 无指定 */

    /* set（可只带部分字段） */
    bool has_slide;
    bool has_intv;
    bool has_shift;
    bool has_soff;
    int  slide;  /* 0/1 */
    int  intv;   /* 秒，1..3600 */
    int  shift;  /* 0/1 */
    int  soff_s; /* 秒，0..86400（0=关） */
} photo_cmd_t;

/**
 * 解析一条命令。
 *
 * 必填校验：
 *   - `{"t":"p"}`；
 *   - begin：kind∈{0,1}、bytes>0、w/h∈[1,466]、frames>=1、kind=1 时 fps>=1；
 *   - del：id 通过 `photo_name_valid()`；
 *   - play：index >= 0（越界由运行时夹取）；
 *   - set：至少带一个已知字段；slide/shift∈{0,1}；intv∈[1,3600]；soff_s∈[0,86400]。
 *
 * @param json 以 0 结尾或给定 len；允许首尾空白
 * @param len  可用长度（0 时用 strlen）
 */
bool photo_cmd_parse(const char *json, size_t len, photo_cmd_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PHOTO_CMD_H */
