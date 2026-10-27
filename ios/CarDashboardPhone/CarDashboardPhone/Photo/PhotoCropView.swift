//
//  PhotoCropView.swift
//  CarDashboardPhone
//
//  单张照片的圆屏裁切编辑（466×466）。
//
//  ## 为什么单独做一个可复用页面
//
//  添加照片是"多选 + 默认居中裁切"：大多数照片直接默认裁切就能用，
//  只有需要构图时点开单张调整。因此裁切必须是**独立、可复用**的一页，
//  而不是塞在列表里 —— 列表里塞手势会互相抢触摸。
//
//  ## 所见即所得
//
//  预览和最终导出用**同一套变换语义**（`PhotoStudio.CropTransform`）：
//  预览上的"捏合缩放 / 拖动 / 旋转"直接就是导出时的参数，
//  不存在"预览看着好、导出变样"的问题。
//
//  ## 手势约定
//
//  - 捏合：以画布中心缩放（1×–4×），超出范围自动夹住；
//  - 拖动：平移画面，松手时把画面夹回"仍能铺满圆"的范围，不留黑边；
//  - 旋转：按 90° 步进（自由角度在圆形裁切里没有意义，还会露出黑角）。
//

import SwiftUI

struct PhotoCropView: View {

    /// 原始画面（已是摆正方向的正方形或任意比例图）
    let source: CGImage
    /// 初始变换（从"上次调整"进入时可带上；默认 identity）
    let initialTransform: PhotoStudio.CropTransform
    let onCancel: () -> Void
    /// 完成回调：给出裁切结果 + 使用的变换。
    /// 带上变换是为了**非破坏性再编辑**：再次打开时能回到上次的构图，
    /// 而不是在已经裁过的图上继续裁（那样用户再也放不回去）。
    let onDone: (CGImage, PhotoStudio.CropTransform) -> Void

    /// 预览用的正方形底图（只生成一次，避免每次手势都重裁原图）
    @State private var basePreview: UIImage?
    /// 已提交的缩放 / 平移（平移是**相对边长的比例**，与导出尺寸无关）
    @State private var committedZoom: CGFloat = 1
    @State private var committedPan: CGSize = .zero
    /// 已提交的旋转（度，90 的整数倍）
    @State private var rotation: CGFloat = 0
    /// 手势进行中的实时值
    @State private var liveZoom: CGFloat = 1
    @State private var livePan: CGSize = .zero

    init(source: CGImage,
         initialTransform: PhotoStudio.CropTransform = .identity,
         onCancel: @escaping () -> Void,
         onDone: @escaping (CGImage, PhotoStudio.CropTransform) -> Void) {
        self.source = source
        self.initialTransform = initialTransform
        self.onCancel = onCancel
        self.onDone = onDone
        _committedZoom = State(initialValue: initialTransform.zoom)
        _liveZoom = State(initialValue: initialTransform.zoom)
        _committedPan = State(initialValue: CGSize(width: initialTransform.panX,
                                                   height: initialTransform.panY))
        _livePan = State(initialValue: CGSize(width: initialTransform.panX,
                                              height: initialTransform.panY))
        _rotation = State(initialValue: initialTransform.rotation)
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                preview
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                controls
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("调整照片")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("取消") { onCancel() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("完成") { finish() }
                        .fontWeight(.semibold)
                }
            }
            .onAppear(perform: preparePreview)
        }
    }

    // MARK: - 预览

    private var preview: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height) - 32
            ZStack {
                Color.black
                if let basePreview {
                    Image(uiImage: basePreview)
                        .resizable()
                        .scaledToFill()
                        .frame(width: side, height: side)
                        .rotationEffect(.degrees(rotation))
                        .scaleEffect(liveZoom)
                        .offset(x: livePan.width * side, y: livePan.height * side)
                        .clipShape(Circle())
                        .overlay(
                            Circle().strokeBorder(Color.white.opacity(0.25), lineWidth: 1)
                        )
                        // 手势挂在**圆形预览**上：只有手指落在图片上才响应，
                        // 避免在空白背景上拖动也改画面。
                        .gesture(simultaneousGestures(side: side))
                        .accessibilityLabel("照片预览，可双指缩放、拖动、旋转")
                } else {
                    ProgressView()
                        .tint(.white)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color.black)
        }
        .padding(.top, 8)
    }

    // MARK: - 底部操作

    private var controls: some View {
        VStack(spacing: 12) {
            HStack(spacing: 24) {
                Button {
                    rotation = (rotation + 90).truncatingRemainder(dividingBy: 360)
                } label: {
                    Label("旋转 90°", systemImage: "rotate.right")
                }
                Button {
                    reset()
                } label: {
                    Label("重置", systemImage: "arrow.counterclockwise")
                }
            }
            .buttonStyle(.bordered)

            Text("双指捏合缩放，单指拖动调整构图。设备是圆屏，四角会被裁掉。")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 24)
        }
        .padding(.vertical, 16)
        .frame(maxWidth: .infinity)
        .background(.regularMaterial)
    }

    // MARK: - 手势

    private func simultaneousGestures(side: CGFloat) -> some Gesture {
        let magnify = MagnificationGesture()
            .onChanged { value in
                liveZoom = clampedZoom(committedZoom * value)
            }
            .onEnded { _ in
                committedZoom = liveZoom
                committedPan = clampedPan(livePan, zoom: committedZoom)
                livePan = committedPan
            }

        let drag = DragGesture()
            .onChanged { value in
                let candidate = CGSize(
                    width: committedPan.width + value.translation.width / max(side, 1),
                    height: committedPan.height + value.translation.height / max(side, 1)
                )
                livePan = clampedPan(candidate, zoom: liveZoom)
            }
            .onEnded { _ in
                committedPan = livePan
            }

        return magnify.simultaneously(with: drag)
    }

    private func clampedZoom(_ value: CGFloat) -> CGFloat {
        min(max(value, PhotoStudio.CropTransform.minZoom),
            PhotoStudio.CropTransform.maxZoom)
    }

    /// 平移上限：图片放大后多出来的部分的一半（以边长比例表示）。
    /// 平移超过这个范围就会露出黑边，所以夹住。
    private func clampedPan(_ value: CGSize, zoom: CGFloat) -> CGSize {
        let limit = max(0, (zoom - 1) / 2)
        return CGSize(width: min(max(value.width, -limit), limit),
                      height: min(max(value.height, -limit), limit))
    }

    // MARK: - 生成 / 导出

    private func preparePreview() {
        guard basePreview == nil else { return }
        // 底图就是"无手势时的 466 方形居中裁切"；之后手势只是在此基础上
        // 缩放/平移/旋转，与导出时的计算完全一致。
        basePreview = PhotoStudio.squareCropped(
            UIImage(cgImage: source),
            size: PhotoEncoder.photoSide
        )
    }

    private func reset() {
        committedZoom = 1
        committedPan = .zero
        liveZoom = 1
        livePan = .zero
        rotation = 0
    }

    private func finish() {
        let transform = PhotoStudio.CropTransform(
            zoom: committedZoom,
            panX: committedPan.width,
            panY: committedPan.height,
            rotation: rotation
        )
        let image = PhotoStudio.squareCropped(
            UIImage(cgImage: source),
            size: PhotoEncoder.photoSide,
            transform: transform
        )
        guard let cg = image.cgImage else { return }
        onDone(cg, transform)
    }
}
