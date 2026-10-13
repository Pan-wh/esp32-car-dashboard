# nav_sim —— Mac 侧「模拟手机」

在 Mac 上**完整扮演 iPhone 在导航链路中的角色**，驱动真机仪表。

```
网关（真实高德路线）
      │ HTTPS
      ▼
Mac: nav_sim ── 导航核心（真实 Swift 实现）── 坐标往返 ── BLE ──▶ ESP32 仪表
```

## 为什么需要它

iPhone 负责：定位 → 匹配路线 → 算进度/偏航 → 经 BLE 下发。
**不需要 iPhone、不需要装车、不需要看屏幕**，就能验证除 iPhone 自身以外的整条链路。

| ✅ 可验证 | ❌ 不可验证 |
|----------|-----------|
| 网关取真实路线 | 真实 CoreLocation 的精度/频率/后台行为 |
| 坐标转换（WGS84↔GCJ-02）往返不破坏匹配 | App 界面（需 iPhone） |
| 导航核心的进度/偏航/到达判定 | 手机端权限、耗电、锁屏表现 |
| BLE 协议与固件解析 | |
| 仪表 UI 随行程更新 | |

> 它复用 `ios/.../Nav/NavCore.swift` 与 `Coords.swift` —— **测的就是将跑在 iPhone 上的那份代码**。

---

## 构建

```bash
cd <仓库根目录>
swiftc -O -o /tmp/nav_sim -framework CoreBluetooth \
  ios/CarDashboardPhone/CarDashboardPhone/Nav/NavCore.swift \
  ios/CarDashboardPhone/CarDashboardPhone/Nav/Coords.swift \
  tools/nav_sim/NavSimSupport.swift \
  tools/nav_sim/NavSimBLE.swift \
  tools/nav_sim/NavSimMain.swift
```

> ⚠️ 入口文件不能命名为 `main.swift`（会与 `@main` 冲突）。
> ⚠️ CoreBluetooth 回调走**独立队列**（`navsim.ble`）—— 主线程要休眠，
> 若回调排在主队列会死锁。

---

## 用法

```bash
# 1) 经网关取真实路线（需要网关在跑，见 gateway/README.md）
export GATEWAY_TOKEN=$(grep GATEWAY_TOKEN gateway/.env | cut -d= -f2)
/tmp/nav_sim --from 116.397428,39.90923 --to 116.461000,39.908700 --speed 60

# 2) 离线：先存一份网关响应，之后无需网络
curl -s -X POST localhost:8080/route \
  -H "authorization: Bearer $GATEWAY_TOKEN" -H 'content-type: application/json' \
  -d '{"origin":{"lon":116.397428,"lat":39.90923},"destination":{"lon":116.461000,"lat":39.908700}}' \
  -o /tmp/route.json
/tmp/nav_sim --route-file /tmp/route.json --speed 60

# 3) 只看会发什么，不连设备
/tmp/nav_sim --route-file /tmp/route.json --dry-run

# 4) 人为制造偏航，验证"重算"全流程（会自动调网关取新路线并续导）
/tmp/nav_sim --from 116.397428,39.90923 --to 116.461000,39.908700 \
             --offroute-from 300 --offroute-offset 80

# 5) 只看"进入 rerouting"，不真的去重算
/tmp/nav_sim --route-file /tmp/route.json --offroute-from 300 --offroute-offset 80 --no-reroute
```

### 重算流程会做什么

偏航确认后，模拟器会：

1. 从导航核心收到 `NavCommand.requestRoute`（核心**不自己发请求**）
2. 把核心的 GCJ-02 坐标转回 **WGS84**（网关约定），调网关取新路线
3. 用 `setRoute` 交回核心 → 状态回到 `active`，**立即显示新路线首个转向**
4. 里程表**跨重算累计**（不归零），但"路线进度"从 0 重新开始

失败时核心**保持在 `rerouting`**，并按 5 秒间隔重试（最多 5 次）。

`--from/--to` 是 **WGS84**（手机定位坐标系），网关会转 GCJ-02。

### 输出示例

```
   t(s)  里程(m)  状态        动作          距转向    垂距
  ─────────────────────────────────────────────────────────────
      1      17  active     left         25 m     0.0 m
      3      50  active     left         301 m    0.0 m
     ...
     19     317  active     left         47 m     79.7 m
     20     333  rerouting  unknown      --       78.7 m   ← 连续确认后判定偏航
```

---

## 设计要点

| 决定 | 原因 |
|------|------|
| 复用 iOS 的 `NavCore` / `Coords` | 测真实实现，而不是为验证另写一份（避免漂移） |
| 刻意走**坐标往返**：GCJ-02 → WGS84 → GCJ-02 | 模拟"CoreLocation 给 WGS84、App 转回 GCJ-02"的真实路径；顺带验证往返不破坏匹配（垂距应恒为 0） |
| 偏航用**当前航向的法向**偏移 | 用固定朝东偏移时，路线一转向偏移就变成"沿路线"方向，垂距忽大忽小（实测 43.9→12.6→3.5m），永远触发不了偏航 |
| 到达后连发 3 条 | 确保固件稳定进入到达显示（单条可能被后续状态覆盖） |
| 消息构造与 `docs/ble.md` 一致 | 无效距离发 `"dist_m":null`（明确无效），而不是省略字段 |

---

## 测试

```bash
cd tests && make test-nav-sim      # 25 项，纯逻辑，不需要设备/网络
```

覆盖：RouteBundle 解码、行车推进与钳制、法向偏航（垂距 ≈ 指定值）、
模拟循环产出的消息序列（可被 JSON 解析、长度在固件 512B 重组上限内、
覆盖 active 与 arrived、动作多样）。

> ⚠️ 夹具缺失时**判失败**而不是跳过 —— 曾出现"找不到夹具 → 4 项全跳过却显示通过"。

### 夹具

`testdata/route-bundle.json`（网关响应格式，**合成数据**）。
重新生成：

```bash
node gateway/test/make-structural-fixture.js \
     gateway/test/fixtures/amap-driving-real.json \
     gateway/test/fixtures/amap-driving-v3.json
node -e "import('./gateway/src/normalize.js').then(async m=>{
  const fs=await import('node:fs');
  const raw=JSON.parse(fs.readFileSync('gateway/test/fixtures/amap-driving-v3.json','utf8'));
  const b=m.normalizeRoute(raw,{routeId:'sim-test'});
  fs.writeFileSync('tools/nav_sim/testdata/route-bundle.json',JSON.stringify(b,null,2)+'\n');
})"
```

---

## 真机验证记录

已实测（2026-10-13）：Mac 模拟器经 BLE 驱动真机，串口日志确认固件逐条收到：

```
[BLE] 已连接 CarDashboard，Telemetry 特征就绪
[结果] 发送 20 条，失败 0 条

固件侧：
[导航] state=active maneuver=right dist=2268   ← 与模拟器打印完全一致
[导航] state=active maneuver=right dist=2059
[导航] BLE 断开 → 无数据                        ← 断连安全降级正确
```
