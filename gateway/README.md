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

# 1) 复制模板并填入真实值（.env 已被 .gitignore 忽略，不会误提交）
cp .env.example .env
#    然后编辑 .env：
#      AMAP_KEY=        ← 控制台申请，服务平台必须选「Web 服务」
#      GATEWAY_TOKEN=   ← 自己生成：node -e "console.log(crypto.randomUUID())"

# 2) 起服务（会自动加载 .env，无需手动 export）
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
| `GET` | `/health` | `{ ok, hasKey, amapCallsToday }` —— **不回显 Key 内容** |
| `POST` | `/route` | 驾车路线规划，返回 RouteBundle |
| `POST` | `/routes` | **多方案路线**：返回若干条真正不同的候选供选择 |
| `POST` | `/geocode` | 地址文本 → 坐标（中文地名目的地）|
| `POST` | `/regeo` | 坐标 → 地址 / **当前道路名** |
| `POST` | `/traffic` | 周边**拥堵**情况（⚠️ 不是红绿灯）|
| `POST` | `/around` | 周边搜索（加油站 / 服务区等）|
| `POST` | `/tips` | 地址联想（搜索框输入提示）|
| `POST` | `/bicycling` | 骑行路线规划 |
| `POST` | `/mapwindow` | 周边地图窗口（±500m 路网/建筑几何）|

> 除 `/health` 外**全部需要** `authorization: Bearer <GATEWAY_TOKEN>`。
> 所有接口的**入参与返回坐标一律 WGS84**（网关内部往返 GCJ-02）。

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

### `POST /routes` —— 多方案路线

**请求**与 `/route` 完全相同（同样可选 `strategy`，但此处会被忽略 —— 本接口
自己用固定的三个策略去取）。

**响应**：

```json
{
  "origin":      { "lon": 116.403694, "lat": 39.910612 },
  "destination": { "lon": 116.487295, "lat": 39.990960 },
  "candidates": [
    { "strategy": 0, "distanceM": 16847, "durationS": 2349,
      "steps": [ ... ], "polyline": [ ... ] },
    { "strategy": 6, "distanceM": 16056, "durationS": 2376, "steps": [ ... ], "polyline": [ ... ] },
    { "strategy": 2, "distanceM": 15173, "durationS": 2859, "steps": [ ... ], "polyline": [ ... ] }
  ],
  "triedStrategies": [0, 2, 6],
  "okStrategies":    [0, 2, 6]
}
```

每个 `candidates[]` 元素与 `/route` 的响应**同构**（`strategy` 是额外字段），
所以客户端选定后可以直接当作一条路线使用，**不需要再请求一次**。

> #### 这个接口为什么存在（值得记下来）
>
> **高德 v3 驾车 API 一次只返回 1 条路径**。实测 `strategy`
> 0/1/2/6/8/35/36/37 **全部**只返回 `route.paths.length === 1` ——
> 所以"多方案对比"不可能靠它一次给。
>
> **但换 strategy 会得到真正不同的路线**。同一起终点实测：
>
> | 策略 | 含义 | 距离 | 耗时 |
> |---|---|---|---|
> | 0 | 速度优先 | 16847 m | 2349 s |
> | 6 | 不走高速 | 16056 m | 2376 s |
> | 2 | 距离最短 | 15173 m | 2859 s |
>
> 于是本接口的做法是：**串行**用这三个策略各请求一次，按
> **距离 + 耗时去重**后合并返回。
>
> ⚠️ **去重是必须的**：实测策略 0/1/8/35/36/37 给出的是**完全相同**的一条路，
> 不去重客户端会看到三条一模一样的"选项"。
>
> **代价与约束**：
> - 一次调用 = 服务端 **3 次**高德请求（换来"真正可选 + 无需二次请求"）；
> - 高德对单账号有 **QPS 限制**（实测快速连打会返回
>   `10021 CUQPS_HAS_EXCEEDED_THE_LIMIT`），故串行 + 350ms 间隔；
> - **单个策略失败不影响其余**：有几个成功的就给几个，全失败才返回 502。

错误响应：

| 状态 | `error` | 含义 |
|------|---------|------|
| 400 | `invalid_coordinates` | 坐标缺失或格式不对 |
| 502 | `no_route_candidate` | 所有策略都没取到路线（配额 / QPS / 不可达）|

### `POST /regeo` —— 坐标 → 当前道路名

```json
// 请求（WGS84）
{ "lon": 116.397428, "lat": 39.90923, "radius": 0 }
```

```json
// 响应
{
  "formattedAddress": "北京市东城区东华门街道北湾子胡同",
  "road": "北湾子胡同",
  "poiName": null,
  "district": "东城区",
  "city": null,
  "province": "北京市"
}
```

`road` 为 `null` 表示该处无道路名（高德此时返回**空数组**，已容错）。
仪表可直接显示 "北湾子胡同"。

### `POST /traffic` —— 周边拥堵（⚠️ **不是红绿灯**）

```json
// 请求（WGS84）；halfMeters 默认 500（即 1km×1km）
{ "lon": 116.397428, "lat": 39.90923, "halfMeters": 500, "level": 6 }
```

```json
// 响应
{
  "status": "拥堵",
  "statusCode": "3",
  "description": "中度拥堵",
  "roads": [
    { "name": "东华门大街", "status": "拥堵", "statusCode": "3",
      "direction": "从北河沿大街到南池子大街",
      "lengthM": 317, "speedKmh": 5, "congestedLengthM": 317 }
  ],
  "count": 1
}
```

**这是拥堵信息，不是红绿灯。** 红绿灯的相位/倒计时不在任何高德 Web 服务 API 中
（属地图客户端与导航 SDK 能力），详见 [`docs/nav-backend.md`](../docs/nav-backend.md)。

### `POST /around` —— 周边搜索

```json
// 请求（WGS84）；keywords 可省略
{ "lon": 116.397428, "lat": 39.90923, "keywords": "加油站", "radius": 3000, "limit": 20 }
```

```json
// 响应（坐标已转回 **WGS84**）
{ "pois": [
  { "name": "中国石化加油站(北京站)", "type": "汽车服务;加油站;中国石化",
    "distanceM": 1797, "lon": 116.416618, "lat": 39.902593, "address": "" }
] }
```

无坐标的 POI 会被跳过（对导航无用）。

### `POST /tips` —— 地址联想

```json
// 请求
{ "keywords": "天安", "city": "" }
```

```json
// 响应（有坐标的已转 WGS84；无坐标的候选保留但 lon/lat 为 null）
{ "tips": [
  { "name": "天安门", "district": "北京市东城区", "adcode": "110101",
    "lon": 116.39122182304565, "lat": 39.90778574896749 }
] }
```

### `POST /bicycling` —— 骑行路线

请求与 `/route` 相同（WGS84 的 `origin` / `destination`）。
响应结构与 RouteBundle 一致，额外带 `"mode": "bicycling"`。

> ⚠️ 骑行用的是高德 **v4** 接口，路径在 `data.paths[]`（驾车是 `route.paths[]`），
> 且用 `errcode: 0` 表示成功（驾车是 `status: "1"`）。已单独归一化。

---

## 环境变量

> 推荐写在 `gateway/.env`（由 `--env-file-if-exists` 自动加载，**已被 gitignore**）；
> 也可用 `export` 直接注入。缺 `AMAP_KEY` 或 `GATEWAY_TOKEN` 时**启动即失败**，
> 不会带着空配置跑起来。

| 变量 | 必填 | 默认 | 说明 |
|------|------|------|------|
| `AMAP_KEY` | ✅ | — | 高德「Web 服务」类型 Key；**缺失则启动即失败** |
| `GATEWAY_TOKEN` | ✅ | — | 客户端访问令牌；**缺失则启动即失败**（CORS 不是认证）|
| `HOST` | | `127.0.0.1` | 监听地址。**默认只回环**（安全默认；反代/隧道场景用默认值即可）|
| `PORT` | | `8080` | 监听端口 |
| `TRUST_PROXY` | | `0` | 信任的反代跳数（默认不信任 `X-Forwarded-For`，避免被伪造）|
| `RATE_LIMIT_PER_MIN` | | `60` | 每分钟请求上限（防误用刷爆配额）|
| `UPSTREAM_TIMEOUT_MS` | | `8000` | 单次上游超时 |
| `MAP_CACHE_DIR` | | `.cache/map-tiles` | 地图瓦片缓存目录（需可写，已 gitignore）|
| `VERBOSE_ERRORS` | | — | 设为 `1` 才回传内部错误细节（仅本地调试用）|

> ⚠️ **HTTPS 由外部提供**（反向代理或隧道），网关自身只监听明文 HTTP、不处理 TLS。
> 部署要求与验收步骤见 [`DEPLOY.md`](DEPLOY.md)。

---

## 测试

```bash
npm test          # 88 项，全部离线（不联网、不需要真实 Key）
```

覆盖：坐标转换（往返/境外直通/偏移量级）、动作映射（**顺序敏感**用例）、
响应归一化（字符串数字、解析失败不为 0、业务错误显式抛出）、
HTTP 层（认证/限流/参数校验/请求体限制/上游错误透传/**Key 不泄露**）、
以及新增 5 个接口（坐标是否真的转 GCJ-02 / 是否转回 WGS84 / 限幅 / 容错）。

### 用真实 Key 核实新接口字段名

```bash
node --env-file-if-exists=.env bin/probe-new.js
```

本项目的一条硬规矩是**字段名以实测为准，不凭印象填写**。
上面这个探针会用真实 Key 跑一遍 5 个新接口并打印字段清单。
**它在开发时抓出了 3 个凭印象写就会错的地方**（见下）。

---

## ✅ 真实响应已核实（2026-10-13）

**已用真实 Key 完成核实**，以下是核实结果 —— 保留本节是因为它记录了
"为什么字段名是这样"，将来上游改动时便于比对。

| 项 | 结果 |
|----|------|
| `steps[].distance` | ✅ 实际字段名就是 `distance`（**不是** `step_distance`）|
| `steps[].road` | ✅ 实际是 `road`（**不是** `road_name`）|
| `steps[].action` | ✅ **中文文本**（"左转"/"靠左"/"左转调头"…），**不是数字码** |
| 类型一致性 | ⚠️ `action` 可为字符串**或空数组**；`assistant_action` 可为数组**或字符串**；路名为空时**省略 `road` 键** |

**回归保护**：`test/fixtures/amap-driving-v3.json` 保留了真实响应的
**字段名与结构**（数值为合成数据，见下方合规说明），字段名再变会立刻测试失败。

---

## ✅ 新增 5 接口的实测更正（2026-10-15）

用真实 Key 跑 `bin/probe-new.js` 后，**推翻了 3 处凭印象写就会错的地方**。
记录在此，避免将来有人"照直觉改回去"。

| # | 原先的（错误）假设 | 实测结果 | 后果 |
|---|-------------------|---------|------|
| 1 | 骑行 v4 没有 `action`，maneuver 应为"未知" | `steps[].action` **就是中文**（"右转"），与驾车一致 | 若写死 UNKNOWN，骑行模式下仪表**完全不显示转向提示** |
| 2 | `trafficinfo.evaluation.status` 是中文等级 | 是**数字码** `"3"`，中文在 `description`（"中度拥堵"）| 直接透出会得到 `"3"` 这种不可读值 |
| 3 | `roads[]` 有 `congestion_distance` 字段（拥堵里程）| **该字段不存在**。实际字段：`name/status/direction/angle/speed/lcodes/polyline` | 拥堵里程恒为 `null`（现已改为**由 `polyline` 折线算出**）|

补充实测结论：

- **`extensions=base` 时 `roads` 恒为空数组** —— 要拿具体拥堵路段必须用 `all`。
- **骑行 v4 用 `errcode: 0` 表示成功**，没有 `status` 字段 ——
  直接套 `assertAmapOk`（要求 `status === "1"`）会把成功响应判成失败、恒返回 502。
- 骑行接口路径是 `/v4/direction/bicycling`（驾车是 `/v3/...`）。

### 若上游再次改动，如何重新核实

```bash
npm run probe -- --from <lon,lat> --to <lon,lat> --save test/fixtures/amap-driving-real.json
# 对照输出里的「字段清单」与 src/normalize.js 用到的字段名
# 核实后重新生成结构夹具（真实响应不入库）：
node test/make-structural-fixture.js test/fixtures/amap-driving-real.json \
                                     test/fixtures/amap-driving-v3.json
```

### 合规说明（协议 §3.5 不得直接存储服务数据）

`probe --save` 落盘的真实响应**不入库**（`*-real.json` 已 gitignore）。
入库的是**结构夹具**：字段名/层级/`action` 词表来自真实响应，
坐标、路名、距离、折线均为**合成数据**。
回归保护针对字段名与结构，用合成值同样有效。

---

## 附：设计上"不猜"的两处（保留作为背景）

### 1. 核对字段名

```bash
npm run probe -- --from 116.397428,39.90923 --to 116.481028,39.989643
```

它会打印**字段清单**与**原始响应**。对照 `src/normalize.js` 用到的字段：

- `route.paths[].distance` / `.duration`
- `route.paths[].steps[].instruction` / `.road_name` / `.step_distance` / `.polyline`

若实际字段名不同，改 `normalize.js` 并同步更新 `test/fixtures/`。

**动作映射已经是实测过的**（`ACTION_TEXT_MAP` 收录了真实响应中出现过的
全部 action 取值，每项标注了 [实测] / [推断]）。`test/maneuver.test.js` 里有一条
测试**逐项断言实测词表命中**；若高德改词，该测试会失败并直接指出是哪一项。

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
| `src/normalize.js` | 高德响应 → RouteBundle（含骑行 v4）|
| `src/amap.js` | 高德客户端：驾车/地理编码/逆地理/交通态势/周边/输入提示/骑行 |
| `src/mapwindow.js` | 周边地图窗口（OSM 矢量瓦片，不消耗高德额度）|
| `src/server.js` | HTTP 服务（认证/限流/无缓存）|
| `src/config.js` | 环境变量 + 日志脱敏 |
| `bin/probe.js` | 拉真实驾车响应核对字段（**接 Key 后先跑这个**）|
| `bin/probe-new.js` | 核对逆地理/态势/周边/提示/骑行 5 个接口的字段名 |
