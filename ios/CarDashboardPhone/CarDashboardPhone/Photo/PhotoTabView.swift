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

    /*
     * 设备的 AP 模式是**固定 IP**：ESP32 softAP 的网关恒为 192.168.4.1，
     * 所以这里不需要用户填、也不需要 BLE 传 IP。
     */
    private static let deviceAPHost = "192.168.4.1"
    private static let deviceAPSSID = "CarDash"
    private static let deviceAPPass = "cardash1"

    @State private var deviceHost: String = PhotoTabView.deviceAPHost
    @State private var uploadStatus = ""
    @State private var uploading = false
    @State private var pgiBytes = 0

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
                    Text("仪表会开一个名为 \(Self.deviceAPSSID) 的热点（IP 固定 \(Self.deviceAPHost)）。\n点上面的按钮会自动连上它并上传，传完自动断开。\n\n手机需先在串口敲 `wifi on` 打开热点。")
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
            // 先确保连上仪表热点（已连则直接返回）
            try await joinDeviceHotspot()
            try await post(pgi)
            await MainActor.run {
                uploadStatus = "上传成功（\(pgi.count / 1024) KB）"
                uploading = false
            }
        } catch {
            await MainActor.run {
                uploadStatus = "失败：\(error.localizedDescription)"
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
    private func joinDeviceHotspot() async throws {
        let manager = NEHotspotConfigurationManager.shared
        let cfg = NEHotspotConfiguration(
            ssid: Self.deviceAPSSID,
            passphrase: Self.deviceAPPass,
            isWEP: false
        )
        cfg.joinOnce = true   // 传完就断，不长期占用

        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            manager.apply(cfg) { error in
                if let e = error as NSError? {
                    // 已经连着这个网络时也会报这个错 —— 当作成功
                    if e.domain == NEHotspotConfigurationErrorDomain,
                       e.code == NEHotspotConfigurationError.alreadyAssociated.rawValue {
                        cont.resume()
                        return
                    }
                    cont.resume(throwing: e)
                    return
                }
                cont.resume()
            }
        }
        // 拿到 DHCP 需要一点时间
        try? await Task.sleep(nanoseconds: 1_200_000_000)
    }

    private func post(_ data: Data) async throws {
        let host = deviceHost.trimmingCharacters(in: .whitespacesAndNewlines)
        let name = "app-\(Int(Date().timeIntervalSince1970)).pgi"
        guard var comps = URLComponents(string: "http://\(host)/upload") else {
            throw PgiEncoderError.encodeFailed("IP 格式不对")
        }
        comps.queryItems = [URLQueryItem(name: "name", value: name)]
        guard let url = comps.url else { throw PgiEncoderError.encodeFailed("IP 格式不对") }

        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        req.timeoutInterval = 60

        let (resp, _) = try await URLSession.shared.upload(for: req, from: data)
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
