# 导航网关（gateway/）

为 iPhone 侧代理**高德 Web 服务**：Key 只在服务端，App 不接触 Key。

> 📌 **先读** [`docs/nav-backend.md`](../docs/nav-backend.md) ——
> 含**项目基准定位**（个人开发学习 / 不装车 / 不商用 / 不分发）、
> 高德协议注意事项、部署与维护说明。
>
> ⚠️ 高德协议 §3.5 禁止存储/缓存服务数据 → 本网关**无任何缓存层**。

---

## 快速开始

**零依赖**（只用 Node 内置 `fetch` / `node:http` / `node:test`），无需 `npm install`。
要求 Node ≥ 18（开发环境实测 v24）。

```bash
cd gateway

# 1) 准备两个环境变量（都不要提交到 Git）
export AMAP_KEY=你的Web服务Key          # 控制台申请，平台必须选「Web 服务」
export GATEWAY_TOKEN=$(node -e "console.log(crypto.randomUUID())")

# 2) 起服务
npm start                                # 默认 127.0.0.1:8080

# 3) 自检
curl -s localhost:8080/health

# 4) 请求一条路线
curl -s -X POST localhost:8080/route \
  -H "authorization: Bearer $GATEWAY_TOKEN" \
  -H 'content-type: application/json' \
  -d '{"origin":{"lon":116.397428,"lat":39.90923},
       "destination":{"lon":116.481028,"lat":39.989643}}'
```

---

## 接口

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/health` | `{ ok, hasKey }` —— **不回显 Key 内容** |
| `POST` | `/route` | 路线规划，返回 RouteBundle |

### `POST /route`

请求（坐标是 **WGS84**，即手机 CoreLocation 的坐标系）：

```json
{
  "origin":      { "lon": 116.397428, "lat": 39.90923 },
  "destination": { "lon": 116.481028, "lat": 39.989643 },
  "strategy": 0,
  "routeId": "可选，便于对账"
}
```

需要请求头 `authorization: Bearer <GATEWAY_TOKEN>`。

响应（坐标已是 **GCJ-02**）：

```json
{
  "routeId": null,
  "distanceM": 10234,
  "durationS": 1200,
  "origin": { "lon": 116.403694, "lat": 39.910612 },
  "destination": { "lon": 116.487295, "lat": 39.990960 },
  "steps": [
    { "index": 0, "maneuver": 3, "distanceM": 120, "roadName": null,
      "instruction": "向东行驶120米右转",
      "polyline": [{ "lon": 116.403694, "lat": 39.910612 }] }
  ],
  "polyline": [{ "lon": 116.403694, "lat": 39.910612 }],
  "alternativeCount": 0
}
```

`maneuver` 取值与固件 `nav_maneuver_t` **逐值一致**：

| 值 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|----|---|---|---|---|---|---|---|---|
| 含义 | 未知 | 直行 | 左转 | 右转 | 掉头 | 靠左 | 靠右 | 到达 |

错误响应：

| 状态 | `error` | 含义 |
|------|---------|------|
| 400 | `invalid_coordinates` / `bad_request` | 坐标或 JSON 非法（会附 `expect` 说明格式）|
| 401 | `unauthorized` | token 缺失或错误 |
| 413 | `BODY_TOO_LARGE` | 请求体超过 8 KB |
| 429 | `rate_limited` | 触发限流 |
| 502 | `amap_error` | 高德业务错误（附 `info` / `infocode` / `hint`）|
| 502 | `upstream_failed` | 网络/超时（默认不暴露细节，`VERBOSE_ERRORS=1` 可开）|

---

## 环境变量

| 变量 | 必填 | 默认 | 说明 |
|------|------|------|------|
| `AMAP_KEY` | ✅ | — | 高德「Web 服务」类型 Key；**缺失则启动即失败** |
| `GATEWAY_TOKEN` | ✅ | — | 客户端访问令牌；**缺失则启动即失败**（CORS 不是认证）|
| `PORT` | | `8080` | 监听端口 |
| `RATE_LIMIT_PER_MIN` | | `60` | 每分钟请求上限（防误用刷爆配额）|
| `UPSTREAM_TIMEOUT_MS` | | `8000` | 单次上游超时 |
| `VERBOSE_ERRORS` | | — | 设为 `1` 才回传内部错误细节（仅本地调试用）|

---

## 测试

```bash
npm test          # 50 项，全部离线（不联网、不需要真实 Key）
```

覆盖：坐标转换（往返/境外直通/偏移量级）、动作映射（**顺序敏感**用例）、
响应归一化（字符串数字、解析失败不为 0、业务错误显式抛出）、
HTTP 层（认证/限流/参数校验/请求体限制/上游错误透传/**Key 不泄露**）。

---

## ⚠️ 接入真实 Key 后必做的两件事

本项目**没有实测过高德的真实响应**，因此有两处按"实测优先"设计：

### 1. 核对字段名

```bash
npm run probe -- --from 116.397428,39.90923 --to 116.481028,39.989643
```

它会打印**字段清单**与**原始响应**。对照 `src/normalize.js` 用到的字段：

- `route.paths[].distance` / `.duration`
- `route.paths[].steps[].instruction` / `.road_name` / `.step_distance` / `.polyline`

若实际字段名不同，改 `normalize.js` 并同步更新 `test/fixtures/`。

### 2. 填动作码表（若高德返回了数字码）

`src/maneuver.js` 的 `ACTION_CODE_MAP` **默认为空**，是刻意为之：
未实测就填数字码，可能把"左转"映射成"右转"——比显示"未知"危险得多。

`probe` 会列出每个 step 的 `action` 与 `instruction` 对应关系，照着实测填入即可：

```js
export const ACTION_CODE_MAP = Object.freeze({
  1: MANEUVER.LEFT,     // ← 以 probe 实测为准
  2: MANEUVER.RIGHT,
});
```

填完**记得同步更新** `test/maneuver.test.js`（其中有一条测试断言该表为空，
用于防止未实测就填写）。

---

## 设计要点

| 决定 | 原因 |
|------|------|
| 零依赖 | 减少供应链风险；Node 内置能力已足够 |
| Key 只在服务端 + Bearer Token | 客户端可被反编译；CORS 不是认证 |
| 无缓存 | 高德协议 §3.5 禁止存储/缓存服务数据 |
| 坐标转换在网关与端侧各有一份 | 手机每次定位都要转换（2Hz），不能走网络往返 |
| 无法解析的距离记为 `null` 而非 `0` | 距离 0 会让仪表显示"即将转向"，属危险误导 |
| 动作码表默认为空 | 未实测的映射可能给出**方向相反**的指引 |
| 启动即校验环境变量 | 避免带着空 Key 运行、到请求时才报错（易误判为网络问题）|

---

## 相关文件

| 文件 | 作用 |
|------|------|
| `src/coords.js` | WGS84 ↔ GCJ-02（纯逻辑）|
| `src/maneuver.js` | 动作映射（码表 + 文本兜底）|
| `src/normalize.js` | 高德响应 → RouteBundle |
| `src/amap.js` | 高德客户端（可注入 fetch）|
| `src/server.js` | HTTP 服务（认证/限流/无缓存）|
| `src/config.js` | 环境变量 + 日志脱敏 |
| `bin/probe.js` | 拉真实响应核对字段（**接 Key 后先跑这个**）|
