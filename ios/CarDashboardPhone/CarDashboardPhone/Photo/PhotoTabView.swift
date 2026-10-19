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
                    ZStack {
                        Color.black
                        if let preview {
                            Image(uiImage: preview)
                                .resizable()
                                .scaledToFill()
                        } else {
                            Text("还没有素材")
                                .foregroundStyle(.secondary)
                        }
                    }
                    .frame(height: 300)
                    .frame(maxWidth: .infinity)
                    // 圆形遮罩：与设备的圆屏一致
                    .clipShape(Circle())
                    .overlay(Circle().strokeBorder(.white.opacity(0.15), lineWidth: 1))
                    .padding(.vertical, 6)

                    if !status.isEmpty {
                        Text(status)
                            .font(.footnote)
                            .foregroundStyle(busy ? .orange : .secondary)
                    }
                }

                Section("参数") {
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
                    Text("导出的 GIF 会存进系统相册，可直接查看播放效果。\n（传到设备需要 Wi-Fi 上传，在后续版本接入。）")
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

    // MARK: - 载入

    private func load(_ item: PhotosPickerItem) async {
        await MainActor.run {
            busy = true
            status = "正在读取…"
            gifData = nil
            preview = nil
            frameCount = 0
        }
        defer { Task { @MainActor in busy = false } }

        do {
            let frames = try await loadFrames(from: item)
            await MainActor.run {
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
            return try await PhotoStudio.frames(fromVideo: movie.url, size: size, fps: fps)
        }
        // 图片/动图：直接读原始数据（GIF 的帧信息在数据里）
        if let data = try await item.loadTransferable(type: Data.self) {
            return try PhotoStudio.frames(from: data, maxSize: size)
        }
        throw PhotoStudioError.cannotReadSource
    }

    private func exportGIF() async {
        guard let gifData else { return }
        await MainActor.run {
            busy = true
            status = "正在保存到相册…"
        }
        do {
            try await PhotoStudio.saveToPhotoLibrary(gifData)
            await MainActor.run {
                status = "已保存到相册（\(frameCount) 帧）"
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
            // 系统给的临时文件可能在导入后就被回收，复制到自己的临时目录
            let dst = FileManager.default.temporaryDirectory
                .appendingPathComponent("picked-\(UUID().uuidString).mov")
            try FileManager.default.copyItem(at: received.file, to: dst)
            return MovieTransferable(url: dst)
        }
    }
}
