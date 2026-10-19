//
//  PhotoTabView.swift
//  CarDashboardPhone
//
//  「照片」页：选图 / 选视频 → 圆屏裁切预览 → 导出 GIF → 存相册。
//
//  这是**纯 App 侧**的一页，不依赖设备连接 —— 先把素材做出来看到效果，
//  再谈怎么传到设备（`.pgi` 打包 + Wi-Fi 上传在 P5）。
//

import AVFoundation
import NetworkExtension
import PhotosUI
import SwiftUI

struct PhotoTabView: View {
    /// 用来发设备控制命令（轮播开关）；不连设备时也能正常用（只是命令发不出去）
    let session: DriveSessionManager
    /// 导出尺寸：设备屏幕直径（圆屏最终会裁掉四角）
    @State private var size: Int = 466

    @State private var pickedItem: PhotosPickerItem?
    @State private var preview: UIImage?
    @State private var gifData: Data?
    @State private var frameCount = 0
    @State private var status = ""
    @State private var busy = false
    @State private var showError = false
    @State private var errorText = ""

    /// 视频转 GIF 的目标帧率
    @State private var fps: Double = 12

    /// 手势变换：捏合缩放 + 拖动选取。
    ///
    /// `zoom` / `pan` 是**实时**值（跟手），`committed*` 是**手势结束时**的基准，
    /// 两者相加得到当前变换。这样捏合过程中能即时预览，松手后才重新裁切。
    @State private var zoom: CGFloat = 1
    @State private var pan: CGSize = .zero
    @State private var committedZoom: CGFloat = 1
    @State private var committedPan: CGSize = .zero
    /// 已提交的旋转角（度）；手势期间是"基准 + 实时增量"
    @State private var committedRotation: CGFloat = 0

    /// 未经过手势的原始正方形帧（换素材时填；改手势时在此基础上重新裁切）
    @State private var rawFrames: [StudioFrame] = []
    /// 视频自动降帧率的提示（跨 await 传递用 static，SwiftUI 里 @State 不方便从非主线程写）
    private static var pendingFPSNote: String?
    /// 自动连热点的结果（失败也不阻塞上传，但失败时要说清楚）
    private static var lastJoinError: String?

    /*
     * 设备的 AP 模式是**固定 IP**：ESP32 softAP 的网关恒为 192.168.4.1，
     * 所以这里不需要用户填、也不需要 BLE 传 IP。
     */
    private static let deviceAPHost = "192.168.4.1"
    private static let deviceAPSSID = "CarDash"
    /* 热点是**免密**的（见固件 WIFI_AP_OPEN），所以这里不需要密码 */

    @State private var deviceHost: String = PhotoTabView.deviceAPHost
    @State private var uploadStatus = ""
    @State private var uploading = false
    @State private var pgiBytes = 0
    /// 设备端轮播开关（与固件 photo_slide 命令对应）
    @State private var slideshowOn = true

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    PhotosPicker(selection: $pickedItem,
                                 matching: .any(of: [.images, .videos]),
                                 photoLibrary: .shared()) {
                        Label("选择图片或视频", systemImage: "photo.on.rectangle.angled")
                    }
                    .disabled(busy)
                } footer: {
                    Text("支持：单张图片、GIF/动图、视频（转 GIF）。\n裁切后为正方形，设备圆屏会再裁掉四角。")
                }

                // ── 预览（圆屏遮罩，所见即所得）──
                Section("预览") {
                    GeometryReader { geo in
                        let side = min(geo.size.width, geo.size.height)
                        ZStack {
                            Color.black
                            if let preview {
                                /*
                                 * 预览 = 裁好的正方形 + **与导出一致的变换**。
                                 * 顺序很重要：先 scale 再 offset（与 squareCropped 里的
                                 * "先绕中心放大、再按比例平移"是同一套语义）。
                                 */
                                Image(uiImage: preview)
                                    .resizable()
                                    .scaledToFill()
                                    .frame(width: side, height: side)
                                    /*
                                     * 注意：旋转**只影响预览的示意** ——
                                     * 真正参与导出的是 squareCropped 里的旋转，
                                     * 两者角度必须来自同一个 committedRotation。
                                     */
                                    .rotationEffect(.degrees(committedRotation))
                                    .scaleEffect(zoom)
                                    .offset(x: pan.width + committedPan.width,
                                            y: pan.height + committedPan.height)
                            } else {
                                Text("还没有素材")
                                    .foregroundStyle(.secondary)
                            }
                        }
                        .frame(width: side, height: side)
                        // 圆形遮罩：与设备圆屏一致，也是"最终会显示的区域"
                        .clipShape(Circle())
                        .overlay(
                            Circle()
                                .strokeBorder(.white.opacity(0.18), lineWidth: 1)
                        )
                        .contentShape(Circle())
                        .gesture(
                            SimultaneousGesture(
                                SimultaneousGesture(magnifyGesture(side: side),
                                                    dragGesture(side: side)),
                                rotateGesture()
                            )
                        )
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                    .frame(height: 300)
                    .padding(.vertical, 6)

                    if preview != nil {
                        HStack {
                            Text("缩放 \(String(format: "%.1f", zoom))×")
                                .font(.caption).monospacedDigit()
                            if committedRotation != 0 {
                                Text("\(Int(committedRotation))°")
                                    .font(.caption).monospacedDigit()
                            }
                            Spacer()
                            Spacer()
                            Button("复位") { resetTransform() }
                                .font(.caption)
                                .buttonStyle(.borderless)
                        }
                        .foregroundStyle(.secondary)
                    }

                    if let note = Self.pendingFPSNote {
                        Text(note)
                            .font(.caption2)
                            .foregroundStyle(.orange)
                    }
                    if !status.isEmpty {
                        Text(status)
                            .font(.footnote)
                            .foregroundStyle(busy ? .orange : .secondary)
                    }
                }

                Section {
                    Toggle("照片表盘轮播", isOn: Binding(
                        get: { slideshowOn },
                        set: { v in
                            slideshowOn = v
                            // 设备端命令：{"cmd":"photo_slide","value":0|1}
                            _ = session.ble.sendCommand("{\"cmd\":\"photo_slide\",\"value\":\(v ? 1 : 0)}")
                        }
                    ))
                } header: {
                    Text("仪表设置")
                } footer: {
                    Text("关闭后仪表会停在当前那张照片（此时自动开启像素位移防烧屏）。\n需要先连上仪表。")
                }

                Section("参数") {
                    Picker("旋转角度", selection: Binding(
                        get: { Int(committedRotation) % 360 },
                        set: { newValue in
                            committedRotation = CGFloat(newValue)
                            normalizeRotation()
                            reapplyTransform()
                        }
                    )) {
                        Text("0°").tag(0)
                        Text("90°").tag(90)
                        Text("180°").tag(180)
                        Text("270°").tag(270)
                    }
                    .pickerStyle(.segmented)
                    .disabled(rawFrames.isEmpty)

                    Stepper("边长：\(size) px", value: $size, in: 240...466, step: 2)
                    HStack {
                        Text("视频帧率")
                        Spacer()
                        Text("\(Int(fps)) fps")
                            .foregroundStyle(.secondary)
                        Slider(value: $fps, in: 4...20, step: 1)
                            .frame(width: 150)
                    }
                }

                Section {
                    Button {
                        Task { await exportGIF() }
                    } label: {
                        Label("导出 GIF 到相册", systemImage: "square.and.arrow.down")
                    }
                    .disabled(gifData == nil || busy)

                    if let gifData {
                        Label("已生成 \(frameCount) 帧 · \(gifData.count / 1024) KB",
                              systemImage: "checkmark.circle")
                            .font(.footnote)
                            .foregroundStyle(.green)
                    }
                } footer: {
                    Text("导出的 GIF 会存进系统相册，可直接查看播放效果。")
                }

                // ── 上传到仪表 ──
                Section {
                    HStack {
                        Text("仪表 IP")
                        TextField("192.168.1.23", text: $deviceHost)
                            .keyboardType(.decimalPad)
                            .multilineTextAlignment(.trailing)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .onChange(of: deviceHost) { _, v in
                                UserDefaults.standard.set(v, forKey: "photoDeviceHost")
                            }
                    }
                    Button {
                        Task { await upload() }
                    } label: {
                        Label(uploading ? "正在上传…" : "连接仪表并上传", systemImage: "paperplane")
                    }
                    .disabled(uploading || rawFrames.isEmpty)

                    if !uploadStatus.isEmpty {
                        Text(uploadStatus)
                            .font(.footnote)
                            .foregroundStyle(uploading ? .orange : .secondary)
                    }
                } header: {
                    Text("上传到仪表")
                } footer: {
                    Text("仪表**开机就会开**一个名为 \(Self.deviceAPSSID) 的免密热点（IP 固定 \(Self.deviceAPHost)）。\n点上面的按钮会自动连上它并上传，传完自动断开。\n\n如果自动连失败，手动在「设置 → 无线局域网」里选 \(Self.deviceAPSSID) 也能传。")
                }
            }
            .navigationTitle("照片")
            .onChange(of: pickedItem) { _, item in
                guard let item else { return }
                Task { await load(item) }
            }
            .alert("出错了", isPresented: $showError) {
                Button("好", role: .cancel) {}
            } message: {
                Text(errorText)
            }
        }
    }

    // MARK: - 手势

    private func magnifyGesture(side: CGFloat) -> some Gesture {
        MagnificationGesture()
            .onChanged { v in
                let raw = committedZoom * v
                zoom = min(max(raw, PhotoStudio.CropTransform.minZoom), PhotoStudio.CropTransform.maxZoom)
            }
            .onEnded { _ in
                committedZoom = zoom
                pan = .zero
                reapplyTransform()
            }
    }

    /// 两指旋转：吸附到 90° 的整数倍（自由角度在圆屏裁切里没有意义，还会留黑角）
    private func rotateGesture() -> some Gesture {
        RotationGesture()
            .onEnded { v in
                let degrees = committedRotation + v.degrees
                committedRotation = (degrees / 90).rounded() * 90
                normalizeRotation()
                reapplyTransform()
            }
    }

    /// 保留：手势吸附或调试时按 90° 步进（UI 上是四选一 Picker，不再用这个按钮）
    private func rotate90() {
        committedRotation += 90
        normalizeRotation()
        reapplyTransform()
    }

    /// 把角度归到 0..<360，避免长期累加后出现 720°、-90° 这类值
    private func normalizeRotation() {
        var r = committedRotation.truncatingRemainder(dividingBy: 360)
        if r < 0 { r += 360 }
        committedRotation = r
    }

    private func dragGesture(side: CGFloat) -> some Gesture {
        DragGesture()
            .onChanged { v in
                pan = v.translation
            }
            .onEnded { v in
                committedPan.width += v.translation.width
                committedPan.height += v.translation.height
                pan = .zero
                clampCommittedPan(side: side)
                reapplyTransform()
            }
    }

    /// 把平移夹到"图片仍能铺满圆"的范围内
    private func clampCommittedPan(side: CGFloat) {
        guard side > 0 else { return }
        /*
         * 可平移的极限：图片放大后超出的部分的一半。
         * 边长 = side * zoom，圆形区域 = side；最大偏移 = (side*zoom - side)/2。
         * 用比例保存（除以 side），所以预览/导出尺寸变化都通用。
         */
        let limitX = (committedZoom - 1) / 2
        let limitY = (committedZoom - 1) / 2
        committedPan.width = min(max(committedPan.width / side, -limitX), limitX) * side
        committedPan.height = min(max(committedPan.height / side, -limitY), limitY) * side
    }

    private func resetTransform() {
        zoom = 1
        pan = .zero
        committedZoom = 1
        committedPan = .zero
        committedRotation = 0
        reapplyTransform()
    }

    /// 手势结束后：用当前变换重新裁切所有帧，并重新编码 GIF
    private func reapplyTransform() {
        guard !rawFrames.isEmpty else { return }
        let side = 300.0   // 只用它的**倒数**做比例换算，具体值不影响结果
        let t = PhotoStudio.CropTransform(
            zoom: committedZoom,
            panX: committedPan.width / side,
            panY: committedPan.height / side,
            rotation: committedRotation
        )
        // 换帧：把 rawFrames 按变换重新画一遍（尺寸用原始 size）
        let w = Int(rawFrames[0].image.width)
        var out: [StudioFrame] = []
        out.reserveCapacity(rawFrames.count)
        for f in rawFrames {
            let ui = UIImage(cgImage: f.image)
            let cropped = PhotoStudio.squareCropped(ui, size: w, transform: t)
            guard let cg = cropped.cgImage else { continue }
            out.append(StudioFrame(image: cg, duration: f.duration))
        }
        guard !out.isEmpty else { return }
        frameCount = out.count
        preview = UIImage(cgImage: out[0].image)
        gifData = try? PhotoStudio.gifData(from: out)
    }

    // MARK: - 载入

    private func load(_ item: PhotosPickerItem) async {
        await MainActor.run {
            busy = true
            status = "正在读取…"
            gifData = nil
            preview = nil
            frameCount = 0
            Self.pendingFPSNote = nil
        }
        defer { Task { @MainActor in busy = false } }

        do {
            let frames = try await loadFrames(from: item)
            await MainActor.run {
                rawFrames = frames
                zoom = 1; pan = .zero; committedZoom = 1; committedPan = .zero
                committedRotation = 0
                frameCount = frames.count
                if let first = frames.first {
                    preview = UIImage(cgImage: first.image)
                }
                let total = frames.reduce(0.0) { $0 + $1.duration }
                status = frames.count == 1
                    ? "单帧（静态图片）"
                    : "\(frames.count) 帧 · 约 \(String(format: "%.1f", total)) 秒"
                // 先编码好，让"导出"按钮立即可用（编码几百 KB 很快）
                gifData = try? PhotoStudio.gifData(from: frames)
            }
        } catch {
            await MainActor.run {
                errorText = error.localizedDescription
                showError = true
                status = ""
            }
        }
    }

    private func loadFrames(from item: PhotosPickerItem) async throws -> [StudioFrame] {
        // 视频：走 AVAssetImageGenerator 抽帧
        if let movie = try? await item.loadTransferable(type: MovieTransferable.self) {
            // 临时文件必须删 —— 否则一个 200MB 的视频会一直躺在沙盒里
            defer { try? FileManager.default.removeItem(at: movie.url) }
            do {
                return try await PhotoStudio.frames(fromVideo: movie.url, size: size, fps: fps) {
                    effective, wanted in
                    // 在后台线程拿到结果；存到本地变量，主线程再显示
                    Self.pendingFPSNote = "帧数预算有限：已从 \(Int(wanted)) fps 自动降到 \(String(format: "%.1f", effective)) fps（整段保留）"
                }
            } catch {
                throw error
            }
        }
        // 图片/动图：直接读原始数据（GIF 的帧信息在数据里）
        if let data = try await item.loadTransferable(type: Data.self) {
            return try PhotoStudio.frames(from: data, maxSize: size)
        }
        throw PhotoStudioError.cannotReadSource
    }

    // MARK: - 上传到仪表

    private func upload() async {
        await MainActor.run {
            uploading = true
            uploadStatus = "正在编码 .pgi…"
        }
        do {
            // 用**当前（已裁切/变换后）的帧**编码，而不是原始帧
            let frames = try currentFrames()
            let pgi = try PgiEncoder.encode(frames: frames, fps: Int(fps.rounded()))
            await MainActor.run {
                pgiBytes = pgi.count
                uploadStatus = "已编码 \(pgi.count / 1024) KB，正在上传…"
            }
            /*
             * ⚠️ **顺序很重要：先直接传，失败了才去连热点。**
             *
             * 踩过的坑（两次）：
             *   1. 一上来就 `apply` 热点配置 —— 用户已经手动连上时会报 `internal`，
             *      而当时把它当失败抛出去，导致"网络通着却上传失败"；
             *   2. 改成"先 apply 再传"后报 **"The network connection was lost"** ——
             *      因为 `NEHotspotConfiguration.apply` 会**重新关联**网络，
             *      把正在用的那条连接**拆掉了**，请求自然中断。
             *
             * 正确顺序：手机多半已经连着 CarDash（用户手动连的，或上次 joinOnce 留下的），
             * **直接 POST 一次**；只有它失败才说明真没连上，这时再去 apply 并重试。
             */
            /*
             * 先做一次 GET /ping 探针。
             *
             * 为什么：用户实测 Safari 打开 http://192.168.4.1/ping 是通的
             * （返回 {"ok":true}），说明**设备和网络都没问题**，问题在 App 的网络层。
             * 先 ping 一下能把范围再收窄一层：
             *   - ping 也失败 → App 的网络配置/权限问题（会显示具体 URLError 码）；
             *   - ping 成功、POST 失败 → 就是 POST 这条请求本身（体积/超时/服务端 upload 流）。
             */
            await MainActor.run { uploadStatus = "正在探测设备…" }
            let pingResult = await probe()
            guard pingResult == nil else {
                throw PgiEncoderError.encodeFailed("探测 /ping 失败：\(pingResult!)")
            }

            do {
                try await post(pgi)
            } catch {
                // 第一次失败 → 尝试连热点，再重试一次
                await MainActor.run { uploadStatus = "第一次上传失败，正在连接热点后重试…" }
                if await ensureHotspot() {
                    try await post(pgi)
                } else {
                    throw error
                }
            }
            forgetHotspot()
            await MainActor.run {
                uploadStatus = "上传成功（\(pgi.count / 1024) KB）"
                uploading = false
            }
        } catch {
            await MainActor.run {
                var msg = "失败：\(Self.describeNetError(error))"
                if let join = Self.lastJoinError {
                    msg += "\n（自动连热点：\(join)）"
                }
                msg += "\n请确认手机已连上 \(Self.deviceAPSSID) 热点"
                uploadStatus = msg
                uploading = false
            }
        }
    }

    /// 取"当前变换下"的帧（与预览一致）
    private func currentFrames() throws -> [CGImage] {
        guard !rawFrames.isEmpty else { throw PgiEncoderError.noFrames }
        let side = 300.0
        let t = PhotoStudio.CropTransform(
            zoom: committedZoom,
            panX: committedPan.width / side,
            panY: committedPan.height / side,
            rotation: committedRotation
        )
        let w = Int(rawFrames[0].image.width)
        var out: [CGImage] = []
        for f in rawFrames {
            let ui = UIImage(cgImage: f.image)
            let cropped = PhotoStudio.squareCropped(ui, size: w, transform: t)
            guard let cg = cropped.cgImage else { continue }
            out.append(cg)
        }
        guard !out.isEmpty else { throw PgiEncoderError.noFrames }
        return out
    }

    /// 加入仪表的 Wi-Fi 热点（设备开的是 AP，IP 固定 192.168.4.1）
    ///
    /// 用 `NEHotspotConfiguration` 是为了**免去用户去「设置」里手动选网络** ——
    /// 这正是用户要的"像蓝牙那样，扫到就连上"。
    /// 若系统拒绝（没授权/已连别的网被拒），回退提示用户手动连接。
    /*
     * 面向 ESP32 `WebServer` 的会话配置。
     *
     * ⚠️ **关键：`Connection: close`**。
     *
     * 现象：Safari 打开 `http://192.168.4.1/ping` 正常，但 App 里 URLSession 报
     * **URLError -1005（连接被断开）**。
     * 差别在**连接复用**：Safari 每次新开连接，而 URLSession 默认会复用（keep-alive）。
     * ESP32 的 `WebServer` 对 keep-alive 支持很差 —— 它处理完一个请求就断开，
     * 下一次复用这条连接时就被判成"连接被断开"。
     *
     * 强制 `Connection: close` 后，每个请求都是新连接，与 Safari 行为一致。
     */
    private static func makeSession(timeout: TimeInterval) -> URLSession {
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = timeout
        cfg.timeoutIntervalForResource = timeout * 2
        cfg.httpShouldUsePipelining = false
        cfg.httpMaximumConnectionsPerHost = 1
        cfg.requestCachePolicy = .reloadIgnoringLocalCacheData
        cfg.httpAdditionalHeaders = ["Connection": "close"]
        return URLSession(configuration: cfg)
    }

    /// 最小连通性探针：GET /ping。成功返回 nil，失败返回"错误码 + 说明"。
    private func probe() async -> String? {
        guard let url = URL(string: "http://\(Self.deviceAPHost)/ping") else {
            return "URL 非法"
        }
        var req = URLRequest(url: url)
        req.timeoutInterval = 8
        req.setValue("close", forHTTPHeaderField: "Connection")
        let session = Self.makeSession(timeout: 8)
        do {
            let (_, resp) = try await session.data(for: req)
            guard let http = resp as? HTTPURLResponse else { return "非 HTTP 响应" }
            return http.statusCode == 200 ? nil : "HTTP \(http.statusCode)"
        } catch {
            return Self.describeNetError(error)
        }
    }

    /// 把 URLError 翻成"码 + 说明"，否则永远只看到一句没用的英文
    private static func describeNetError(_ error: Error) -> String {
        let ns = error as NSError
        guard ns.domain == NSURLErrorDomain else {
            return "\(ns.domain)#\(ns.code) \(ns.localizedDescription)"
        }
        let names: [Int: String] = [
            NSURLErrorCannotFindHost: "找不到主机",
            NSURLErrorCannotConnectToHost: "连不上主机",
            NSURLErrorNetworkConnectionLost: "连接被断开",
            NSURLErrorNotConnectedToInternet: "未联网",
            NSURLErrorTimedOut: "超时",
            NSURLErrorAppTransportSecurityRequiresSecureConnection: "ATS 要求 HTTPS",
            NSURLErrorDataNotAllowed: "数据被策略禁止（蜂窝被禁时常见）",
            NSURLErrorSecureConnectionFailed: "安全连接失败",
            NSURLErrorCannotLoadFromNetwork: "无法从网络加载",
        ]
        let name = names[ns.code] ?? "未知"
        return "URLError \(ns.code)（\(name)）"
    }

    /// 尝试连上仪表热点；返回是否"现在应该连着"（失败也会返回 true 让调用方重试一次）
    private func ensureHotspot() async -> Bool {
        let manager = NEHotspotConfigurationManager.shared
        let cfg = NEHotspotConfiguration(ssid: Self.deviceAPSSID)  // 免密不传 passphrase
        /*
         * **不用 joinOnce** —— 实测它会配合 `apply` 把已建立的关联拆掉，
         * 表现为 "The network connection was lost"。
         * 改为普通加入，传完再显式 `removeConfiguration`（见下）。
         */

        let result: NSError? = await withCheckedContinuation { cont in
            manager.apply(cfg) { error in
                cont.resume(returning: error as NSError?)
            }
        }
        if let e = result {
            /*
             * 常见的三种，全都**不影响**后续上传：
             *   - alreadyAssociated / internal：手机已经连着这个网络；
             *   - applicationIsNotInForeground：App 不在前台；
             *   - systemConfiguration：系统层问题。
             * 所以这里只记录，不抛错。
             */
            Self.lastJoinError = Self.describeHotspotError(e)
        } else {
            Self.lastJoinError = nil
            // 刚关联上时 DHCP 还没就绪，稍等一下（只有真的连过才需要）
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
        return true   // 让调用方重试 —— 到底通不通由 POST 说了算
    }

    /// 传完把热点配置移掉，避免手机长期"粘"在 CarDash 上
    private func forgetHotspot() {
        NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: Self.deviceAPSSID)
    }

    /// 把 NEHotspotConfiguration 的错误码翻成人话（否则只会看到 "internal error"）
    private static func describeHotspotError(_ e: NSError) -> String {
        guard e.domain == NEHotspotConfigurationErrorDomain else {
            return e.localizedDescription
        }
        switch NEHotspotConfigurationError(rawValue: e.code) {
        case .alreadyAssociated: return "已连接该热点"
        case .invalid: return "热点参数非法"
        case .invalidSSID: return "热点名非法"
        case .invalidWPAPassphrase: return "热点密码非法"
        case .invalidWEPPassphrase: return "热点密码非法（WEP）"
        case .invalidSSIDPrefix: return "热点名前缀非法"
        case .userDenied: return "用户拒绝了连接"
        case .systemConfiguration: return "系统网络配置错误"
        case .unknown: return "未知错误"
        case .applicationIsNotInForeground: return "App 不在前台"
        case .internal: return "系统内部错误（通常是已连着该网络）"
        case .none: return "调用成功"
        @unknown default: return e.localizedDescription
        }
    }

    private func post(_ data: Data) async throws {
        let host = deviceHost.trimmingCharacters(in: .whitespacesAndNewlines)
        let name = "app-\(Int(Date().timeIntervalSince1970)).pgi"
        guard var comps = URLComponents(string: "http://\(host)/upload") else {
            throw PgiEncoderError.encodeFailed("IP 格式不对")
        }
        comps.queryItems = [URLQueryItem(name: "name", value: name)]
        guard let url = comps.url else { throw PgiEncoderError.encodeFailed("IP 格式不对") }

        /*
         * ⚠️ **必须用 multipart/form-data，不能用 application/octet-stream。**
         *
         * 崩溃现场（设备串口 + addr2line 解出的调用栈）：
         *     WebServer::upload()      ← 我们的上传回调
         *     HTTPUpload 的 unique_ptr 是 **NULL**
         *     → LoadProhibited（空指针），设备 panic 重启
         *     → 手机端看到 URLError -1005「连接被断开」
         *
         * 原因：ESP32 的 `WebServer` **只在 `multipart/form-data` 时才创建
         * upload 上下文**；发原始 octet-stream 时它不存在，
         * 回调里 `server.upload()` 一解引用就崩。
         */
        let boundary = "----CarDash\(UUID().uuidString)"
        var body = Data()
        body.append("--\(boundary)\r\n".data(using: .utf8)!)
        body.append("Content-Disposition: form-data; name=\"file\"; filename=\"\(name)\"\r\n".data(using: .utf8)!)
        body.append("Content-Type: application/octet-stream\r\n\r\n".data(using: .utf8)!)
        body.append(data)
        body.append("\r\n--\(boundary)--\r\n".data(using: .utf8)!)

        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        req.timeoutInterval = 60

        /*
         * 走与 probe 相同的会话配置（`Connection: close`）。
         *
         * 之前试过 `allowsCellularAccess = false` 想强制走 Wi-Fi，
         * 但那会配合"无互联网的 Wi-Fi"让 iOS 一直等不到"可用网络"，
         * 反而更容易报 -1005。**不使用**该限制，改用"短连接 + 重试"。
         */
        req.setValue("close", forHTTPHeaderField: "Connection")
        let session = Self.makeSession(timeout: 120)

        // -1005 常见于设备处理完后主动断开，重试一次往往就成功
        var lastError: Error?
        var resp: URLResponse?
        for attempt in 0..<3 {
            do {
                let (_, r) = try await session.upload(for: req, from: body)
                resp = r
                lastError = nil
                break
            } catch {
                lastError = error
                if attempt < 2 {
                    await MainActor.run { uploadStatus = "上传中断，正在重试（\(attempt + 2)/3）…" }
                    try? await Task.sleep(nanoseconds: 800_000_000)
                }
            }
        }
        if let e = lastError { throw e }
        guard let resp else { throw PgiEncoderError.encodeFailed("没有响应") }
        guard let http = resp as? HTTPURLResponse else {
            throw PgiEncoderError.encodeFailed("没有 HTTP 响应")
        }
        guard (200..<300).contains(http.statusCode) else {
            throw PgiEncoderError.encodeFailed("HTTP \(http.statusCode)")
        }
    }

    private func exportGIF() async {
        guard let gifData else { return }
        await MainActor.run {
            busy = true
            status = "正在保存到相册…"
        }
        do {
            try await PhotoStudio.saveToPhotoLibrary(gifData)
            let over = gifData.count > PhotoStudio.deviceGifLimitBytes
            await MainActor.run {
                status = over
                    ? "已保存到相册（\(frameCount) 帧），但 \(gifData.count / 1024 / 1024) MB 超过设备 8MB 上限，传到仪表前需要减小边长或帧数"
                    : "已保存到相册（\(frameCount) 帧 · \(gifData.count / 1024) KB）"
                busy = false
            }
        } catch {
            await MainActor.run {
                errorText = error.localizedDescription
                showError = true
                busy = false
                status = ""
            }
        }
    }
}

/// 把 PhotosPicker 选到的视频落到临时文件（AVAsset 需要 URL）
struct MovieTransferable: Transferable {
    let url: URL

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(contentType: .movie) { movie in
            SentTransferredFile(movie.url)
        } importing: { received in
            /*
             * **先卡体积再复制** —— 复制本身要读写整份文件，
             * 等复制完再判断就白花了时间和磁盘。
             */
            let size = (try? received.file.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            if size > PhotoStudio.maxVideoBytes {
                throw PhotoStudioError.videoTooLarge(maxBytes: PhotoStudio.maxVideoBytes)
            }
            // 系统给的临时文件可能在导入后就被回收，复制到自己的临时目录
            let dst = FileManager.default.temporaryDirectory
                .appendingPathComponent("picked-\(UUID().uuidString).mov")
            try FileManager.default.copyItem(at: received.file, to: dst)
            return MovieTransferable(url: dst)
        }
    }
}
