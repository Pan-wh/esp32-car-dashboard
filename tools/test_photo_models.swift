/**
 * test_photo_models.swift —— 照片协议模型层测试（Parser / PGJ / CRC32 / 命令 / 事件）
 *
 * 运行（**不改 tests/Makefile**，直接 swiftc 直编）：
 *   swiftc -O -o /tmp/test_photo_models \
 *     ios/CarDashboardPhone/CarDashboardPhone/Photo/PhotoProtocol.swift \
 *     ios/CarDashboardPhone/CarDashboardPhone/Photo/DevicePhotos.swift \
 *     tools/test_photo_models.swift && /tmp/test_photo_models
 *
 * 编译的是 App 的真实源文件（不内联复制），所以测的就是上真机的那份逻辑。
 *
 * 覆盖：CRC32 标准向量、PGJ 头字节级布局/校验、命令 JSON（字段名/顺序/≤180 B）、
 * 事件解析（go/credit/done/err/changed/info、pq/pf/pz）、
 * Info/State/清单项的"缺字段 vs 真的是 0"、supportsAnimation 三层门控中的两层、
 * 文件名白名单、分片帧编码、上传模型的进度/ETA。
 */

import Foundation

var gPass = 0
var gFail = 0
func check(_ cond: Bool, _ msg: @autoclosure () -> String) {
    if cond { gPass += 1 } else { gFail += 1; print("  ✗ \(msg())") }
}
func checkEq<T: Equatable>(_ actual: T, _ expected: T, _ msg: @autoclosure () -> String) {
    if actual == expected { gPass += 1 } else {
        gFail += 1
        print("  ✗ \(msg())：期望 \(expected)，实际 \(actual)")
    }
}

// MARK: - 夹具

/// 假的 baseline JPEG 载荷（内容无所谓，PGJ 只打包/校验字节）
let fakeJPEG = Data((0..<1024).map { UInt8($0 & 0xFF) })

/// 协议 §2 的示例（LITE 档：cap 支持动图但 tier=1、fps_meas=4 → 动图不可用）
let infoJSON = """
{"t":"pi","v":1,"tier":1,"cap":7,"px":466,"max_kb":512,\
"anim":{"px":300,"fps":8,"frames":12,"max_kb":2048,"fps_meas":4},\
"bench":{"dec_ms":230}}
"""

/// 协议 §3 的示例
let stateJSON = """
{"t":"ps","cur":3,"n":12,"stor":1,"free_kb":5230,\
"slide":1,"intv":30,"shift":1,"soff_s":1800,"rx":0}
"""

/// 协议 §5 的清单项示例
let itemJSON = """
{"t":"pf","i":0,"id":"p00012.pgj","b":45210,"f":1,"k":0,"w":466,"h":466,"fps":0}
"""

// MARK: - CRC32

func testCRC32() {
    print("--- CRC32（必须与 zlib / 固件 pgj_crc32 一致）---")
    checkEq(PhotoProtocol.crc32(Data()), UInt32(0x0000_0000), "空数据 CRC")
    checkEq(PhotoProtocol.crc32(Data("123456789".utf8)), UInt32(0xCBF4_3926), "标准向量 123456789")
    checkEq(PhotoProtocol.crc32(Data("The quick brown fox jumps over the lazy dog".utf8)),
            UInt32(0x414F_A339), "标准向量 fox")
    // Data 与 [UInt8] 重载必须给出同一结果
    let bytes = (0..<997).map { UInt8(($0 &* 37) & 0xFF) }
    checkEq(PhotoProtocol.crc32(Data(bytes)), PhotoProtocol.crc32(bytes), "Data/[UInt8] 重载一致")
    // 单字节差异必须改变 CRC（证明真的在算，不是常数）
    var other = bytes
    other[500] ^= 0x01
    check(PhotoProtocol.crc32(other) != PhotoProtocol.crc32(bytes), "改 1 字节 CRC 必须变")
}

// MARK: - PGJ

func testPGJLayout() {
    print("--- PGJ 头字节级布局（协议 §7）---")
    let file = PhotoProtocol.makePGJ(jpeg: fakeJPEG, width: 466, height: 300,
                                     quality: 80, createdUnix: 1_700_000_000)
    checkEq(file.count, 32 + fakeJPEG.count, "文件大小 = 32 + JPEG 载荷")

    let b = [UInt8](file.prefix(32))
    checkEq(String(decoding: file[0..<4], as: UTF8.self), "PGJ1", "magic")
    checkEq(Int(b[4]) | Int(b[5]) << 8, 466, "width u16 LE @4")
    checkEq(Int(b[6]) | Int(b[7]) << 8, 300, "height u16 LE @6")
    checkEq(Int(b[8]), 1, "version @8")
    checkEq(Int(b[9]), 1, "flags bit0=baseline @9")
    checkEq(Int(b[10]) | Int(b[11]) << 8, 80, "quality u16 LE @10")
    checkEq(PhotoProtocol.readU32(b, 12), UInt32(fakeJPEG.count), "jpeg_bytes u32 LE @12")
    checkEq(PhotoProtocol.readU32(b, 16), PhotoProtocol.fileCRC32(file), "crc32 字段 == 整文件 CRC")
    checkEq(PhotoProtocol.readU32(b, 20), UInt32(1_700_000_000), "created_unix @20")
    check((24..<32).allSatisfy { b[$0] == 0 }, "reserved 24..31 必须全 0")

    // 头 + 载荷原样拼回
    checkEq(Data(file[32...]), fakeJPEG, "载荷必须原样跟在头后面")
}

func testPGJCRCConvention() {
    print("--- PGJ CRC 口径（crc 字段自身按 0 计算）---")
    let file = PhotoProtocol.makePGJ(jpeg: fakeJPEG, width: 100, height: 100)
    // 把 crc 字段改成任意值，"排除字段"的 CRC 必须不变；"包含字段"的 CRC 必须变
    var mutated = file
    PhotoProtocol.writeU32(&mutated, at: 16, 0xDEAD_BEEF)
    checkEq(PhotoProtocol.fileCRC32(mutated), PhotoProtocol.fileCRC32(file),
            "fileCRC32 必须排除 crc 字段（否则两端口径永远对不上）")
    check(PhotoProtocol.crc32(mutated) != PhotoProtocol.crc32(file),
          "raw crc32 应包含 crc 字段（证明两种口径确实不同）")
    // 载荷改 1 字节必须校验失败
    var corrupt = file
    corrupt[0] = UInt8(ascii: "X")
    check(PhotoProtocol.validatePGJ(corrupt) != nil, "改 magic → 校验必须失败")
    var corruptPayload = file
    corruptPayload[40] ^= 0x01
    checkEq(PhotoProtocol.validatePGJ(corruptPayload), .badCRC(expected: PhotoProtocol.readU32([UInt8](file.prefix(32)), 16),
                                                               actual: PhotoProtocol.fileCRC32(corruptPayload)),
            "改载荷 → err(crc) 级别失败")
}

func testPGJParse() {
    print("--- PGJ 解析/校验 ---")
    let file = PhotoProtocol.makePGJ(jpeg: fakeJPEG, width: 466, height: 466,
                                     quality: 90, createdUnix: 123)
    guard let h = PhotoProtocol.parsePGJHeader(file) else {
        check(false, "合法文件应能解析出头部"); return
    }
    checkEq(h.width, 466, "解析 width")
    checkEq(h.height, 466, "解析 height")
    checkEq(h.version, 1, "解析 version")
    checkEq(h.quality, 90, "解析 quality")
    checkEq(h.jpegBytes, fakeJPEG.count, "解析 jpeg_bytes")
    checkEq(h.storedCRC, PhotoProtocol.fileCRC32(file), "解析 crc 字段")
    checkEq(h.createdUnix, 123, "解析 created_unix")
    check(h.isBaseline, "flags 应为 baseline")
    checkEq(h.totalBytes, 32 + fakeJPEG.count, "totalBytes 派生")
    check(PhotoProtocol.validatePGJ(file) == nil, "完整文件校验通过")
    // 只给前 32 字节的头也能解析（扫描清单用）
    check(PhotoProtocol.parsePGJHeader(Data(file.prefix(32))) != nil, "只给头也应能解析")
}

func testPGJRejectsBad() {
    print("--- PGJ 拒绝坏文件（不猜）---")
    let good = PhotoProtocol.makePGJ(jpeg: fakeJPEG, width: 466, height: 466)
    check(PhotoProtocol.parsePGJHeader(Data()) == nil, "空数据 → nil")
    checkEq(PhotoProtocol.diagnosePGJHeader(Data()), .tooShort(actual: 0), "空数据原因")
    check(PhotoProtocol.parsePGJHeader(Data(good.prefix(31))) == nil, "31 字节 → nil")

    var badMagic = good; badMagic[0] = 0x00
    check(PhotoProtocol.parsePGJHeader(badMagic) == nil, "坏 magic → nil")
    checkEq(PhotoProtocol.diagnosePGJHeader(badMagic), .badMagic, "坏 magic 原因")

    var badVersion = good; badVersion[8] = 2
    checkEq(PhotoProtocol.diagnosePGJHeader(badVersion), .badVersion(2), "版本不对")

    var notBaseline = good; notBaseline[9] = 0
    checkEq(PhotoProtocol.diagnosePGJHeader(notBaseline), .notBaseline(flags: 0), "flags bit0=0")

    var zeroW = good; zeroW[4] = 0; zeroW[5] = 0
    checkEq(PhotoProtocol.diagnosePGJHeader(zeroW), .badDimension(width: 0, height: 466), "宽 0")

    var hugeW = good; hugeW[4] = UInt8(467 & 0xFF); hugeW[5] = UInt8(467 >> 8)
    checkEq(PhotoProtocol.diagnosePGJHeader(hugeW), .badDimension(width: 467, height: 466), "宽 467")

    var badQ = good; badQ[10] = 0; badQ[11] = 0
    checkEq(PhotoProtocol.diagnosePGJHeader(badQ), .badQuality(0), "质量 0")

    var zeroJPEG = good; PhotoProtocol.writeU32(&zeroJPEG, at: 12, 0)
    checkEq(PhotoProtocol.diagnosePGJHeader(zeroJPEG), .badJPEGBytes(0), "jpeg_bytes 0")

    var hugeJPEG = good; PhotoProtocol.writeU32(&hugeJPEG, at: 12, 512 * 1024 + 1)
    checkEq(PhotoProtocol.diagnosePGJHeader(hugeJPEG), .badJPEGBytes(512 * 1024 + 1), "jpeg_bytes 超上限")

    // 截断一个字节：头合法但这与 jpeg_bytes 不符
    let truncated = Data(good.prefix(good.count - 1))
    check(PhotoProtocol.validatePGJ(truncated) != nil, "截断文件 → 校验失败")
    checkEq(PhotoProtocol.validatePGJ(truncated), .badLength(expected: good.count, actual: good.count - 1),
            "截断原因")
}

func testPGJClamp() {
    print("--- PGJ 构造参数夹取 ---")
    let low = PhotoProtocol.makePGJ(jpeg: fakeJPEG, width: 0, height: -5, quality: 5)
    checkEq(PhotoProtocol.parsePGJHeader(low)?.width, 1, "宽度 0 → 夹到 1")
    checkEq(PhotoProtocol.parsePGJHeader(low)?.height, 1, "高度 -5 → 夹到 1")
    checkEq(PhotoProtocol.parsePGJHeader(low)?.quality, 10, "质量 5 → 夹到 10")

    let high = PhotoProtocol.makePGJ(jpeg: fakeJPEG, width: 9999, height: 9999, quality: 999)
    checkEq(PhotoProtocol.parsePGJHeader(high)?.width, 466, "宽度 9999 → 夹到 466")
    checkEq(PhotoProtocol.parsePGJHeader(high)?.quality, 100, "质量 999 → 夹到 100")

    // 空载荷：能构造出来，但解析必然拒绝（jpeg_bytes == 0）
    let empty = PhotoProtocol.makePGJ(jpeg: Data(), width: 10, height: 10)
    checkEq(empty.count, 32, "空载荷只有头")
    check(PhotoProtocol.parsePGJHeader(empty) == nil, "空载荷文件必须被拒")
    checkEq(PhotoProtocol.fileCRC32(Data("abc".utf8)), PhotoProtocol.crc32(Data("abc".utf8)),
            "少于 32 B 时按原样算 CRC")
}

// MARK: - 命令

func testCommands() {
    print("--- PhotoCtl 命令编码（字段名/顺序逐字对齐协议 §4）---")
    let begin = PhotoCommand.begin(kind: .pgj, bytes: 45210, crc: 282_593_821,
                                   width: 466, height: 466, frames: 1, fps: 0)
    checkEq(begin.jsonString ?? "",
            "{\"t\":\"p\",\"op\":\"begin\",\"kind\":0,\"bytes\":45210,\"crc\":282593821,"
            + "\"w\":466,\"h\":466,\"frames\":1,\"fps\":0}",
            "begin JSON 必须与协议示例逐字一致")

    checkEq(PhotoCommand.end.jsonString, "{\"t\":\"p\",\"op\":\"end\"}", "end")
    checkEq(PhotoCommand.abort.jsonString, "{\"t\":\"p\",\"op\":\"abort\"}", "abort")
    checkEq(PhotoCommand.list.jsonString, "{\"t\":\"p\",\"op\":\"list\"}", "list")
    checkEq(PhotoCommand.bench.jsonString, "{\"t\":\"p\",\"op\":\"bench\"}", "bench")
    checkEq(PhotoCommand.del(id: "p00012.pgj").jsonString,
            "{\"t\":\"p\",\"op\":\"del\",\"id\":\"p00012.pgj\"}", "del")
    checkEq(PhotoCommand.play(index: 3).jsonString,
            "{\"t\":\"p\",\"op\":\"play\",\"i\":3}", "play")
    checkEq(PhotoCommand.set(slideshow: true, interval: 30, shift: true, screenOff: 1800).jsonString,
            "{\"t\":\"p\",\"op\":\"set\",\"slide\":1,\"intv\":30,\"shift\":1,\"soff_s\":1800}",
            "set 全字段")
    checkEq(PhotoCommand.set(slideshow: false, interval: nil, shift: nil, screenOff: nil).jsonString,
            "{\"t\":\"p\",\"op\":\"set\",\"slide\":0}", "set 只带一个字段")
    checkEq(PhotoCommand.set(slideshow: nil, interval: 15, shift: nil, screenOff: nil).jsonString,
            "{\"t\":\"p\",\"op\":\"set\",\"intv\":15}", "set 只带 intv")

    // 非法参数 → nil（不允许静默夹取）
    check(PhotoCommand.begin(kind: .pgj, bytes: 0, crc: 0, width: 10, height: 10, frames: 1, fps: 0)
            .encode() == nil, "bytes 0 → nil")
    check(PhotoCommand.begin(kind: .pgj, bytes: 100, crc: 0, width: 467, height: 10, frames: 1, fps: 0)
            .encode() == nil, "宽 467 → nil")
    check(PhotoCommand.begin(kind: .pgi, bytes: 100, crc: 0, width: 300, height: 300,
                             frames: 12, fps: 0).encode() == nil, "动图 fps 0 → nil")
    check(PhotoCommand.begin(kind: .pgj, bytes: 100, crc: 0, width: 10, height: 10,
                             frames: 0, fps: 0).encode() == nil, "frames 0 → nil")
    check(PhotoCommand.del(id: "abc.jpg").encode() == nil, "del 扩展名非法 → nil")
    check(PhotoCommand.del(id: "../etc/passwd").encode() == nil, "del 路径穿越 → nil")
    check(PhotoCommand.play(index: -1).encode() == nil, "play 负数 → nil")
    check(PhotoCommand.set(slideshow: nil, interval: 0, shift: nil, screenOff: nil).encode() == nil,
          "intv 0 → nil")
    check(PhotoCommand.set(slideshow: nil, interval: 3601, shift: nil, screenOff: nil).encode() == nil,
          "intv 3601 → nil")
    check(PhotoCommand.set(slideshow: nil, interval: nil, shift: nil, screenOff: -1).encode() == nil,
          "soff_s -1 → nil")
    check(PhotoCommand.set(slideshow: nil, interval: nil, shift: nil, screenOff: nil).encode() == nil,
          "set 一个字段都没有 → nil")

    // 所有合法命令都必须 ≤180 B（iOS 单次写/通知上限）
    let all: [PhotoCommand] = [
        begin,
        .end, .abort, .list, .bench,
        .del(id: "p00012.pgj"),
        .play(index: 127),
        .del(id: String(repeating: "a", count: 59) + ".pgj"), // 最长合法名 63
        .set(slideshow: true, interval: 3600, shift: true, screenOff: 86400),
        .begin(kind: .pgi, bytes: 2_097_152, crc: UInt32.max, width: 300, height: 300,
               frames: 12, fps: 8),
    ]
    for cmd in all {
        guard let data = cmd.encode() else {
            check(false, "\(cmd.op) 应能编码"); continue
        }
        check(data.count <= PhotoLimits.maxMessageBytes,
              "\(cmd.op) 必须 ≤180 B，实际 \(data.count) B")
        // 必须是合法 JSON（固件手写扫描器要求字段名精确，App 至少别发坏 JSON）
        check((try? JSONSerialization.jsonObject(with: data)) is [String: Any],
              "\(cmd.op) 必须是合法 JSON")
    }
    // 最长文件名的 del 也不能超（设备上限 63）
    check((PhotoCommand.del(id: String(repeating: "a", count: 59) + ".pgj").jsonString ?? "").count <= 180,
          "最长文件名 del ≤180 B")
}

func testCommandRoundTripParse() {
    print("--- 命令 JSON 可被通用解析（字段名检查）---")
    let data = PhotoCommand.begin(kind: .pgi, bytes: 12345, crc: 99, width: 300,
                                  height: 300, frames: 12, fps: 8).encode()!
    let root = try! JSONSerialization.jsonObject(with: data) as! [String: Any]
    checkEq(root["t"] as? String, "p", "t 字段")
    checkEq(root["op"] as? String, "begin", "op 字段")
    checkEq(root["kind"] as? Int, 1, "kind")
    checkEq(root["bytes"] as? Int, 12345, "bytes")
    checkEq(root["crc"] as? Int, 99, "crc")
    checkEq(root["w"] as? Int, 300, "w")
    checkEq(root["h"] as? Int, 300, "h")
    checkEq(root["frames"] as? Int, 12, "frames")
    checkEq(root["fps"] as? Int, 8, "fps")
}

// MARK: - 事件

func testEvents() {
    print("--- PhotoEvt 事件解析（协议 §5）---")
    guard case let .event(.go(off, chunk, win, id))? =
            PhotoProtocol.parseIncoming(Data("""
            {"t":"pe","ev":"go","off":0,"chunk":178,"win":4096,"id":"p00012.pgj"}
            """.utf8)) else {
        check(false, "go 应能解析"); return
    }
    checkEq(off, 0, "go.off")
    checkEq(chunk, 178, "go.chunk")
    checkEq(win, 4096, "go.win")
    checkEq(id ?? "", "p00012.pgj", "go.id")

    // 续传：off > 0
    if case let .event(.go(off2, _, _, _))? =
        PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"go\",\"off\":4096}".utf8)) {
        checkEq(off2, 4096, "续传 go.off")
    } else { check(false, "无 id 的 go 应能解析") }

    // credit
    guard case let .event(.credit(off3))? =
            PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"credit\",\"off\":4096}".utf8)) else {
        check(false, "credit 应能解析"); return
    }
    checkEq(off3, 4096, "credit.off")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"credit\"}".utf8)) == nil,
          "credit 缺 off → nil")

    // done（id 可选）
    checkEq(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"done\",\"id\":\"p00012.pgj\"}".utf8)),
            .event(.done(id: "p00012.pgj")), "done 带 id")
    checkEq(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"done\"}".utf8)),
            .event(.done(id: nil)), "done 不带 id")

    // err：已知 code + want
    checkEq(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"err\",\"code\":\"crc\",\"want\":0}".utf8)),
            .event(.error(code: .crc, rawCode: "crc", want: 0)), "err crc")
    checkEq(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"err\",\"code\":\"offset\",\"want\":1000}".utf8)),
            .event(.error(code: .offset, rawCode: "offset", want: 1000)), "err offset")
    // err：未知 code → unknown，但保留原文
    checkEq(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"err\",\"code\":\"zombie\"}".utf8)),
            .event(.error(code: .unknown, rawCode: "zombie", want: nil)), "err 未知 code")
    // 所有错误码
    for raw in ["busy", "no_space", "too_large", "unsupported", "crc", "offset", "io", "not_ready", "bad_arg"] {
        if case let .event(.error(code, _, _))? =
            PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"err\",\"code\":\"\(raw)\"}".utf8)) {
            checkEq(code.rawValue, raw, "错误码 \(raw) 应被识别")
        } else {
            check(false, "错误码 \(raw) 解析失败")
        }
    }

    checkEq(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"changed\"}".utf8)),
            .event(.changed), "changed")
    checkEq(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"info\"}".utf8)),
            .event(.info), "info")

    // 坏输入一律 nil（不猜）
    check(PhotoProtocol.parseIncoming(Data("not json".utf8)) == nil, "坏 JSON → nil")
    check(PhotoProtocol.parseIncoming(Data("{}".utf8)) == nil, "缺 t → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\"}".utf8)) == nil, "缺 ev → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pe\",\"ev\":\"whatever\"}".utf8)) == nil,
          "未知 ev → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pi\",\"v\":1}".utf8)) == nil,
          "pi 是 READ 不是 Evt → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"ps\",\"n\":1}".utf8)) == nil,
          "ps 是 READ 不是 Evt → nil")
}

func testListEvents() {
    print("--- 清单分页 pq/pf/pz ---")
    checkEq(PhotoProtocol.parseIncoming(Data("{\"t\":\"pq\",\"n\":12}".utf8)),
            .listBegin(count: 12), "pq")
    checkEq(PhotoProtocol.parseIncoming(Data("{\"t\":\"pz\"}".utf8)), .listEnd, "pz")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pq\"}".utf8)) == nil, "pq 缺 n → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pq\",\"n\":-1}".utf8)) == nil, "pq n 负数 → nil")

    guard case let .listItem(item)? = PhotoProtocol.parseIncoming(Data(itemJSON.utf8)) else {
        check(false, "pf 应能解析"); return
    }
    checkEq(item.index, 0, "pf.i")
    checkEq(item.id, "p00012.pgj", "pf.id")
    checkEq(item.bytes, 45210, "pf.b")
    checkEq(item.frames, 1, "pf.f")
    checkEq(item.kind, 0, "pf.k")
    checkEq(item.width, 466, "pf.w")
    checkEq(item.height, 466, "pf.h")
    checkEq(item.fps, 0, "pf.fps")
    check(!item.isAnimated, "帧数 1 → 静态")
    checkEq(item.kindValue, .pgj, "kindValue")
    checkEq(item.kilobytes, 45, "45210 B ≈ 45 KB（向上取整）")
    checkEq(item.summaryText, "静态照片 · 45 KB", "静态项摘要")

    // 动图项
    let animJSON = "{\"t\":\"pf\",\"i\":1,\"id\":\"p00013.pgi\",\"b\":218220,\"f\":12,\"k\":1,\"w\":300,\"h\":300,\"fps\":8}"
    guard case let .listItem(anim)? = PhotoProtocol.parseIncoming(Data(animJSON.utf8)) else {
        check(false, "动图 pf 应能解析"); return
    }
    check(anim.isAnimated, "帧数 12 → 动图")
    checkEq(anim.kindValue, .pgi, "kindValue pgi")
    checkEq(anim.summaryText, "动图 · 12 帧 · 8 fps · 214 KB", "动图项摘要")

    // 坏项：不猜
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pf\",\"i\":0}".utf8)) == nil, "pf 缺 id → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pf\",\"id\":\"p1.pgj\"}".utf8)) == nil, "pf 缺 i → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pf\",\"i\":-1,\"id\":\"p1.pgj\"}".utf8)) == nil,
          "pf i 负数 → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pf\",\"i\":0,\"id\":\"p1.jpg\"}".utf8)) == nil,
          "pf 扩展名非法 → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pf\",\"i\":0,\"id\":\"../p1.pgj\"}".utf8)) == nil,
          "pf 路径穿越 → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pf\",\"i\":0,\"id\":\"p1.pgj\",\"k\":2}".utf8)) == nil,
          "pf k 非法 → nil")
    check(PhotoProtocol.parseIncoming(Data("{\"t\":\"pf\",\"i\":0,\"id\":\"p1.pgj\",\"k\":\"x\"}".utf8)) == nil,
          "pf k 非数字 → nil")
    // 缺可选数值字段：合理默认
    guard case let .listItem(sparse)? =
            PhotoProtocol.parseIncoming(Data("{\"t\":\"pf\",\"i\":2,\"id\":\"p00002.pgj\"}".utf8)) else {
        check(false, "缺数值字段的 pf 应能解析"); return
    }
    checkEq(sparse.frames, 1, "缺 f → 默认 1 帧")
    checkEq(sparse.bytes, 0, "缺 b → 0")
    checkEq(sparse.kind, 0, "缺 k → 0（PGJ）")
    checkEq(sparse.kindValue, .pgj, "缺 k 的 kindValue")
}

// MARK: - Info

func testInfoParse() {
    print("--- PhotoInfo 解析（能力/档位）---")
    guard let info = DevicePhotoParser.parseInfo(Data(infoJSON.utf8)) else {
        check(false, "协议示例应能解析"); return
    }
    checkEq(info.version, 1, "version")
    checkEq(info.tier, 1, "tier")
    checkEq(info.capability, 7, "cap")
    checkEq(info.maxPixel, 466, "px")
    checkEq(info.maxBytes, 512, "max_kb")
    checkEq(info.animPixel, 300, "anim.px")
    checkEq(info.animFPS, 8, "anim.fps")
    checkEq(info.animFrames, 12, "anim.frames")
    checkEq(info.animMaxBytes, 2048, "anim.max_kb")
    checkEq(info.animFPSMeasured, 4, "anim.fps_meas")
    checkEq(info.benchDecodeMs, 230, "bench.dec_ms")
    check(info.missing.isEmpty, "完整上报不应有 missing，实际 \(info.missing)")
    checkEq(info.maxFileBytes, 512 * 1024, "maxFileBytes 换算")
    checkEq(info.animMaxFileBytes, 2048 * 1024, "animMaxFileBytes 换算")

    // 协议示例：cap 支持动图 + tier=1 + fps_meas=4 → 动图不可用（设备自己降档）
    check(!info.supportsAnimation, "协议示例应判定动图不可用")
    check(info.supportsAnimated, "cap bit1 = 1（固件支持动图）")
    check(info.supportsStaticPhoto, "cap bit0 = 1")
    check(info.supportsDelete, "cap bit2 = 1")
    check(!info.supportsInternalStorage, "cap bit3 = 0")
    check(!info.isFullTier, "tier 1 → 非 FULL")
    check(info.animationUnavailableReason?.contains("完整档位") == true,
          "降档原因应说明档位，实际：\(info.animationUnavailableReason ?? "nil")")
}

func testAnimationGating() {
    print("--- 动图门控（①固件能力 + ②设备实测，三层里的前两层）---")
    func info(cap: Int, tier: Int, fpsMeasured: Int) -> DevicePhotoInfo {
        var i = DevicePhotoInfo()
        i.capability = cap
        i.tier = tier
        i.animFPSMeasured = fpsMeasured
        i.missing = []
        return i
    }
    check(!info(cap: 7, tier: 2, fpsMeasured: 7).supportsAnimation, "fps_meas 7 → 不可用")
    check(info(cap: 7, tier: 2, fpsMeasured: 8).supportsAnimation, "fps_meas 8 → 可用")
    check(info(cap: 7, tier: 2, fpsMeasured: 12).supportsAnimation, "fps_meas 12 → 可用")
    check(!info(cap: 7, tier: 1, fpsMeasured: 12).supportsAnimation, "tier 1 → 不可用")
    check(!info(cap: 5, tier: 2, fpsMeasured: 12).supportsAnimation, "cap bit1=0 → 不可用")
    check(!info(cap: 7, tier: 2, fpsMeasured: 0).supportsAnimation, "fps_meas 0（没播过）→ 不可用")

    check(info(cap: 5, tier: 2, fpsMeasured: 12).animationUnavailableReason?.contains("固件不支持动图") == true,
          "缺能力位的原因")
    check(info(cap: 7, tier: 1, fpsMeasured: 12).animationUnavailableReason?.contains("完整档位") == true,
          "档位原因")
    check(info(cap: 7, tier: 2, fpsMeasured: 0).animationUnavailableReason?.contains("还没播过") == true,
          "未实测原因")
    check(info(cap: 7, tier: 2, fpsMeasured: 3).animationUnavailableReason?.contains("3 fps") == true,
          "帧率不足原因要带数字")
    check(info(cap: 7, tier: 2, fpsMeasured: 8).animationUnavailableReason == nil,
          "可用时不应有原因")
}

func testInfoMissingVsZero() {
    print("--- PhotoInfo 缺字段 vs 真的是 0 ---")
    // 设备只报了协议版本：其余 10 个字段必须进 missing，值保持 0
    guard let sparse = DevicePhotoParser.parseInfo(Data("{\"t\":\"pi\",\"v\":1}".utf8)) else {
        check(false, "最小合法上报应能解析"); return
    }
    checkEq(sparse.version, 1, "version")
    checkEq(sparse.missing.count, 10, "其余 10 个字段应记为 missing，实际 \(sparse.missing)")
    check(!sparse.reported(.capability), "cap 未上报")
    check(sparse.reported(.version), "v 已上报")
    check(!sparse.supportsAnimation, "缺能力位 → 动图不可用")
    check(sparse.capability == 0, "缺 cap → 0（默认值）")

    // 设备明确上报 0：不能进 missing（这就是"没上报 vs 真的是 0"）
    guard let zeros = DevicePhotoParser.parseInfo(Data("""
    {"t":"pi","v":1,"tier":1,"cap":0,"px":0,"max_kb":0,\
    "anim":{"px":0,"fps":0,"frames":0,"max_kb":0,"fps_meas":0},"bench":{"dec_ms":0}}
    """.utf8)) else {
        check(false, "全 0 上报应能解析"); return
    }
    check(zeros.missing.isEmpty, "全 0 上报必须没有 missing，实际 \(zeros.missing)")
    check(zeros.reported(.animFPSMeasured), "上报了 0 也算已上报")
    check(!zeros.supportsAnimation, "0 能力位 → 动图不可用")
    check(zeros.animationUnavailableReason?.contains("固件不支持动图") == true, "0 能力位原因")

    // anim / bench 子对象整个缺失
    guard let noAnim = DevicePhotoParser.parseInfo(Data("{\"t\":\"pi\",\"v\":1,\"cap\":7}".utf8)) else {
        check(false, "缺 anim 应能解析"); return
    }
    checkEq(noAnim.missing.count, 9, "缺 anim+bench 的 missing 数，实际 \(noAnim.missing)")
    check(!noAnim.reported(.animFPSMeasured), "fps_meas 未上报")

    // 非法/坏 JSON → nil
    check(DevicePhotoParser.parseInfo(Data("nope".utf8)) == nil, "坏 JSON → nil")
    check(DevicePhotoParser.parseInfo(Data("{\"t\":\"ps\"}".utf8)) == nil, "错 t → nil")
    check(DevicePhotoParser.parseInfo(Data("[]".utf8)) == nil, "数组根 → nil")
    check(DevicePhotoParser.parseInfo(Data("{\"t\":\"pi\",\"v\":\"1\"}".utf8)) == nil
          || DevicePhotoParser.parseInfo(Data("{\"t\":\"pi\",\"v\":\"1\"}".utf8))?.missing.contains("v") == true,
          "字符串数字不应被当成 Int（或至少记 missing）")
    // 小数/负值容错
    guard let weird = DevicePhotoParser.parseInfo(Data("{\"t\":\"pi\",\"v\":1,\"tier\":-2,\"px\":466.0}".utf8)) else {
        check(false, "容错解析失败"); return
    }
    checkEq(weird.tier, 0, "负 tier → 夹到 0")
    checkEq(weird.maxPixel, 466, "466.0 应能读成 466")
}

// MARK: - State

func testStateParse() {
    print("--- PhotoState 解析（状态/设置）---")
    guard let s = DevicePhotoParser.parseState(Data(stateJSON.utf8)) else {
        check(false, "协议示例应能解析"); return
    }
    checkEq(s.currentIndex, 3, "cur")
    checkEq(s.count, 12, "n")
    checkEq(s.storage, 1, "stor")
    checkEq(s.storageKind, .sd, "stor 1 → SD")
    check(s.isStorageReady, "SD 就绪")
    checkEq(s.freeKiB, 5230, "free_kb")
    checkEq(s.freeBytes, 5230 * 1024, "freeBytes 换算")
    check(s.slideshow, "slide 1 → true")
    checkEq(s.intervalSeconds, 30, "intv")
    check(s.shift, "shift 1 → true")
    checkEq(s.screenOffSeconds, 1800, "soff_s")
    check(!s.receiving, "rx 0 → false")
    check(s.missing.isEmpty, "完整上报无 missing，实际 \(s.missing)")

    // 存储介质三态
    checkEq(PhotoStorage(raw: 0), .unknown, "stor 0")
    checkEq(PhotoStorage(raw: 2), .internalFlash, "stor 2")
    checkEq(PhotoStorage(raw: 9), .unknown, "stor 9（未知）")
    checkEq(PhotoStorage(raw: 1).displayName, "SD 卡", "存储显示名")

    // n == 0 时设备给 cur = -1
    guard let empty = DevicePhotoParser.parseState(Data("{\"t\":\"ps\",\"cur\":-1,\"n\":0}".utf8)) else {
        check(false, "空相册应能解析"); return
    }
    checkEq(empty.currentIndex, -1, "空相册 cur=-1 必须保留")
    checkEq(empty.count, 0, "空相册 n=0")

    // 0/1 与 true/false 都接受
    guard let boolForm = DevicePhotoParser.parseState(Data("""
    {"t":"ps","slide":true,"shift":false,"rx":true,"n":2,"cur":0}
    """.utf8)) else {
        check(false, "布尔形式应能解析"); return
    }
    check(boolForm.slideshow, "slide true")
    check(!boolForm.shift, "shift false")
    check(boolForm.receiving, "rx true")

    // 缺字段
    guard let sparse = DevicePhotoParser.parseState(Data("{\"t\":\"ps\",\"n\":5}".utf8)) else {
        check(false, "最小上报应能解析"); return
    }
    checkEq(sparse.count, 5, "n")
    checkEq(sparse.missing.count, 8, "其余 8 个字段 missing，实际 \(sparse.missing)")
    check(sparse.reported(.count), "n 已上报")
    check(!sparse.reported(.slideshow), "slide 未上报")
    checkEq(sparse.storageKind, .unknown, "缺 stor → unknown（而非 SD）")

    // 坏输入
    check(DevicePhotoParser.parseState(Data("{}".utf8)) == nil, "缺 t → nil")
    check(DevicePhotoParser.parseState(Data("{\"t\":\"pi\"}".utf8)) == nil, "错 t → nil")
    check(DevicePhotoParser.parseState(Data("oops".utf8)) == nil, "坏 JSON → nil")

    // 负值夹取
    guard let neg = DevicePhotoParser.parseState(Data("{\"t\":\"ps\",\"n\":-3,\"free_kb\":-1}".utf8)) else {
        check(false, "负值应能解析"); return
    }
    checkEq(neg.count, 0, "负 n 夹到 0")
    checkEq(neg.freeKiB, 0, "负 free_kb 夹到 0")
}

// MARK: - 照片库

func testLibrary() {
    print("--- DevicePhotoLibrary 派生 ---")
    var lib = DevicePhotoLibrary()
    checkEq(lib.count, 0, "默认空")
    check(lib.isEmpty, "默认 isEmpty")
    check(!lib.canDelete, "默认不能删除")
    check(!lib.canAnimate, "默认不能动图")
    check(lib.currentPhoto == nil, "默认没有当前项")
    check(!lib.hasDeviceReport, "默认没有设备上报")

    lib.info = DevicePhotoParser.parseInfo(Data(infoJSON.utf8))!
    lib.state = DevicePhotoParser.parseState(Data(stateJSON.utf8))!
    check(lib.hasDeviceReport, "解析后应有设备上报")
    lib.items = [
        DevicePhoto(index: 0, id: "p00000.pgj", bytes: 40_000, frames: 1, kind: 0,
                    width: 466, height: 466, fps: 0),
        DevicePhoto(index: 3, id: "p00003.pgj", bytes: 50_000, frames: 1, kind: 0,
                    width: 466, height: 466, fps: 0),
    ]
    checkEq(lib.count, 2, "count")
    check(!lib.isEmpty, "非空")
    check(lib.canDelete, "cap bit2=1 且有照片 → 可删除")
    check(!lib.canAnimate, "LITE 档 → 不可动图")
    checkEq(lib.currentPhoto?.id ?? "", "p00003.pgj", "currentPhoto 按 state.cur=3 匹配")

    lib.state.currentIndex = 99
    check(lib.currentPhoto == nil, "下标越界不猜")

    lib.info.capability = 7
    lib.info.tier = 2
    lib.info.animFPSMeasured = 8
    check(lib.canAnimate, "FULL 档且实测够 → 可动图")
}

// MARK: - 上传模型

func testUploadModels() {
    print("--- 上传请求/进度模型 ---")
    var req = PhotoUploadRequest(id: "A1",
                                 fileURL: URL(fileURLWithPath: "/tmp/photo-upload/A1.pgj"),
                                 kind: .pgj,
                                 bytes: 100_000,
                                 crc32: 12345,
                                 width: 466,
                                 height: 466,
                                 frames: 1,
                                 fps: 0)
    checkEq(req.displayTitle, "A1.pgj", "无 displayName 时回退文件名")
    req.displayName = "IMG_1234"
    checkEq(req.displayTitle, "IMG_1234", "有 displayName 时用它")

    var p = PhotoUploadProgress(request: req)
    checkEq(p.id, "A1", "进度 id")
    checkEq(p.state, .queued, "初始 queued")
    checkEq(p.totalBytes, 100_000, "totalBytes")
    checkEq(p.fraction, 0, "初始进度 0")
    checkEq(p.displayName ?? "", "IMG_1234", "displayName")
    check(p.etaSeconds == nil, "排队中没有 ETA")
    check(!p.isFinished, "排队不是终态")
    check(!p.isActive, "排队不占通道")

    // 进度 = acked / total（不是 sent！）
    p.state = .uploading
    p.sentBytes = 40_000
    p.ackedBytes = 20_000
    checkEq(p.fraction, 0.2, "进度按 acked 算")
    checkEq(p.remainingBytes, 80_000, "剩余字节")
    // 速率未知时按 8 KB/s 兜底估 ETA
    let eta = p.etaSeconds ?? 0
    check(abs(eta - 80_000.0 / 8192.0) < 0.001, "速率未知按 8 KB/s 估 ETA，实际 \(eta)")
    p.bytesPerSecond = 25_000
    let eta2 = p.etaSeconds ?? 0
    check(abs(eta2 - 80_000.0 / 25_000.0) < 0.001, "按实测速率算 ETA，实际 \(eta2)")
    checkEq(p.stateText, "正在上传 20%", "进行中文案")

    p.state = .verifying
    check(p.isActive, "校验中占通道")
    check(p.etaSeconds != nil, "校验中仍有 ETA")
    p.state = .done
    p.ackedBytes = 100_000
    checkEq(p.fraction, 1, "完成后 100%")
    check(p.isFinished, "完成是终态")
    check(p.etaSeconds == nil, "完成后无 ETA")
    checkEq(p.stateText, "已完成", "完成文案")

    // 7 个状态的性质矩阵（UI 会 switch 全部 case）
    let states: [PhotoUploadState] = [.queued, .uploading, .verifying, .done, .failed, .cancelled, .waitingReconnect]
    checkEq(states.count, 7, "状态数量")
    let finished = states.filter { $0.isFinished }.map(\.stateText)
    checkEq(finished.count, 3, "终态应该是 3 个（done/failed/cancelled），实际 \(finished)")
    let active = states.filter { $0.isActive }.map(\.stateText)
    checkEq(active.count, 3, "进行中应该是 3 个（uploading/verifying/waitingReconnect），实际 \(active)")
    let retryable = states.filter { $0.canRetry }.map(\.stateText)
    checkEq(retryable.count, 2, "可重试应该是 2 个（failed/cancelled），实际 \(retryable)")
    // 每个状态都必须有非空文案
    for s in states { check(!s.stateText.isEmpty, "\(s) 应有文案") }

    // 失败态带文案
    var failed = PhotoUploadProgress(request: req)
    failed.state = .failed
    failed.errorText = "仪表存储空间不足"
    checkEq(failed.stateText, "仪表存储空间不足", "失败时直接显示原因")

    // 空文件不让除零
    var zero = PhotoUploadProgress(id: "Z")
    checkEq(zero.fraction, 0, "total=0 时进度 0")
    zero.state = .done
    checkEq(zero.fraction, 1, "total=0 且完成 → 1")
}

// MARK: - 文件名 / 分片帧 / 工具

func testPhotoName() {
    print("--- 文件名白名单（与 photo_list.h 一致）---")
    for name in ["p00012.pgj", "p00012.pgi", "P00012.PGJ", "a_b-c.d.pgj", "x.pgi"] {
        check(PhotoProtocol.isValidPhotoName(name), "\(name) 应该合法")
    }
    for name in ["", ".pgj", "p1.jpg", "p1.jpeg", ".hidden.pgj", "a..b.pgj",
                 "a/b.pgj", "a b.pgj", "中文.pgj", "p1.pgj ",
                 String(repeating: "a", count: 60) + ".pgj"] {
        check(!PhotoProtocol.isValidPhotoName(name), "\(name.prefix(20))… 应该非法")
    }
    check(PhotoProtocol.isValidPhotoName(String(repeating: "a", count: 59) + ".pgj"),
          "59+4=63 字符应合法（边界）")
}

func testChunkFrame() {
    print("--- 分片帧 [u32 offset LE][payload] ---")
    let payload = Data([0xAA, 0xBB, 0xCC])
    let frame = PhotoProtocol.makeChunkFrame(offset: 0x0102_0304, payload: payload)
    checkEq(frame.count, 4 + payload.count, "帧长 = 4 + payload")
    checkEq([UInt8](frame.prefix(4)), [0x04, 0x03, 0x02, 0x01], "offset 小端")
    checkEq(Data(frame.dropFirst(4)), payload, "payload 原样")

    guard let parsed = PhotoProtocol.parseChunkFrame(frame) else {
        check(false, "分片帧应能解析"); return
    }
    checkEq(parsed.offset, 0x0102_0304, "解析 offset")
    checkEq(parsed.payload, payload, "解析 payload")
    check(PhotoProtocol.parseChunkFrame(Data([1, 2, 3])) == nil, "<4 B → nil")
    checkEq(PhotoProtocol.parseChunkFrame(Data([9, 0, 0, 0]))?.payload ?? Data(), Data(), "空 payload")
}

func testIntValueHelper() {
    print("--- JSON 数值容错 ---")
    checkEq(PhotoProtocol.intValue(42), 42, "Int")
    checkEq(PhotoProtocol.intValue(true), 1, "Bool true")
    checkEq(PhotoProtocol.intValue(false), 0, "Bool false")
    checkEq(PhotoProtocol.intValue(8.0), 8, "Double")
    check(PhotoProtocol.intValue("8") == nil, "字符串不猜")
    check(PhotoProtocol.intValue(nil) == nil, "nil")
}

func testKindsAndLimits() {
    print("--- 类型/常量 ---")
    checkEq(PhotoKind.pgj.rawValue, 0, "PGJ kind = 0")
    checkEq(PhotoKind.pgi.rawValue, 1, "PGI kind = 1")
    checkEq(PhotoKind.pgj.fileExtension, ".pgj", "扩展名")
    checkEq(PhotoKind.pgi.displayName, "动图", "类型名")
    checkEq(PhotoGATT.allCharacteristics.count, 5, "照片服务 5 个特征")
    checkEq(PhotoGATT.service, "7D2A0010-8F4A-4B7C-9D21-5E6A3B8C1000", "服务 UUID")
    checkEq(PhotoGATT.ctl, "7D2A0011-8F4A-4B7C-9D21-5E6A3B8C1000", "Ctl UUID")
    checkEq(PhotoGATT.data, "7D2A0012-8F4A-4B7C-9D21-5E6A3B8C1000", "Data UUID")
    checkEq(PhotoGATT.evt, "7D2A0013-8F4A-4B7C-9D21-5E6A3B8C1000", "Evt UUID")
    checkEq(PhotoGATT.info, "7D2A0014-8F4A-4B7C-9D21-5E6A3B8C1000", "Info UUID")
    checkEq(PhotoGATT.state, "7D2A0015-8F4A-4B7C-9D21-5E6A3B8C1000", "State UUID")
    checkEq(PhotoLimits.maxMessageBytes, 180, "单条 JSON 上限 180 B")
    checkEq(PhotoLimits.pgjHeaderBytes, 32, "PGJ 头 32 B")
    checkEq(PhotoLimits.pgjMaxDim, 466, "最大边长 466")
    checkEq(PhotoLimits.creditTimeout, 6, "credit 看门狗 6 s")
}

// MARK: - 入口

@main
struct TestPhotoModels {
    static func main() {
        print("=== 照片协议模型层 ===")
        testCRC32()
        testPGJLayout()
        testPGJCRCConvention()
        testPGJParse()
        testPGJRejectsBad()
        testPGJClamp()
        testCommands()
        testCommandRoundTripParse()
        testEvents()
        testListEvents()
        testInfoParse()
        testAnimationGating()
        testInfoMissingVsZero()
        testStateParse()
        testLibrary()
        testUploadModels()
        testPhotoName()
        testChunkFrame()
        testIntValueHelper()
        testKindsAndLimits()
        print("结果: \(gPass) 通过, \(gFail) 失败")
        exit(gFail == 0 ? 0 : 1)
    }
}
