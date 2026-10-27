//
//  PhotoTabView.swift
//  CarDashboardPhone
//
//  「照片」页 —— 通过蓝牙把照片直接传到仪表（不再拔 SD 卡、不再走 Wi-Fi）。
//
//  ## 页面结构（从上到下）
//
//    ① 仪表状态：连接 / 存储 / 可用空间 / 照片数；设备能力（静态 ✅、动图可用性 + 原因）
//    ② 上传队列：逐张进度 / 速率 / 预计剩余；失败可重试；断线显示"等待重连"
//    ③ 设备相册：缩略图 + 类型/尺寸/大小/帧数 + 当前播放标记；点按播放、左滑删除（二次确认）
//    ④ 播放设置：轮播 / 间隔 / 像素位移 / 自动息屏 —— 值来自设备，改动即下发、失败回滚
//    ⑤ 高级：设备自检（重新测动图帧率）、手动刷新
//
//  ## 三条铁律
//
//  1. **不假装**：能力是否可用、上传到哪一步，全部来自 `BLEClient.photoLibrary` /
//     `photoUploads` 与设备回执；没数据就显示"读取中"，不编一个乐观状态。
//  2. **降级要说明原因**：旧固件 / LITE 档 / 速率不够 / 没插卡，都写清楚"为什么"
//     以及"怎么办"，而不是给一个点不动的死按钮。
//  3. **文案说人话**：不用"操作失败"这种没有信息量的句子，写清楚发生了什么、
//     用户下一步能做什么（对齐 docs/photo-wireless-design.md §10.6）。
//

import AVFoundation
import PhotosUI
import SwiftUI
import UniformTypeIdentifiers

struct PhotoTabView: View {

    @ObservedObject var session: DriveSessionManager
    /// true = 从「仪表 → 表盘 → 照片」的 NavigationLink 进来（外层已有导航栈），
    /// 不再套一层 NavigationStack，避免双导航栏。
    var embedded: Bool = false

    private var ble: BLEClient { session.ble }

    // ── 添加照片流程 ────────────────────────────────
    @State private var pickerItems: [PhotosPickerItem] = []
    @State private var loadingSelection = false
    @State private var prepareItems: [PhotoPrepareItem] = []
    @State private var showingPrepare = false
    @State private var loadWarning: String?

    // ── 上传 / 缩略图 ───────────────────────────────
    /// 上传队列 id → 已落盘的临时文件（完成/取消后删除，避免占盘）
    @State private var stagedFileURLs: [String: URL] = [:]
    /// 等待与设备相册对上的缩略图（上传完成、清单刷新后按 bytes/frames 匹配设备 id）
    @State private var pendingThumbnails: [PendingPhotoThumbnail] = []
    /// 上一轮看到的队列状态（用来只在"变成 done"时提示一次）
    @State private var seenUploadStates: [String: PhotoUploadState] = [:]

    // ── 设备相册 ────────────────────────────────────
    @State private var deleteTarget: DevicePhoto?

    // ── 播放设置（草稿；真值永远来自设备 PhotoState）──
    @State private var settings = PhotoSettingsDraft()

    // ── 反馈 ────────────────────────────────────────
    @State private var alertMessage: String?
    @State private var showFormatConfirm = false
    @State private var toast: String?
    @State private var toastToken = UUID()

    @Environment(\.scenePhase) private var scenePhase

    init(session: DriveSessionManager, embedded: Bool = false) {
        self._session = ObservedObject(wrappedValue: session)
        self.embedded = embedded
    }

    var body: some View {
        Group {
            if embedded {
                form
            } else {
                NavigationStack { form }
            }
        }
    }

    private var form: some View {
        formPresentations
            .navigationTitle("照片")
            .toolbar { addToolbar }
            .refreshable {
                if ble.isConnected && ble.photoSupported { ble.refreshPhotoLibrary() }
            }
    }

    // 拆成 3 段：单个 SwiftUI 表达式太长会触发 type-check 超时（实测 >10 s 报错）
    private var formPresentations: some View {
        formLifecycle
            .confirmationDialog("格式化 SD 卡？", isPresented: $showFormatConfirm,
                                titleVisibility: .visible) {
                Button("格式化并删除全部照片", role: .destructive) {
                    ble.formatPhotoCard()
                }
                Button("取消", role: .cancel) {}
            } message: {
                Text("这会删除仪表存储卡上的所有照片和未完成的上传，且无法恢复。格式化后仪表会重建一个空相册。")
            }
            .onChange(of: ble.photoFormatState) { _, state in
                handleFormatStateChange(state)
            }
            .confirmationDialog("删除这张照片？", isPresented: deletePresented,
                                titleVisibility: .visible, presenting: deleteTarget) { photo in
                Button("删除「\(photoTitle(photo))」", role: .destructive) { delete(photo) }
                Button("取消", role: .cancel) { deleteTarget = nil }
            } message: { _ in
                Text("仪表存储里的文件会被永久删除，无法恢复。")
            }
            .alert("提示", isPresented: alertPresented) {
                Button("好", role: .cancel) {}
            } message: {
                Text(alertMessage ?? "")
            }
    }

    private var formLifecycle: some View {
        formBase
            .onAppear {
                PGJEncoder.cleanStaleFiles()
                syncSettings(from: ble.photoLibrary?.state)
                if ble.isConnected && ble.photoSupported {
                    ble.refreshPhotoLibrary()
                }
            }
            .onChange(of: pickerItems) { _, items in
                guard !items.isEmpty else { return }
                pickerItems = []
                Task { await loadSelection(items) }
            }
            .onChange(of: ble.photoUploads) { _, uploads in
                handleUploadsChange(uploads)
            }
            .onChange(of: ble.photoLibrary) { _, library in
                syncSettings(from: library?.state)
                assignPendingThumbnails()
            }
            .sheet(isPresented: $showingPrepare, onDismiss: cleanupPrepare) {
                PhotoPrepareView(
                    items: prepareItems,
                    canAnimate: canAnimate,
                    animationReason: animationUnavailableReason,
                    measuredRate: ble.photoMeasuredUploadRate,
                    freeBytes: availableFreeBytes,
                    loadWarning: loadWarning,
                    onCancel: { showingPrepare = false },
                    onEnqueue: handlePreparedUploads
                )
            }
    }

    private var formBase: some View {
        Form {
            statusSection
            if ble.isConnected && ble.photoSupported && ble.photoLibrary != nil
                && !showsOldFirmware {
                addPhotoCTASection
            }
            if ble.isConnected && ble.photoSupported && ble.photoLibrary != nil {
                capabilitySection
            }

            if showsOldFirmware {
                oldFirmwareSection
            } else if ble.isConnected && !ble.photoServiceProbed {
                Section {
                    HStack(spacing: 10) {
                        ProgressView()
                        Text("正在确认仪表是否支持无线传图…")
                            .foregroundStyle(.secondary)
                    }
                }
            } else if ble.isConnected {
                if loadingSelection { loadingSelectionSection }
                uploadQueueSection
                albumSection
                settingsSection
                advancedSection
            } else {
                disconnectedHintSection
            }

            toastSection
        }
    }

    private var alertPresented: Binding<Bool> {
        Binding(get: { alertMessage != nil },
                set: { if !$0 { alertMessage = nil } })
    }

    private var deletePresented: Binding<Bool> {
        Binding(get: { deleteTarget != nil },
                set: { if !$0 { deleteTarget = nil } })
    }

    // MARK: - 派生状态

    private var showsOldFirmware: Bool {
        ble.isConnected && ble.photoServiceProbed && !ble.photoSupported
    }

    /// 设备有没有说"存储未就绪"。没说（缺字段）时按"可添加"处理，
    /// 不能把"没上报"当成"没插卡" —— 否则用户会看到莫名其妙的禁用。
    private var storageReadyForUI: Bool {
        guard let state = ble.photoLibrary?.state else { return false }
        return state.reported(.storage) ? state.isStorageReady : true
    }

    /// 可用空间（字节）；设备没上报 free_kb 时返回 nil（不做空间拦截）
    private var availableFreeBytes: Int? {
        guard let state = ble.photoLibrary?.state, state.reported(.freeKiB) else { return nil }
        return state.freeBytes
    }

    /// 只有"连接 + 支持 + 存储就绪"才能添加；其余情况入口不渲染（不是灰按钮）
    private var canAdd: Bool {
        ble.isConnected
            && ble.photoSupported
            && storageReadyForUI
            && !loadingSelection
    }

    /// 动图入口的三层门控（docs/photo-wireless-design.md §7.1）
    /// ① 固件实现了动图 + ② 设备实测 fps ≥8 + ③ 本次连接实测上传 ≥30 KB/s
    private var canAnimate: Bool {
        guard let info = ble.photoLibrary?.info else { return false }
        guard info.supportsAnimation else { return false }
        if let rate = ble.photoMeasuredUploadRate, rate < PhotoLimit.animateMinRate {
            return false
        }
        return true
    }

    /// 动图不可用的**具体原因**（必须可见，否则用户以为功能坏了）。
    ///
    /// 前两层（固件能力 + 设备实测 fps）由 `DevicePhotoInfo` 统一给文案，
    /// 这里只补第三层"本次连接实测上传速率"。
    private var animationUnavailableReason: String? {
        guard let info = ble.photoLibrary?.info else { return "还没读到设备能力" }
        if let reason = info.animationUnavailableReason { return reason }
        if let rate = ble.photoMeasuredUploadRate, rate < PhotoLimit.animateMinRate {
            return "当前连接实测 \(PhotoFormat.rate(rate))，动图需要 ≥30 KB/s"
        }
        return nil
    }

    // MARK: - ① 仪表状态

    @ViewBuilder
    private var statusSection: some View {
        Section {
            HStack(spacing: 8) {
                Circle()
                    .fill(ble.isConnected ? Color.green : Color.orange)
                    .frame(width: 9, height: 9)
                Text(ble.isConnected
                     ? "已连接 \(ble.discoveredName ?? "仪表")"
                     : "未连接仪表")
                Spacer()
                if !ble.isConnected {
                    Text(ble.stateText)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            if !ble.isConnected, let notice = ble.lastNotice, !notice.isEmpty {
                Text(notice)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }

            if !ble.isConnected {
                Text("照片会通过蓝牙直接传到仪表，不用拔存储卡。连接仪表后才能查看或添加照片。")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                Button {
                    ble.scanAndConnect()
                } label: {
                    Label("连接仪表", systemImage: "antenna.radiowaves.left.and.right")
                }
            } else if let library = ble.photoLibrary {
                LabeledContent("存储", value: storageText(library.state.storageKind))
                LabeledContent("可用空间", value: PhotoFormat.bytes(library.state.freeBytes))
                LabeledContent("照片", value: "\(library.count) 张")
                LabeledContent("本次实测上传",
                               value: ble.photoMeasuredUploadRate.map(PhotoFormat.rate) ?? "还没测过")
            } else if ble.photoSupported {
                HStack(spacing: 10) {
                    ProgressView()
                    Text("正在读取仪表存储信息…").foregroundStyle(.secondary)
                }
            }

            if let library = ble.photoLibrary, library.state.receiving {
                Label("仪表正在接收另一张照片，新的上传会排队等待。",
                      systemImage: "arrow.down.circle")
                    .font(.footnote)
                    .foregroundStyle(.orange)
            }

            if let error = ble.photoLibrary?.lastError {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .font(.footnote)
                    .foregroundStyle(.orange)
            }
        } header: {
            Text("仪表状态")
        } footer: {
            if let library = ble.photoLibrary {
                Text("档位 \(library.info.tier >= 2 ? "FULL（可传动图）" : "LITE（只传静态照片）")")
                    .font(.footnote)
            }
        }
    }

    /// 页面最显眼的主操作：添加照片（比右上角 ＋ 可发现性高得多）
    private var addPhotoCTASection: some View {
        Section {
            PhotosPicker(selection: $pickerItems,
                         maxSelectionCount: 20,
                         selectionBehavior: .ordered,
                         matching: .any(of: [.images, .videos]),
                         photoLibrary: .shared()) {
                HStack(spacing: 14) {
                    ZStack {
                        Circle()
                            .fill(Color.accentColor.opacity(0.14))
                            .frame(width: 46, height: 46)
                        Image(systemName: "photo.badge.plus")
                            .font(.system(size: 20, weight: .semibold))
                            .foregroundStyle(Color.accentColor)
                    }
                    VStack(alignment: .leading, spacing: 3) {
                        Text("添加照片")
                            .font(.headline)
                            .foregroundStyle(.primary)
                        Text("从相册选择，自动裁成 466×466 后传到仪表")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 0)
                    Image(systemName: "chevron.right")
                        .font(.footnote.weight(.semibold))
                        .foregroundStyle(.tertiary)
                }
                .contentShape(Rectangle())
            }
            .disabled(!canAdd)
            .accessibilityIdentifier("addPhotoCTA")
        } footer: {
            if !storageReadyForUI {
                Text("仪表没有检测到存储卡，暂时不能添加照片。")
            }
        }
    }

    private func storageText(_ kind: PhotoStorage) -> String {
        switch kind {
        case .sd: return "SD 卡"
        case .internalFlash: return "仪表内置存储"
        case .unknown: return "未就绪"
        }
    }

    // MARK: - 设备能力

    @ViewBuilder
    private var capabilitySection: some View {
        let info = ble.photoLibrary?.info
        Section {
            HStack {
                Label("静态照片", systemImage: "photo")
                Spacer()
                Text(info?.supportsStaticPhoto == true ? "可用" : "不可用")
                    .font(.footnote)
                    .foregroundStyle(info?.supportsStaticPhoto == true ? .green : .orange)
            }
            HStack {
                Label("动图", systemImage: "photo.stack")
                Spacer()
                Text(canAnimate ? "可用" : "不可用")
                    .font(.footnote)
                    .foregroundStyle(canAnimate ? .green : .orange)
            }
            if let reason = animationUnavailableReason {
                Text(reason)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            if let rate = ble.photoMeasuredUploadRate {
                Text("本次连接实测上传 \(PhotoFormat.rate(rate))")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            if let info, info.supportsStaticPhoto, info.benchDecodeMs > 0 {
                Text("上次测到 JPEG 解码约 \(info.benchDecodeMs) ms")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("设备能力")
        } footer: {
            Text("动图要同时满足：固件支持、设备实测帧率 ≥8 fps、本次连接上传 ≥30 KB/s。不满足时会说明具体原因。")
                .font(.footnote)
        }
    }

    // MARK: - 能力说明（旧固件 / 存储未就绪 / 无照片）

    @ViewBuilder
    private var oldFirmwareSection: some View {
        Section {
            Label("当前固件不支持无线传图", systemImage: "exclamationmark.triangle.fill")
                .foregroundStyle(.orange)
            Text("这版仪表固件还没有蓝牙收图功能。升级固件后，就能像现在这样在 App 里选照片直接发送。")
                .font(.footnote)
            Divider()
            Text("现在想加照片，可以走 SD 卡：").font(.subheadline.weight(.medium))
            VStack(alignment: .leading, spacing: 6) {
                Text("① 用电脑上的 tools/import_photos.sh 把图片转成仪表格式")
                Text("② 把生成的文件拷进存储卡 /photos/ 目录")
                Text("③ 把卡插回仪表，照片表盘即可播放")
            }
            .font(.footnote)
            .foregroundStyle(.secondary)
        } header: {
            Text("如何添加照片")
        }
    }

    @ViewBuilder
    private var disconnectedHintSection: some View {
        Section {
            Text("连接仪表后，这里会显示它存储里的照片、剩余空间，以及能不能传动图。")
                .font(.footnote)
                .foregroundStyle(.secondary)
        } header: {
            Text("设备相册")
        }

        // 播放设置没连接时不能读也不能改 —— 显示说明而不是一排点不动的控件，
        // 避免用户误以为 App 卡住了（"值来自设备"意味着没连接就没有真值可比对）。
        Section {
            Label("连接仪表后才能查看和修改播放设置", systemImage: "lock")
                .font(.footnote)
                .foregroundStyle(.secondary)
        } header: {
            Text("播放设置")
        }
    }

    @ViewBuilder
    private var loadingSelectionSection: some View {
        Section {
            HStack(spacing: 10) {
                ProgressView()
                Text("正在读取选中的素材…").foregroundStyle(.secondary)
            }
        }
    }

    // MARK: - ② 上传队列

    @ViewBuilder
    private var uploadQueueSection: some View {
        let uploads = ble.photoUploads
        if !uploads.isEmpty {
            Section {
                if hasActiveUploads && scenePhase != .active {
                    Label("上传已暂停：App 不在前台。回到 App 并保持屏幕常亮，剩下的会继续。",
                          systemImage: "pause.circle.fill")
                        .font(.footnote)
                        .foregroundStyle(.orange)
                }
                ForEach(uploads) { upload in
                    uploadRow(upload)
                }
            } header: {
                HStack {
                    Text("上传队列")
                    Spacer()
                    let active = uploads.filter(\.isActive).count
                    if active > 0 {
                        Text("\(active) 项进行中").font(.caption).foregroundStyle(.secondary)
                    }
                }
            } footer: {
                VStack(alignment: .leading, spacing: 8) {
                    Text("照片正在通过蓝牙传到仪表，请保持 App 打开、不要锁屏。")
                        .font(.footnote)
                    if uploads.contains(where: \.isFinished) {
                        Button("清除已完成记录") {
                            ble.clearFinishedPhotoUploads()
                            pruneStagedFiles()
                        }
                        .font(.footnote)
                    }
                }
            }
        }
    }

    private var hasActiveUploads: Bool {
        ble.photoUploads.contains(where: \.isActive)
    }

    @ViewBuilder
    private func uploadRow(_ upload: PhotoUploadProgress) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Image(systemName: uploadStateSymbol(upload.state))
                    .foregroundStyle(uploadStateColor(upload.state))
                Text(upload.displayName ?? "照片")
                    .font(.subheadline.weight(.medium))
                Spacer()
                Text("\(Int((upload.fraction * 100).rounded()))%")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }

            if upload.state != .done && upload.state != .failed && upload.state != .cancelled {
                ProgressView(value: min(max(upload.fraction, 0), 1))
            }

            HStack(alignment: .firstTextBaseline) {
                Text(uploadDetailText(upload))
                    .font(.caption)
                    .foregroundStyle(upload.state == .failed ? .orange : .secondary)
                Spacer()
                uploadActions(upload)
            }
        }
        .padding(.vertical, 2)
    }

    @ViewBuilder
    private func uploadActions(_ upload: PhotoUploadProgress) -> some View {
        HStack(spacing: 12) {
            if upload.state == .failed {
                Button("重试") { ble.retryPhotoUpload(id: upload.id) }
                    .font(.caption)
            }
            if upload.isActive {
                Button("取消", role: .destructive) { ble.cancelPhotoUpload(id: upload.id) }
                    .font(.caption)
            }
        }
        .buttonStyle(.borderless)
    }

    private func uploadDetailText(_ upload: PhotoUploadProgress) -> String {
        let sent = PhotoFormat.bytes(upload.ackedBytes)
        let total = PhotoFormat.bytes(upload.totalBytes)
        switch upload.state {
        case .queued:
            return "排队中 · \(total)"
        case .uploading:
            var parts = ["\(sent) / \(total)", PhotoFormat.rate(upload.bytesPerSecond)]
            if let eta = upload.etaSeconds { parts.append("剩余\(PhotoFormat.eta(eta))") }
            return parts.joined(separator: " · ")
        case .verifying:
            return "正在校验，等仪表确认（\(sent) / \(total)）"
        case .waitingReconnect:
            return "等待重连，已传 \(Int((upload.fraction * 100).rounded()))% · 重连后自动继续"
        case .done:
            return "已传到仪表"
        case .failed:
            // 优先显示设备/传输层给出的具体原因；没有才给一句能指导下一步的话
            return upload.errorText ?? "传输中断了，点「重试」再传一次"
        case .cancelled:
            return "已取消"
        }
    }

    private func uploadStateSymbol(_ state: PhotoUploadState) -> String {
        switch state {
        case .queued: return "clock"
        case .uploading: return "arrow.up.circle"
        case .verifying: return "checkmark.shield"
        case .waitingReconnect: return "wifi.exclamationmark"
        case .done: return "checkmark.circle.fill"
        case .failed: return "exclamationmark.triangle.fill"
        case .cancelled: return "xmark.circle"
        }
    }

    private func uploadStateColor(_ state: PhotoUploadState) -> Color {
        switch state {
        case .done: return .green
        case .failed: return .orange
        case .cancelled: return .secondary
        case .waitingReconnect: return .orange
        default: return .accentColor
        }
    }

    // MARK: - ③ 设备相册

    @ViewBuilder
    private var albumSection: some View {
        Section {
            if let library = ble.photoLibrary {
                if !storageReadyForUI {
                    Label("仪表未检测到存储卡", systemImage: "externaldrive.badge.exclamationmark")
                        .foregroundStyle(.orange)
                    Text("插好存储卡（或启用仪表内置存储）后再添加照片。现在可以先查看已有内容。")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }

                if library.isLoading {
                    HStack(spacing: 10) {
                        ProgressView()
                        Text("正在读取设备相册…").foregroundStyle(.secondary)
                    }
                } else if library.items.isEmpty {
                    emptyAlbumRow
                } else {
                    ForEach(library.items) { photo in
                        albumRow(photo, currentIndex: library.state.currentIndex)
                    }
                }
            } else {
                HStack(spacing: 10) {
                    ProgressView()
                    Text("正在读取设备相册…").foregroundStyle(.secondary)
                }
            }
        } header: {
            HStack {
                Text("设备相册")
                Spacer()
                if let library = ble.photoLibrary {
                    Text("\(library.items.count) 张")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        } footer: {
            if let library = ble.photoLibrary, !library.items.isEmpty {
                Text("点一张就在仪表上播放；左滑可以删除。文件名顺序就是仪表上的播放顺序。")
                    .font(.footnote)
            }
        }
    }

    private var emptyAlbumRow: some View {
        VStack(spacing: 8) {
            Image(systemName: "photo.on.rectangle.angled")
                .font(.system(size: 30))
                .foregroundStyle(.secondary)
            Text("还没有照片")
                .font(.subheadline.weight(.medium))
            Text("点上方「添加照片」，或右上角 ＋")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 8)
    }

    private func albumRow(_ photo: DevicePhoto, currentIndex: Int) -> some View {
        let isCurrent = photo.index == currentIndex
        let canDelete = ble.photoLibrary?.canDelete ?? false
        return Button {
            if !ble.playPhoto(index: photo.index) {
                alertMessage = "没有切换过去：仪表可能正在接收照片，稍后再点一次。"
            }
        } label: {
            HStack(spacing: 12) {
                PhotoThumbnailView(id: photo.id)
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Text(photoTitle(photo))
                            .foregroundStyle(.primary)
                        if isCurrent {
                            Text("正在播放")
                                .font(.caption2)
                                .foregroundStyle(.green)
                        }
                    }
                    Text(photoDetail(photo))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer(minLength: 8)
                Image(systemName: isCurrent ? "play.circle.fill" : "play.circle")
                    .font(.system(size: 20))
                    .foregroundStyle(isCurrent ? .green : .secondary)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .swipeActions(edge: .trailing, allowsFullSwipe: false) {
            if canDelete {
                Button(role: .destructive) {
                    deleteTarget = photo
                } label: {
                    Label("删除", systemImage: "trash")
                }
            }
        }
    }

    private func photoTitle(_ photo: DevicePhoto) -> String {
        photo.isAnimated ? "动图 \(photo.index + 1)" : "照片 \(photo.index + 1)"
    }

    private func photoDetail(_ photo: DevicePhoto) -> String {
        var parts = ["\(photo.width)×\(photo.height)", PhotoFormat.bytes(photo.bytes)]
        if photo.isAnimated {
            parts.append("\(photo.frames) 帧")
            if photo.fps > 0 { parts.append("\(photo.fps) fps") }
        }
        return parts.joined(separator: " · ")
    }

    private func delete(_ photo: DevicePhoto) {
        deleteTarget = nil
        guard ble.deletePhoto(id: photo.id) else {
            alertMessage = "没有删掉：仪表未连接，或正在接收照片。稍后再试。"
            return
        }
        PhotoThumbnailCache.shared.remove(for: photo.id)
        showToast("已删除「\(photoTitle(photo))」")
    }

    // MARK: - ④ 播放设置

    @ViewBuilder
    private var settingsSection: some View {
        let ready = ble.isConnected && ble.photoLibrary != nil
        Section {
            Group {
                Toggle("自动轮播", isOn: Binding(
                    get: { settings.slideshow },
                    set: { applySettings(slideshow: $0) }
                ))
                Picker("轮播间隔", selection: Binding(
                    get: { settings.intervalSeconds },
                    set: { applySettings(interval: $0) }
                )) {
                    ForEach(intervalOptions, id: \.self) { value in
                        Text(PhotoFormat.interval(value)).tag(value)
                    }
                }
                .disabled(!settings.slideshow)

                Toggle("像素位移（防烧屏）", isOn: Binding(
                    get: { settings.shift },
                    set: { applySettings(shift: $0) }
                ))
                Picker("自动息屏", selection: Binding(
                    get: { settings.screenOffSeconds },
                    set: { applySettings(screenOff: $0) }
                )) {
                    ForEach(screenOffOptions, id: \.self) { value in
                        Text(PhotoFormat.screenOff(value)).tag(value)
                    }
                }
            }
            .disabled(!ready)
        } header: {
            Text("播放设置")
        } footer: {
            Text(ready
                 ? "这些是仪表当前的真实设置，改完立即生效；如果仪表重启或被人改过，App 会自动同步成设备的值。"
                 : "未连接仪表，暂时改不了播放设置。")
                .font(.footnote)
        }
    }

    private var intervalOptions: [Int] {
        var list = [10, 30, 60, 120, 300]
        if !list.contains(settings.intervalSeconds) {
            list.append(settings.intervalSeconds)
            list.sort()
        }
        return list
    }

    private var screenOffOptions: [Int] {
        var list = [0, 60, 180, 300, 600, 1800]
        if !list.contains(settings.screenOffSeconds) {
            list.append(settings.screenOffSeconds)
            list.sort()
        }
        return list
    }

    /// 把设备真值同步到本地草稿；只同步**设备确实上报过**的字段。
    private func syncSettings(from state: DevicePhotoState?) {
        guard let state, ble.isConnected else { return }
        if state.reported(.slideshow) { settings.slideshow = state.slideshow }
        if state.reported(.intervalSeconds) { settings.intervalSeconds = state.intervalSeconds }
        if state.reported(.shift) { settings.shift = state.shift }
        if state.reported(.screenOffSeconds) { settings.screenOffSeconds = state.screenOffSeconds }
    }

    /// 改动即下发；发不出去就**回滚显示值**（设备真值才是准的）。
    private func applySettings(slideshow: Bool? = nil,
                               interval: Int? = nil,
                               shift: Bool? = nil,
                               screenOff: Int? = nil) {
        let previous = settings
        if let slideshow { settings.slideshow = slideshow }
        if let interval { settings.intervalSeconds = interval }
        if let shift { settings.shift = shift }
        if let screenOff { settings.screenOffSeconds = screenOff }

        let sent = ble.setPhotoSettings(slideshow: slideshow,
                                        interval: interval,
                                        shift: shift,
                                        screenOff: screenOff)
        if !sent {
            settings = previous
            alertMessage = "设置没有发出去：仪表未连接，或正在接收照片。稍后再试。"
        }
    }

    // MARK: - ⑤ 高级

    @ViewBuilder
    private var advancedSection: some View {
        Section {
            Button {
                ble.runPhotoBench()
                showToast("已开始设备自检，几秒后返回结果")
            } label: {
                Label("重新测量设备性能", systemImage: "speedometer")
            }
            Button {
                ble.refreshPhotoLibrary()
            } label: {
                Label("重新读取照片列表", systemImage: "arrow.clockwise")
            }

            if ble.photoFormatState == .formatting {
                HStack(spacing: 10) {
                    ProgressView()
                    Text("正在格式化 SD 卡…")
                    Spacer()
                }
            } else {
                Button(role: .destructive) {
                    showFormatConfirm = true
                } label: {
                    Label("格式化 SD 卡", systemImage: "eraser")
                }
                .disabled(hasActiveUploads || !ble.isConnected)
            }
        } header: {
            Text("高级")
        } footer: {
            Text("自检会测量仪表播放动图的帧率与 JPEG 解码耗时，用来判断这台设备能不能开动图。")
                .font(.footnote)
            Text("格式化用于清理存储卡里的脏数据/残留文件：会删除仪表上的全部照片，无法恢复；正在上传时不可用。")
                .font(.footnote)
        }
    }

    // MARK: - 提示条

    @ViewBuilder
    private var toastSection: some View {
        if let toast {
            Section {
                Label(toast, systemImage: "checkmark.circle.fill")
                    .font(.footnote)
                    .foregroundStyle(.green)
            }
        }
    }

    private func showToast(_ text: String) {
        toast = text
        let token = UUID()
        toastToken = token
        Task {
            try? await Task.sleep(nanoseconds: 4_000_000_000)
            await MainActor.run {
                if toastToken == token { toast = nil }
            }
        }
    }

    // MARK: - 添加照片入口

    @ToolbarContentBuilder
    private var addToolbar: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) {
            if canAdd {
                addPhotosPicker(label: "添加照片")
            }
        }
    }

    private func addPhotosPicker(label: String) -> some View {
        PhotosPicker(
            selection: $pickerItems,
            maxSelectionCount: 20,
            selectionBehavior: .ordered,
            matching: .any(of: [.images, .videos]),
            photoLibrary: .shared()
        ) {
            Label(label, systemImage: "plus")
        }
    }

    // MARK: - 读取选中的素材

    private func loadSelection(_ items: [PhotosPickerItem]) async {
        loadingSelection = true
        defer { loadingSelection = false }

        var prepared: [PhotoPrepareItem] = []
        var failures = 0
        var reasons: [String] = []
        for item in items {
            do {
                prepared.append(try await Self.prepare(item))
            } catch {
                failures += 1
                reasons.append(error.localizedDescription)
                print("[照片] 素材读取失败：\(error.localizedDescription)")
            }
        }

        guard !prepared.isEmpty else {
            let detail = reasons.first.map { "：\($0)" } ?? ""
            alertMessage = "选中的素材都读不出来\(detail)。换几张试试，或先把照片下载到本机。"
            return
        }

        // 给每项一个人话名字，用在准备页和上传队列里
        for index in prepared.indices {
            prepared[index].displayName = "\(prepared[index].kindLabel) \(index + 1)"
        }
        prepareItems = prepared
        loadWarning = failures > 0 ? "有 \(failures) 个素材读取失败，已跳过。" : nil
        showingPrepare = true
    }

    /// 从 PhotosPickerItem 拿原始图片字节。
    ///
    /// 为什么不能只用 `loadTransferable(type: Data.self)`：实测部分素材（iCloud
    /// 未下载完 / HEIC 经系统转码的变体）会返回 nil，直接报"读不出来"。
    /// 这里先试 Data；不行再用 FileRepresentation 把系统给的临时文件复制出来读。
    private static func loadImageData(_ item: PhotosPickerItem) async -> Data? {
        if let data = try? await item.loadTransferable(type: Data.self), !data.isEmpty {
            return data
        }
        guard let file = try? await item.loadTransferable(type: PickedImageFile.self) else {
            return nil
        }
        defer { try? FileManager.default.removeItem(at: file.url) }
        return try? Data(contentsOf: file.url)
    }

    private static func prepare(_ item: PhotosPickerItem) async throws -> PhotoPrepareItem {
        let isMovie = item.supportedContentTypes.contains { $0.conforms(to: .movie) }
        if isMovie {
            guard let movie = try await item.loadTransferable(type: MovieTransferable.self) else {
                throw PhotoEncoderError.cannotReadVideo("无法从相册读取")
            }
            let duration = await PhotoEncoder.videoDuration(at: movie.url)
            guard duration > 0 else {
                // 读失败也要清掉已复制的临时视频，否则每次失败都会在沙盒里留一份
                try? FileManager.default.removeItem(at: movie.url)
                throw PhotoEncoderError.cannotReadVideo("时长为 0")
            }
            guard let raw = try? await PhotoEncoder.rawVideoFrame(at: movie.url, seconds: duration / 2),
                  let preview = PhotoEncoder.squareCGImage(raw, side: PhotoEncoder.photoSide) else {
                try? FileManager.default.removeItem(at: movie.url)
                throw PhotoEncoderError.cannotReadVideo("取不到画面")
            }
            let estimated = await Task.detached(priority: .utility) {
                PhotoEncoder.jpegData(from: preview, quality: PhotoEncoder.photoQuality)?.count ?? 0
            }.value
            return PhotoPrepareItem(
                sourceKind: .video,
                videoURL: movie.url,
                imageData: nil,
                sourceImage: raw,
                preview: preview,
                duration: duration,
                pickedTime: duration / 2,
                wantsAnimation: true,
                estimatedBytes: estimated
            )
        }

        guard let data = await Self.loadImageData(item) else {
            throw PhotoEncoderError.cannotReadSourceForCrop
        }
        // 解码/缩放走后台：一次选 10 张时在主线程解码会明显卡界面
        guard let decoded = await Task.detached(priority: .userInitiated, operation: { () -> (CGImage, CGImage, Int, Int)? in
            guard let upright = PhotoEncoder.uprightImage(from: data),
                  let preview = PhotoEncoder.squareCGImage(upright, side: PhotoEncoder.photoSide) else {
                return nil
            }
            let frameCount = PhotoEncoder.imageFrameCount(from: data)
            let estimatedBytes = PhotoEncoder.jpegData(from: preview,
                                                       quality: PhotoEncoder.photoQuality)?.count ?? 0
            return (upright, preview, frameCount, estimatedBytes)
        }).value else {
            throw PhotoEncoderError.cannotDecodeImage("尺寸/色彩空间不支持，或画面为空")
        }
        let (upright, preview, frameCount, estimated) = decoded
        let animated = frameCount > 1
        return PhotoPrepareItem(
            sourceKind: animated ? .animatedImage : .image,
            videoURL: nil,
            imageData: animated ? data : nil,
            sourceImage: upright,
            preview: preview,
            duration: 0,
            pickedTime: 0,
            wantsAnimation: animated,
            estimatedBytes: estimated
        )
    }

    // MARK: - 准备页回调

    private func handlePreparedUploads(_ requests: [PhotoUploadRequest],
                                       thumbnails: [PendingPhotoThumbnail]) {
        // 准备页停留期间可能掉线：这时入队会静默失败，必须先说清楚
        guard ble.isConnected, ble.photoSupported else {
            for request in requests { PGJEncoder.removeStagedFile(at: request.fileURL) }
            showingPrepare = false
            alertMessage = "没有发出去：和仪表的连接已经断开。重新连接后，重新选照片再试一次。"
            return
        }
        showingPrepare = false
        for request in requests {
            stagedFileURLs[request.id] = request.fileURL
        }
        pendingThumbnails.append(contentsOf: thumbnails)
        ble.enqueuePhotoUploads(requests)
        showToast("已加入上传队列：\(requests.count) 项")
    }

    /// 准备页关闭：删掉为取帧而复制的视频临时文件（编码产物已另存）
    private func cleanupPrepare() {
        for item in prepareItems {
            if let url = item.videoURL {
                try? FileManager.default.removeItem(at: url)
            }
        }
        prepareItems = []
        loadWarning = nil
    }

    // MARK: - 上传状态变化

    /// 格式化结果 → 用户可见反馈（成功 toast / 失败 alert），然后复位状态
    private func handleFormatStateChange(_ state: PhotoFormatState) {
        switch state {
        case .success:
            showToast("SD 卡已格式化，可以重新添加照片了")
            ble.clearPhotoFormatState()
        case .failed(let message):
            alertMessage = message
            ble.clearPhotoFormatState()
        case .idle, .formatting:
            break
        }
    }

    private func handleUploadsChange(_ uploads: [PhotoUploadProgress]) {
        for upload in uploads {
            let previous = seenUploadStates[upload.id]
            guard previous != upload.state else { continue }
            seenUploadStates[upload.id] = upload.state
            switch upload.state {
            case .done:
                showToast("「\(upload.displayName ?? "照片")」已传到仪表")
                // 设备清单可能稍后到（changed 通知），这里主动拉一次
                ble.refreshPhotoLibrary()
                removeStagedFile(id: upload.id)
            case .cancelled:
                removeStagedFile(id: upload.id)
            default:
                break
            }
        }
        // 队列里已经没有的 id：清掉记录与临时文件
        let aliveIDs = Set(uploads.map(\.id))
        seenUploadStates = seenUploadStates.filter { aliveIDs.contains($0.key) }
        for id in Array(stagedFileURLs.keys) where !aliveIDs.contains(id) {
            removeStagedFile(id: id)
        }
        assignPendingThumbnails()
    }

    private func removeStagedFile(id: String) {
        if let url = stagedFileURLs.removeValue(forKey: id) {
            PGJEncoder.removeStagedFile(at: url)
        }
    }

    /// 清掉"上传记录已清空"的临时文件
    private func pruneStagedFiles() {
        let aliveIDs = Set(ble.photoUploads.map(\.id))
        for id in Array(stagedFileURLs.keys) where !aliveIDs.contains(id) {
            removeStagedFile(id: id)
        }
    }

    // MARK: - 缩略图与设备照片 id 对齐

    /// 上传时我们还不知道设备分配的文件名，只能等清单刷新后按
    /// 「字节数 + 帧数 + 类型」匹配（设备是原样落盘，大小完全一致）。
    private func assignPendingThumbnails() {
        guard !pendingThumbnails.isEmpty,
              let library = ble.photoLibrary,
              !library.isLoading else { return }

        var matched: [UUID] = []
        for pending in pendingThumbnails {
            // 从后往前找：设备文件名是递增序号，新上传的排在清单末尾；
            // 这样即使两张照片字节数完全相同，也不会把缩略图挂到老照片上。
            let candidate = library.items.reversed().first { photo in
                photo.bytes == pending.bytes
                    && photo.frames == pending.frames
                    && photo.kind == pending.kind
                    && PhotoThumbnailCache.shared.thumbnail(for: photo.id) == nil
            }
            if let candidate {
                PhotoThumbnailCache.shared.store(pending.image, for: candidate.id)
                matched.append(pending.id)
            }
        }
        if !matched.isEmpty {
            pendingThumbnails.removeAll { matched.contains($0.id) }
        }

        // 设备上已经不存在的缩略图可以删 —— 但只在上传全部结束、清单非空时做，
        // 避免"刚上传完、清单还没回来"的窗口把新缩略图误删。
        if !library.items.isEmpty,
           ble.photoUploads.allSatisfy({ $0.isFinished }) {
            PhotoThumbnailCache.shared.prune(keeping: Set(library.items.map(\.id)))
        }
    }
}

// MARK: - 缩略图视图

private struct PhotoThumbnailView: View {
    let id: String

    var body: some View {
        Group {
            if let image = PhotoThumbnailCache.shared.thumbnail(for: id) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                ZStack {
                    Color(.secondarySystemFill)
                    Image(systemName: "photo")
                        .font(.system(size: 16))
                        .foregroundStyle(.secondary)
                }
            }
        }
        .frame(width: 44, height: 44)
        .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        .accessibilityHidden(true)
    }
}

// MARK: - 准备上传页

/// 从相册选完之后的"准备上传"页：
/// 默认居中裁切已经完成，用户可以逐张调整；视频在 LITE 档只能取一帧。
private struct PhotoPrepareView: View {

    @State private var items: [PhotoPrepareItem]
    let canAnimate: Bool
    let animationReason: String?
    let measuredRate: Double?
    let freeBytes: Int?
    let loadWarning: String?
    let onCancel: () -> Void
    let onEnqueue: ([PhotoUploadRequest], [PendingPhotoThumbnail]) -> Void

    @State private var editingItemID: UUID?
    @State private var isEncoding = false
    @State private var alertMessage: String?

    init(items: [PhotoPrepareItem],
         canAnimate: Bool,
         animationReason: String?,
         measuredRate: Double?,
         freeBytes: Int?,
         loadWarning: String?,
         onCancel: @escaping () -> Void,
         onEnqueue: @escaping ([PhotoUploadRequest], [PendingPhotoThumbnail]) -> Void) {
        _items = State(initialValue: items)
        self.canAnimate = canAnimate
        self.animationReason = animationReason
        self.measuredRate = measuredRate
        self.freeBytes = freeBytes
        self.loadWarning = loadWarning
        self.onCancel = onCancel
        self.onEnqueue = onEnqueue
    }

    private var totalBytes: Int {
        items.reduce(0) { $0 + estimatedBytes(for: $1) }
    }

    private var estimatedSeconds: Double {
        let rate = max(measuredRate ?? PhotoLimit.conservativeRate, 8 * 1024)
        return Double(totalBytes) / rate
    }

    var body: some View {
        NavigationStack {
            Form {
                if let loadWarning {
                    Section {
                        Label(loadWarning, systemImage: "exclamationmark.triangle")
                            .font(.footnote)
                            .foregroundStyle(.orange)
                    }
                }

                Section {
                    ForEach($items) { $item in
                        prepareRow(item: $item)
                    }
                } header: {
                    Text("将上传 \(items.count) 项")
                } footer: {
                    Text("默认已按圆屏居中裁切好。需要重新构图就点「调整」。")
                        .font(.footnote)
                }

                Section {
                    LabeledContent("预计总大小", value: PhotoFormat.bytes(totalBytes))
                    LabeledContent("预计时间", value: PhotoFormat.eta(estimatedSeconds))
                } footer: {
                    if let measuredRate {
                        Text("按本次连接实测 \(PhotoFormat.rate(measuredRate)) 估算，实际以进度为准。")
                            .font(.footnote)
                    } else {
                        Text("仪表还没实测过上传速度，按保守 \(PhotoFormat.rate(PhotoLimit.conservativeRate)) 估算；实际以进度为准。")
                            .font(.footnote)
                    }
                }

                if let animationReason, items.contains(where: { $0.isAnimatedSource }) {
                    Section {
                        Label(animationReason, systemImage: "info.circle")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                        Text("所以视频/动图只能取一帧当静态照片。")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    } header: {
                        Text("动图不可用")
                    }
                }
            }
            .navigationTitle("准备上传")
            .navigationBarTitleDisplayMode(.inline)
            .interactiveDismissDisabled(isEncoding)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消") { onCancel() }
                        .disabled(isEncoding)
                }
                ToolbarItem(placement: .confirmationAction) {
                    if isEncoding {
                        ProgressView()
                    } else {
                        Button("上传 \(items.count) 项") { submit() }
                            .fontWeight(.semibold)
                            .disabled(items.isEmpty)
                    }
                }
            }
            .sheet(isPresented: Binding(
                get: { editingItemID != nil },
                set: { if !$0 { editingItemID = nil } }
            )) {
                cropSheet
            }
            .alert(
                "提示",
                isPresented: Binding(
                    get: { alertMessage != nil },
                    set: { if !$0 { alertMessage = nil } }
                )
            ) {
                Button("好", role: .cancel) {}
            } message: {
                Text(alertMessage ?? "")
            }
        }
    }

    // MARK: - 单行

    @ViewBuilder
    private func prepareRow(item: Binding<PhotoPrepareItem>) -> some View {
        let value = item.wrappedValue
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                Image(uiImage: UIImage(cgImage: value.preview))
                    .resizable()
                    .scaledToFill()
                    .frame(width: 48, height: 48)
                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))

                VStack(alignment: .leading, spacing: 2) {
                    Text(value.displayName)
                        .font(.subheadline.weight(.medium))
                    Text(rowSubtitle(value))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Spacer()

                if !value.isAnimatedSource || !value.wantsAnimation {
                    Button("调整") { editingItemID = value.id }
                        .font(.caption)
                }
            }

            if value.sourceKind == .video {
                VStack(alignment: .leading, spacing: 2) {
                    HStack {
                        Text("取帧位置").font(.caption)
                        Spacer()
                        Text(PhotoFormat.clock(value.pickedTime) + " / " + PhotoFormat.clock(value.duration))
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                    Slider(
                        value: Binding(
                            get: { item.wrappedValue.pickedTime },
                            set: { item.wrappedValue.pickedTime = $0 }
                        ),
                        in: 0...max(value.duration, 0.1),
                        onEditingChanged: { editing in
                            if !editing { refreshVideoFrame(id: value.id, time: value.pickedTime) }
                        }
                    )
                    .disabled(value.wantsAnimation && canAnimate)
                    Text(value.wantsAnimation && canAnimate
                         ? "动图会取视频开头的画面；切到「静态照片」才能选帧。"
                         : "拖动选择视频里的一帧，默认取中间。")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }

            if value.isAnimatedSource {
                if canAnimate {
                    Picker("上传方式", selection: item.wantsAnimation) {
                        Text("动图（前 1.5 秒）").tag(true)
                        Text("静态照片（取一帧）").tag(false)
                    }
                    .pickerStyle(.segmented)
                    if value.wantsAnimation {
                        Text(animationDetailText(value))
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                } else if let animationReason {
                    Text(animationReason)
                        .font(.caption2)
                        .foregroundStyle(.orange)
                }
            }
        }
        .padding(.vertical, 4)
    }

    private func animationDetailText(_ item: PhotoPrepareItem) -> String {
        let size = PhotoFormat.bytes(PhotoEncoder.estimatedAnimatedBytes(side: PhotoEncoder.animSide,
                                                                        frames: PhotoEncoder.animMaxFrames))
        if item.sourceKind == .video {
            return "\(PhotoEncoder.animSide)×\(PhotoEncoder.animSide) · \(PhotoEncoder.animFPS) fps · 最多 \(PhotoEncoder.animMaxFrames) 帧 · 约 \(size)"
        }
        return "\(PhotoEncoder.animSide)×\(PhotoEncoder.animSide) · 按原帧率 · 最多 \(PhotoEncoder.animMaxFrames) 帧 · 约 \(size)"
    }

    private func rowSubtitle(_ item: PhotoPrepareItem) -> String {
        if item.wantsAnimation && canAnimate && item.isAnimatedSource {
            return "动图 · 约 \(PhotoFormat.bytes(PhotoEncoder.estimatedAnimatedBytes(side: PhotoEncoder.animSide, frames: PhotoEncoder.animMaxFrames)))"
        }
        if item.sourceKind == .video {
            return "视频 \(PhotoFormat.clock(item.duration)) · 取一帧作静态照片 · \(PhotoFormat.bytes(item.estimatedBytes))"
        }
        if item.sourceKind == .animatedImage {
            return "动图素材 · 取一帧作静态照片 · \(PhotoFormat.bytes(item.estimatedBytes))"
        }
        return "\(item.preview.width)×\(item.preview.height) 静态照片 · 约 \(PhotoFormat.bytes(item.estimatedBytes))"
    }

    // MARK: - 裁切

    @ViewBuilder
    private var cropSheet: some View {
        if let id = editingItemID,
           let index = items.firstIndex(where: { $0.id == id }) {
            PhotoCropView(
                source: items[index].sourceImage,
                initialTransform: items[index].transform,
                onCancel: { editingItemID = nil },
                onDone: { cropped, transform in
                    items[index].transform = transform
                    items[index].preview = cropped
                    items[index].estimatedBytes = PhotoEncoder.jpegData(
                        from: cropped, quality: PhotoEncoder.photoQuality
                    )?.count ?? items[index].estimatedBytes
                    editingItemID = nil
                }
            )
        }
    }

    /// 视频拖时间轴后重新取帧
    private func refreshVideoFrame(id: UUID, time: TimeInterval) {
        guard let index = items.firstIndex(where: { $0.id == id }),
              let url = items[index].videoURL else { return }
        Task {
            guard let raw = try? await PhotoEncoder.rawVideoFrame(at: url, seconds: time),
                  let square = PhotoEncoder.squareCGImage(raw, side: PhotoEncoder.photoSide) else { return }
            await MainActor.run {
                guard let current = items.firstIndex(where: { $0.id == id }) else { return }
                items[current].sourceImage = raw
                items[current].transform = .identity
                items[current].preview = square
                items[current].estimatedBytes = PhotoEncoder.jpegData(
                    from: square, quality: PhotoEncoder.photoQuality
                )?.count ?? items[current].estimatedBytes
            }
        }
    }

    // MARK: - 提交

    private func submit() {
        let total = totalBytes
        if let freeBytes, total > freeBytes {
            alertMessage = "仪表只剩 \(PhotoFormat.bytes(freeBytes))，这批需要 \(PhotoFormat.bytes(total))。先删掉几张照片再传吧。"
            return
        }

        isEncoding = true
        Task {
            var requests: [PhotoUploadRequest] = []
            var thumbnails: [PendingPhotoThumbnail] = []
            var failures: [String] = []
            for item in items {
                do {
                    let encoded = try await Self.encode(item: item, allowAnimation: canAnimate)
                    requests.append(encoded.request)
                    thumbnails.append(encoded.thumbnail)
                } catch {
                    failures.append("\(item.displayName)：\(error.localizedDescription)")
                }
            }
            isEncoding = false
            if !failures.isEmpty {
                alertMessage = "有 \(failures.count) 项没编码成功：\n" + failures.joined(separator: "\n")
                return
            }
            onEnqueue(requests, thumbnails)
        }
    }

    private func estimatedBytes(for item: PhotoPrepareItem) -> Int {
        if item.wantsAnimation && canAnimate && item.isAnimatedSource {
            return PhotoEncoder.estimatedAnimatedBytes(side: PhotoEncoder.animSide,
                                                       frames: PhotoEncoder.animMaxFrames)
        }
        return item.estimatedBytes
    }

    private static func encode(item: PhotoPrepareItem,
                               allowAnimation: Bool) async throws -> (request: PhotoUploadRequest,
                                                                     thumbnail: PendingPhotoThumbnail) {
        let requestID = UUID().uuidString

        if allowAnimation && item.wantsAnimation && item.isAnimatedSource {
            let clip: PhotoAnimationClip
            if item.sourceKind == .video {
                guard let url = item.videoURL else { throw PhotoEncoderError.noFrames }
                clip = try await PhotoEncoder.animationClip(fromVideo: url)
            } else {
                guard let data = item.imageData else { throw PhotoEncoderError.noFrames }
                clip = try PhotoEncoder.animationClip(fromImageData: data)
            }
            guard let first = clip.frames.first else { throw PhotoEncoderError.noFrames }
            let frames = clip.frames
            let fps = clip.fps
            // 调色板量化 + GIF 往返比较重，放到后台线程，避免"准备上传"界面卡住
            let pgi = try await Task.detached(priority: .userInitiated) {
                try PgiEncoder.encode(frames: frames, fps: fps)
            }.value
            let staged = try await Task.detached(priority: .userInitiated) {
                try PGJEncoder.stageAnimated(pgi: pgi,
                                             width: first.width,
                                             height: first.height,
                                             frameCount: frames.count,
                                             fps: fps)
            }.value
            let request = PhotoUploadRequest(
                id: requestID,
                fileURL: staged.fileURL,
                kind: .pgi,
                bytes: staged.byteCount,
                crc32: staged.crc32,
                width: staged.width,
                height: staged.height,
                frames: staged.frameCount,
                fps: staged.fps,
                displayName: item.displayName
            )
            let thumbnail = PendingPhotoThumbnail(bytes: staged.byteCount,
                                                  frames: staged.frameCount,
                                                  kind: 1,
                                                  image: first)
            return (request, thumbnail)
        }

        let preview = item.preview
        let jpeg = await Task.detached(priority: .userInitiated) {
            PhotoEncoder.jpegData(from: preview, quality: PhotoEncoder.photoQuality)
        }.value
        guard let jpeg else { throw PhotoEncoderError.cannotEncodeJPEG }
        let staged = try await Task.detached(priority: .userInitiated) {
            try PGJEncoder.stageStatic(jpeg: jpeg,
                                       width: preview.width,
                                       height: preview.height,
                                       quality: 80)
        }.value
        let request = PhotoUploadRequest(
            id: requestID,
            fileURL: staged.fileURL,
            kind: .pgj,
            bytes: staged.byteCount,
            crc32: staged.crc32,
            width: staged.width,
            height: staged.height,
            frames: 1,
            fps: 0,
            displayName: item.displayName
        )
        let thumbnail = PendingPhotoThumbnail(bytes: staged.byteCount,
                                              frames: 1,
                                              kind: 0,
                                              image: preview)
        return (request, thumbnail)
    }
}

// MARK: - 数据模型（App 侧）

/// 准备页里的一项素材。
struct PhotoPrepareItem: Identifiable {
    enum SourceKind: Equatable {
        case image
        case animatedImage
        case video
    }

    let id = UUID()
    let sourceKind: SourceKind
    /// 视频源文件（需要保留到编码完成；取消/关闭时删除）
    let videoURL: URL?
    /// 动图原始数据（FULL 档生成动图时重新解码用）
    let imageData: Data?
    /// 整帧源图（裁切编辑用，已摆正方向）
    var sourceImage: CGImage
    /// 当前预览（466 方形，已应用 transform）
    var preview: CGImage
    var transform: PhotoStudio.CropTransform = .identity
    var duration: TimeInterval = 0
    var pickedTime: TimeInterval = 0
    /// 默认是否生成动图（视频/动图素材默认开；LITE 档会被门控忽略）
    var wantsAnimation: Bool = false
    var estimatedBytes: Int = 0
    var displayName: String = ""

    var isAnimatedSource: Bool { sourceKind != .image }

    var kindLabel: String {
        switch sourceKind {
        case .image: return "照片"
        case .animatedImage: return "动图"
        case .video: return "视频"
        }
    }
}

/// 上传完成后、等待与设备照片 id 对齐的缩略图。
struct PendingPhotoThumbnail: Identifiable {
    let id = UUID()
    let bytes: Int
    let frames: Int
    /// 0 = PGJ / 1 = PGI（与设备清单 `k` 字段一致，便于按值匹配）
    let kind: Int
    let image: CGImage
}

/// 播放设置的本地草稿（真值来自设备 `DevicePhotoState`）
struct PhotoSettingsDraft {
    var slideshow = true
    var intervalSeconds = 30
    var shift = true
    var screenOffSeconds = 0
}

// MARK: - 常量 / 格式化

enum PhotoLimit {
    /// 动图需要的实测上传速率（设计稿 §7.1 第③层）
    static let animateMinRate: Double = 30 * 1024
    /// 没有实测速率时的保守估算值（设计稿 §5.2：LITE 下限 12 KB/s，留余量）
    static let conservativeRate: Double = 15 * 1024
}

private enum PhotoFormat {
    static func bytes(_ value: Int) -> String {
        if value >= 1024 * 1024 {
            return String(format: "%.1f MB", Double(value) / 1_048_576)
        }
        if value >= 1024 {
            return "\(value / 1024) KB"
        }
        return "\(value) B"
    }

    static func rate(_ bytesPerSecond: Double) -> String {
        guard bytesPerSecond.isFinite, bytesPerSecond > 0 else { return "-- KB/s" }
        if bytesPerSecond >= 1024 * 1024 {
            return String(format: "%.1f MB/s", bytesPerSecond / 1_048_576)
        }
        return String(format: "%.1f KB/s", bytesPerSecond / 1024)
    }

    static func eta(_ seconds: Double) -> String {
        guard seconds.isFinite, seconds > 0 else { return "几秒" }
        if seconds < 60 { return "约 \(Int(seconds.rounded(.up))) 秒" }
        return "约 \(Int((seconds / 60).rounded(.up))) 分钟"
    }

    static func interval(_ seconds: Int) -> String {
        if seconds < 60 { return "\(seconds) 秒" }
        let minutes = seconds / 60
        return minutes >= 60 ? "\(minutes / 60) 小时" : "\(minutes) 分钟"
    }

    static func screenOff(_ seconds: Int) -> String {
        seconds <= 0 ? "不自动息屏" : interval(seconds)
    }

    static func clock(_ seconds: TimeInterval) -> String {
        let total = max(0, Int(seconds.rounded()))
        return String(format: "%d:%02d", total / 60, total % 60)
    }
}

// MARK: - 视频选择（PhotosPicker → 临时文件）

/// 把 PhotosPicker 选到的视频落到临时文件（AVAsset 需要 URL）。
/// 图片的 FileRepresentation 兜底：系统把素材（含 iCloud 未下载的）落成临时文件，
/// 复制到沙盒后再自己读，避开 `Data.self` 偶发 nil 的问题。
struct PickedImageFile: Transferable {
    let url: URL

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(contentType: .image) { file in
            SentTransferredFile(file.url)
        } importing: { received in
            let ext = received.file.pathExtension.isEmpty ? "img" : received.file.pathExtension
            let dst = FileManager.default.temporaryDirectory
                .appendingPathComponent("picked-\(UUID().uuidString).\(ext)")
            try FileManager.default.copyItem(at: received.file, to: dst)
            return PickedImageFile(url: dst)
        }
    }
}

struct MovieTransferable: Transferable {
    let url: URL

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(contentType: .movie) { movie in
            SentTransferredFile(movie.url)
        } importing: { received in
            // 先卡体积再复制：复制要读写整份文件，等复制完再判断就白花了时间
            let size = (try? received.file.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            if size > PhotoStudio.maxVideoBytes {
                throw PhotoStudioError.videoTooLarge(maxBytes: PhotoStudio.maxVideoBytes)
            }
            let dst = FileManager.default.temporaryDirectory
                .appendingPathComponent("picked-\(UUID().uuidString).mov")
            try FileManager.default.copyItem(at: received.file, to: dst)
            return MovieTransferable(url: dst)
        }
    }
}
