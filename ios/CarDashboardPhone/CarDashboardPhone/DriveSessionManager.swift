// DriveSessionManager.swift —— 驾驶会话管理（开始/结束 + 定时发送遥测）
//
// 职责：用户点「开始驾驶」后 → 申请/启动定位 + 连接 BLE + 以 1-2Hz 定时发送遥测；
//      点「结束驾驶」或数据无效时停止。发送频率集中在此，不逐帧发送。
//
// 同时记录「最近一次发送时间」与「定位数据时间」，用于在 UI 上区分
// 「定位数据旧」和「BLE 断连」两种失效（§5.4 / §6.3）。
import Combine
import Foundation
import CoreLocation

final class DriveSessionManager: ObservableObject {
    @Published var isDriving = false

    /// 最近一次成功提交发送的时刻（本机时间）
    @Published var lastSendTime: Date?
    /// 累计发送包数（调试用）
    @Published var sentCount = 0
    /// 最近一次收到的定位样本时间戳（CLLocation.timestamp，用于判断"定位数据旧"）
    @Published var lastLocationTime: Date?

    let location = LocationService()
    let ble = BLEClient()

    /*
     * ⚠️ 关键：把 ble / location 的 @Published 变化**转发**给本对象。
     *
     * 背景（真机踩坑）：ContentView 观察的是 `session`（本类），而 BLE 状态
     * 在 `session.ble` 里。`bleConnected` / `stateText` 都是**普通计算属性**，
     * 不会触发本类的 objectWillChange → SwiftUI 不重绘 → 界面永远停在
     * 首次渲染时的状态（表现为"强退重开后 BLE 状态一直是初始化中"，
     * 点一下别的按钮（改变 @Published）才刷新）。
     *
     * 修复：订阅子对象的 objectWillChange 并向上转发。
     */
    private var cancellables = Set<AnyCancellable>()

    private func forwardChanges() {
        ble.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)
        location.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)
    }

    init() {
        forwardChanges()
    }

    private var sendTimer: Timer?

    static let sendInterval: TimeInterval = 0.5 // 2 Hz；实测后再调

    // 供 UI 绑定
    var locationStatus: CLAuthorizationStatus { location.authorizationStatus }
    var bleConnected: Bool { ble.isConnected }
    var lastLocation: CLLocation? { location.lastLocation }
    var locationError: String? { location.lastError }

    /// 是否需要先向用户解释权限用途（首次启动、尚未决定时）
    var needsPermissionExplanation: Bool { location.authorizationStatus == .notDetermined }

    func requestPermissions() {
        location.requestWhenInUse()
    }

    func startDriving() {
        isDriving = true
        location.requestBackground() // 阶段4：申请后台定位能力（含始终授权）
        location.start()
        ble.scanAndConnect()
        sendTimer = Timer.scheduledTimer(withTimeInterval: Self.sendInterval, repeats: true) { [weak self] _ in
            self?.tick()
        }
    }

    func stopDriving() {
        isDriving = false
        sendTimer?.invalidate()
        sendTimer = nil
        location.stop()
    }

    private func tick() {
        let now = Date()
        let nowUtcMs = Int64(now.timeIntervalSince1970 * 1000)

        guard let packet = location.makePacket(nowUtcMs: nowUtcMs) else { return }

        ble.send(packet: packet)
        lastSendTime = now
        sentCount += 1
        lastLocationTime = location.lastLocation?.timestamp
    }

    /*
     * 发送节奏说明（真机长稳数据驱动）：
     *
     * 定时器固定 2Hz 触发（sendInterval = 0.5s），但**发送是否发生**取决于
     * makePacket() 是否有新鲜位置。iOS 的定位回调频率受信号影响，室内实测
     * 约 1.2–1.4Hz → 实际发送速率也随之降到 1.2–1.4Hz。
     *
     * 这曾导致仪表出现**瞬态 STALE**：若某次定位回调延迟，1.2Hz 下累计
     * 3 个周期（2.5s）无包就会触及仪表的 2.5s 超时阈值。
     *
     * 采用「固定定时器 + 新鲜度校验」而非"每个定位回调都立刻发"是有意为之：
     *   - 避免定位回调突发（iOS 补发多个缓存点）时瞬时刷屏；
     *   - 位置超 3s 陈旧则不发（§6.3 不得用旧值冒充当前值）。
     *
     * 仪表侧的应对：2.5s 阈值 + 数据龄单调增长提示，属于正确的降级表现。
     * 如需进一步减少瞬态 STALE，应优先提高 iOS 定位回调频率
     * （desiredAccuracy 已为 BestForNavigation，distanceFilter 已为 None，
     *  故只能依赖信号质量；室外/行驶中会自然恢复到 ≈2Hz）。
     */
}
