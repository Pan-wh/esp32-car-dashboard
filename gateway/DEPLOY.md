# 导航网关：部署与运维

> **本文是自包含的。** 交给运维或 AI 执行时，**只读这一份**即可完成
> 部署、升级、验收与排障 —— 不需要读代码。
>
> - 想知道**每个接口的请求 / 响应字段** → 见 [`README.md`](README.md)
> - 想知道**为什么这么设计**（高德协议约束、坐标转换等）→ 见
>   [`../docs/nav-backend.md`](../docs/nav-backend.md)

---

## 〇、给执行者的任务书

### 你要做什么

把本仓库 `gateway/` 目录部署成**可通过 HTTPS 访问、供 iPhone App 调用**的线上服务。

```
iPhone App ──(HTTPS + Bearer 令牌)──▶ 本网关 ──(AMAP_KEY)──▶ 高德 Web 服务
```

**高德 Key 只存在于服务器**，App 永远接触不到它。

### 开始前必须先确认（缺一不可）

| # | 前提 | 怎么确认 |
|---|------|---------|
| 1 | 一台有公网访问的 Linux 服务器 | — |
| 2 | **Node.js ≥ 18** | `node -v` |
| 3 | 一个**高德「Web 服务」类型 Key** | 见 §二.3。⚠️ 服务平台选错会一直返回 `10009` |
| 4 | 一个自定义访问令牌 | `node -e "console.log(crypto.randomUUID())"` |
| 5 | HTTPS 的提供方式 | 反向代理（推荐）或隧道，见 §六 |

### 交付标准（全部满足才算完成）

1. `GET https://<你的域名>/health` 返回 `{"ok":true,"hasKey":true,...}`
2. **不带** `authorization` 头访问业务接口 → **401**
3. §九 列出的**全部**接口都返回 200（用真实坐标抽查）
4. **明文 `http://<公网IP>:8080` 从外网不可达**（超时 / 拒绝）
5. 服务设为开机自启，重启服务器后仍可用

验收命令见 §七，逐条可复制。

### ⚠️ 不要做的事

- **不要**把 `.env`、Key、Token 提交进 Git（`.env` 已在 `.gitignore` 里）
- **不要**在日志或聊天里粘贴真实 Key / Token
- **不要**把 `AMAP_KEY` 写进 App 或任何客户端
- **不要**为了省事把 `HOST` 设成 `0.0.0.0` 却不做反代 ——
  那会暴露一个无 TLS 的明文入口（§六有说明）

---

## 一、这个网关是什么

为 iPhone 侧代理**高德 Web 服务**。存在三个理由：

1. **Key 不下发**：Key 只在服务端，App 拿不到，泄漏面最小。
2. **坐标归一**：高德用 GCJ-02，手机定位是 WGS84。网关负责**双向转换**，
   App 与固件全都说 WGS84，不必各自实现一遍（北京地区两者差约 556 m）。
3. **配额可控**：`/health` 输出今日各接口调用次数，随时能看用量。

`GATEWAY_TOKEN` 是**本项目自建**的访问令牌，与高德无关。作用是挡住
"能连到网关的人"盗用服务端的 `AMAP_KEY` 刷配额。它不是强认证，局限见 §十一。

---

## 二、前置条件

### 1. 服务器

任意能跑 Node 18+ 的 Linux。目录建议固定为 `/opt/esp32-car-dashboard`。

### 2. Node.js ≥ 18

```bash
node -v          # 必须是 v18 或更高
```

<details>
<summary>没有 Node 时（Debian / Ubuntu，用 NodeSource）</summary>

```bash
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs
node -v
```
</details>

> ⚠️ Node 18 是**硬性要求**：网关用到内置的 `--env-file-if-exists` 与
> `node --test`，更低版本会直接启动失败。

### 3. 高德 Key

到 <https://console.amap.com/dev/key/app> 创建应用，添加 Key。

**平台必须选「Web 服务」** —— 选成「Web 端 (JS API)」或「iOS」都会导致
接口返回 `10009`（Key 与平台不符）。

免费额度与限流见控制台 <https://console.amap.com/dev/flow/manage>。

### 4. 依赖

`package.json` 声明了两个运行时依赖（解析 OSM 矢量瓦片用）：

```
@mapbox/vector-tile
pbf
```

所以**必须执行 `npm install`** —— 不装会启动失败。

---

## 三、拿到代码

```bash
sudo mkdir -p /opt && cd /opt
sudo git clone <本仓库地址> esp32-car-dashboard
cd esp32-car-dashboard/gateway
npm install
```

装完先跑一遍**离线测试**，确认代码本身没问题
（全离线：不联网、不需要真实 Key）：

```bash
npm test
# 期望：pass 98 / fail 0
```

---

## 四、配置

```bash
cp .env.example .env
chmod 600 .env          # 只有属主可读
${EDITOR:-vi} .env
```

### 必填

| 变量 | 说明 |
|------|------|
| `AMAP_KEY` | 高德「Web 服务」Key（§二.3） |
| `GATEWAY_TOKEN` | 自定义访问令牌。`node -e "console.log(crypto.randomUUID())"` |

### 可选（通常不用改）

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `HOST` | `127.0.0.1` | 监听地址。**默认只回环是刻意的安全选择**，见 §六 |
| `PORT` | `8080` | 监听端口 |
| `TRUST_PROXY` | `0` | 信任几跳反向代理。同机反代可设 `1`（用于按真实客户端 IP 限流） |
| `RATE_LIMIT_PER_MIN` | `60` | 每客户端每分钟请求上限 |
| `UPSTREAM_TIMEOUT_MS` | `8000` | 请求高德的超时 |
| `MAP_CACHE_DIR` | `.cache/map-tiles` | OSM 瓦片磁盘缓存目录（需可写；已 gitignore） |
| `VERBOSE_ERRORS` | 未设 | 设为 `1` 时错误响应带更多上游细节。**排查期临时开，之后关掉** |

> `.env` 由 `npm start` 自动加载（脚本里带了 `--env-file-if-exists=.env`），
> 不需要 export，systemd 里也不必重复注入。

---

## 五、启动

### 推荐：systemd 常驻

```ini
# /etc/systemd/system/car-gateway.service
[Unit]
Description=Car Dashboard Navigation Gateway
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/esp32-car-dashboard/gateway
# .env 由 npm start 自动加载，无需在此重复注入
ExecStart=/usr/bin/npm start
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now car-gateway
sudo systemctl status car-gateway        # 期望 active (running)
sudo journalctl -u car-gateway -n 30     # 看启动日志
```

**启动日志应当长这样**（不含任何 Key / Token）：

```
[网关] 已监听 http://127.0.0.1:8080
[网关] 仅本机可访问（HOST=127.0.0.1）；如需外部访问请用反向代理
[网关] 上游 https://restapi.amap.com/v3/direction/driving
[网关] 限流 60/分钟；无缓存（协议 §3.5）
```

若第二行显示 `已监听 http://0.0.0.0:8080`，说明 `HOST` 被设成了 `0.0.0.0`
—— 只有在你**确实要直连**（无反代、无隧道）时才应如此，见 §六。

### 临时前台运行（调试用）

```bash
cd /opt/esp32-car-dashboard/gateway
npm start
```

---

## 六、对外暴露

**网关代码不处理 TLS**，只监听明文 HTTP。HTTPS 必须由外部提供。

### 关键规则：`HOST` 怎么设

| 你的部署方式 | `HOST` | 原因 |
|-------------|--------|------|
| 反向代理（nginx / Caddy） | `127.0.0.1`（默认） | 公网流量进不来，无法绕过代理直连 |
| 隧道（cloudflared 等） | `127.0.0.1`（默认） | 隧道进程在本机连 8080 |
| 网关自己直接对外 | `0.0.0.0` | ⚠️ 此时**没有 TLS**，只适合内网 / 临时验证 |

### 方式 A：反向代理（推荐）

网关与反代同机，网关用默认 `HOST=127.0.0.1`，反代负责证书与转发。

**nginx** —— ⚠️ **必须转发 `Authorization` 头**，否则网关一律返回 401：

```nginx
location / {
    proxy_pass http://127.0.0.1:8080;
    proxy_set_header Host              $host;
    proxy_set_header X-Real-IP         $remote_addr;
    proxy_set_header X-Forwarded-For   $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header Authorization     $http_authorization;   # ← 关键，别漏
}
```

用 `TRUST_PROXY=1` 让限流按真实客户端 IP 计数。

**Caddy**（自动签发证书，更简单）：

```
your-domain.example {
    reverse_proxy 127.0.0.1:8080
}
```

### 方式 B：隧道

```bash
# 网关用默认 HOST=127.0.0.1 监听本地 8080
cd /opt/esp32-car-dashboard/gateway && npm start

# 另起隧道，把本地 8080 暴露为 HTTPS
cloudflared tunnel --url http://127.0.0.1:8080
```

> #### 🔴 `trycloudflare.com`（Quick Tunnel）是**临时**的，务必知悉
>
> Quick Tunnel 是 Cloudflare 的**临时演示**功能，**每次重启 `cloudflared`
> 都会生成一个全新的随机域名**，旧的立即失效。后果：
>
> - **App 里填的地址会突然连不上**，而新域名是一串随机词，
>   排查时很难想到"是隧道域名换了"；
> - 官方明确说明**不用于生产**，无 SLA。
>
> 要长期可用，请改用**具名隧道（Named Tunnel）**绑定自有域名，或改用方式 A。

### 防火墙

- 用反代 / 隧道时：安全组只放行 `443`（`80` 供证书续期），
  **不要**放行 `8080`。
- 确认外网**无法**直连：
  ```bash
  curl -m 10 http://<公网IP>:8080/health     # 期望：超时 / 拒绝，而不是 200
  ```

---

## 七、部署后验收（逐条执行）

```bash
BASE="https://<你的域名>"
TOKEN="<GATEWAY_TOKEN>"
```

### 1. 健康检查（**不需要**认证）

```bash
curl -s "$BASE/health"
# 期望：{"ok":true,"hasKey":true,"amapCallsToday":{...},"mapWindowRequestsToday":0}
```

`hasKey:false` 表示 `AMAP_KEY` 没读到 —— 检查 `.env` 的位置与权限。

### 2. 认证确实生效

```bash
# 不带令牌 → 期望 401
curl -s -o /dev/null -w '%{http_code}\n' -X POST "$BASE/route" \
  -H 'content-type: application/json' \
  -d '{"origin":{"lon":116.397,"lat":39.909},"destination":{"lon":116.48,"lat":40.00}}'

# 带令牌 → 期望 200
curl -s -o /dev/null -w '%{http_code}\n' -X POST "$BASE/route" \
  -H "authorization: Bearer $TOKEN" -H 'content-type: application/json' \
  -d '{"origin":{"lon":116.397,"lat":39.909},"destination":{"lon":116.48,"lat":40.00}}'
```

### 3. 全部接口逐个体检

```bash
auth=(-H "authorization: Bearer $TOKEN" -H 'content-type: application/json')

# 路线（消耗高德额度）
curl -s "${auth[@]}" -X POST "$BASE/route" \
  -d '{"origin":{"lon":116.397,"lat":39.909},"destination":{"lon":116.48,"lat":40.00}}' | head -c 200; echo

# 多方案路线（网关内部串行请求 3 个策略，约 1~3 秒）
curl -s "${auth[@]}" -X POST "$BASE/routes" \
  -d '{"origin":{"lon":116.397,"lat":39.909},"destination":{"lon":116.48,"lat":40.00}}' | head -c 300; echo

# 地理编码
curl -s "${auth[@]}" -X POST "$BASE/geocode" -d '{"address":"北京南站"}'; echo

# 逆地理：应能解析出中文道路名
curl -s "${auth[@]}" -X POST "$BASE/regeo" -d '{"lon":116.397,"lat":39.909}'; echo

# 路况：status 应为中文（如"缓行"），roads 应有具体路段
curl -s "${auth[@]}" -X POST "$BASE/traffic" -d '{"lon":116.397,"lat":39.909}' | head -c 300; echo

# 周边搜索：⚠️ 入参是**顶层** lon/lat，不是嵌套 location
curl -s "${auth[@]}" -X POST "$BASE/around" \
  -d '{"lon":116.397,"lat":39.909,"keywords":"加油站","radius":3000}' | head -c 300; echo

# 输入提示
curl -s "${auth[@]}" -X POST "$BASE/tips" -d '{"keywords":"北京南"}'; echo

# 地图窗口：走 OSM，不消耗高德额度；首次会下载瓦片，稍慢
curl -s "${auth[@]}" -X POST "$BASE/mapwindow" \
  -d '{"lat":39.909,"lon":116.397,"radiusM":300}' | head -c 200; echo

# 骑行
curl -s "${auth[@]}" -X POST "$BASE/bicycling" \
  -d '{"origin":{"lon":116.397,"lat":39.909},"destination":{"lon":116.42,"lat":39.93}}' | head -c 200; echo
```

### 4. 内容抽查（确认不只是 200，内容也对）

- `/regeo` 的 `road` 或 `formattedAddress` 应是**中文地名**，不是空
- `/traffic` 的 `status` 应是**中文**（`畅通` / `缓行` / `拥堵`），不是数字 `"3"`
- `/route` 的 `distanceM` 应是个合理的正整数

> 若 `/traffic` 的 `status` 返回 `"3"` 这种数字，说明跑的是**未修正的中间版本**，
> 确认 `src/amap.js` 里含 `STATUS_TEXT` 映射表。

### 5. 用量计数在工作

```bash
curl -s "$BASE/health" | grep -o '"amapCallsToday":{[^}]*}'
```

刚跑完上面几条后，`route` / `geocode` 等计数应当增加。
（`/health` 与 `/mapwindow` **不消耗**高德额度。）

### 6. 重启后仍可用

```bash
sudo systemctl restart car-gateway
sleep 3
curl -s "$BASE/health"
```

---

## 八、升级流程

```bash
cd /opt/esp32-car-dashboard
sudo git pull
cd gateway
npm install                      # ← 每次升级都要做（可能新增依赖）
npm test                         # 期望 pass 98 / fail 0
sudo systemctl restart car-gateway
sudo systemctl status car-gateway
curl -s "https://<你的域名>/health"
```

`.env` **不需要**动（`git pull` 不会覆盖它，因为它被 gitignore）。

> 📌 **部署后请核对一次版本**：`/health` 的 `amapCallsToday` 字段是
> 较新版本才有的。若返回里没有它，说明跑的还是旧代码。

---

## 九、接口清单（权威）

以 `src/server.js` 里的 `KNOWN_PATHS` 为准，**不要从别处抄**。

除 `GET /health` 外**全部需要**认证头：

```
authorization: Bearer <GATEWAY_TOKEN>
```

| 方法 | 路径 | 说明 | 消耗高德额度 |
|------|------|------|-------------|
| `GET` | `/health` | 健康检查 + 今日用量（**不回显 Key**） | ❌ |
| `POST` | `/route` | 驾车路线规划（单条） | ✅ |
| `POST` | `/routes` | **多方案路线**（网关内部用 3 个策略各取一条并去重） | ✅ ×3 |
| `POST` | `/geocode` | 地址文本 → 坐标 | ✅ |
| `POST` | `/regeo` | 坐标 → 地址 / **当前道路名** | ✅ |
| `POST` | `/traffic` | 周边**路况**（⚠️ **不是红绿灯**） | ✅ |
| `POST` | `/around` | 周边搜索（加油站 / 停车场等） | ✅ |
| `POST` | `/tips` | 地址联想（搜索框输入提示） | ✅ |
| `POST` | `/bicycling` | 骑行路线规划 | ✅ |
| `POST` | `/mapwindow` | 周边地图窗口（路网几何） | ❌ 走 OSM |

> **入参一律 WGS84** —— 网关内部负责转 GCJ-02，调用方不需要知道 GCJ-02 的存在。
>
> ⚠️ **返回坐标的坐标系数不统一**（这是历史原因，调用方必须知道）：
>
> | 接口 | 返回的坐标 |
> |------|-----------|
> | `/route`、`/routes`、`/bicycling` | **GCJ-02**（`origin`/`destination`/`polyline` 都是）|
> | `/regeo`、`/traffic`、`/around`、`/tips`、`/mapwindow` | **WGS84** |
>
> `/mapwindow` 另外会在顶层给出 `crs: "GCJ-02"` 与 `centerGcj` 供交叉核对。

### 认证与错误码

| HTTP | `error` | 含义 |
|------|---------|------|
| 401 | `unauthorized` | 缺 `authorization` 头，或令牌不对 |
| 400 | `invalid_coordinates` | 缺坐标或格式不对（响应里的 `expect` 会说明期望字段） |
| 429 | `rate_limited` | 超过 `RATE_LIMIT_PER_MIN` |
| 502 | `amap_error` | 高德返回业务错误（响应带 `info` / `infocode` 便于排查） |
| 502 | `upstream_failed` | 请求高德超时 / 网络失败（**超时也走 502**，不单列 504）。可调 `UPSTREAM_TIMEOUT_MS`；`VERBOSE_ERRORS=1` 时带细节 |

### `/health` 的返回

```json
{
  "ok": true,
  "hasKey": true,
  "amapCallsToday": {
    "date": "2026-10-15",
    "route": 3, "geocode": 0, "regeo": 0, "traffic": 0,
    "around": 0, "tips": 0, "bicycling": 0, "total": 3
  },
  "mapWindowRequestsToday": 0
}
```

**不会**回显 `AMAP_KEY` 或 `GATEWAY_TOKEN`。

---

## 十、排障

| 现象 | 原因与处理 |
|------|-----------|
| 启动即报 `--env-file-if-exists` 不识别 | Node < 18，升级 Node |
| 启动报 `Cannot find package '@mapbox/vector-tile'` | 忘了 `npm install` |
| 启动即退出，报缺 `AMAP_KEY` / `GATEWAY_TOKEN` | `.env` 未配置或未被加载；检查 `gateway/.env` 与 systemd 的 `WorkingDirectory` |
| `/health` 里 `hasKey:false` | `.env` 不在 `gateway/` 下，或 `AMAP_KEY` 为空 |
| `/health` 没有 `amapCallsToday` | 跑的是旧版本代码 |
| 业务接口返回 **404** | 跑的是旧版本（该端点还不存在） |
| 所有业务接口 **401** | ① 请求没带 `authorization` 头；② **反向代理没转发该头**（§六 nginx 那行）；③ 令牌不匹配 |
| 高德返回 `10009` / `10001` | Key 的**服务平台选错了**，必须选「Web 服务」；或 Key 被禁用 |
| 高德返回 `10003` | 当日免费额度用尽。看 `/health` 的 `amapCallsToday` 定位是哪个接口 |
| 高德返回 `10021 CUQPS_HAS_EXCEEDED_THE_LIMIT` | 触发高德**单账号 QPS 限流**。`/routes` 已内置串行 + 350ms 间隔，但短时间反复重试仍会触发 —— 等几十秒再试 |
| 高德返回 `20800` / `20803` | 起终点不可达 / 距离过长，属**正常业务错误**，不是部署问题 |
| `/mapwindow` 很慢或失败 | 它要去 OpenFreeMap 下瓦片。首次慢是正常的；连续失败检查服务器能否出网 443 |
| 隧道域名突然失效 | Quick Tunnel 重启会换域名（§六）。取新域名并更新 App；长期用请改具名隧道 |
| 返回中文乱码 | 客户端没按 UTF-8 解析（网关返回的就是 UTF-8 JSON） |

临时打开详细错误：

```bash
# .env 里加
VERBOSE_ERRORS=1
sudo systemctl restart car-gateway
# 排查完记得去掉
```

---

## 十一、安全与合规

### 令牌的固有局限（务必知悉）

`GATEWAY_TOKEN` 是**共享密钥 + 明文等值比较 + 永不过期**，定位是
**个人自用、单一客户端**：

- 它无法区分"哪一个客户端"，只能区分"知道令牌 / 不知道令牌"；
- 令牌一旦泄漏（截图、日志、聊天记录），任何知道它的人都能**消耗你的高德配额**；
- 令牌本身写进 App 后，严格说也处于"可反编译提取"的境地 ——
  它提高的是**盗用门槛、挡住随手扫描**，不是强认证；
- 隧道 / 域名本身**不是秘密**（会出现在服务商日志里），真正起防护作用的是令牌。

若日后要面对公网多用户 / 分发场景，建议补强其一：
**令牌轮换或加过期**、**来源 IP 白名单**、或改用短期令牌 / mTLS。

### 不要缓存高德数据

高德协议禁止存储 / 缓存其服务数据。**本网关没有缓存层**，请勿添加。
唯一的例外是 `/mapwindow` 的 OSM 瓦片缓存（`.cache/map-tiles`）——
那是 OSM 系数据、允许离线，与高德无关；它已 gitignore，属运行时缓存。

### 必须遵守的高德协议约束

《高德地图开放平台服务协议》**第 3.4 条**：未获书面许可，
**不得将本服务应用于车载场景**。本项目的定位是**个人开发 / 学习 / 技术验证**，
**不装车、不商用、不对外分发**。

**部署公网网关不等于获得车载使用许可。** 详见
[`../docs/nav-backend.md`](../docs/nav-backend.md) §零。

### 部署侧硬要求

- `chmod 600 .env`，且**确认 `.env` 不在 Git 里**（已在 `.gitignore`）
- 网关的日志与错误响应已经过 `redact()` 脱敏；`/health` 只回 `hasKey` 布尔值
- 对外**只开 HTTPS**，明文 `8080` 不要暴露（§六）
- `VERBOSE_ERRORS` 只在排查期开

---

## 附：代码位置与相关文档

| 内容 | 位置 |
|------|------|
| 入口与路由 | `src/server.js`（端点白名单见 `KNOWN_PATHS`） |
| 环境变量读取与校验 | `src/config.js`（缺 Key / token 即启动失败；`redact` 脱敏） |
| 高德调用 | `src/amap.js`（`AMAP_KEY` 只在这里拼进上游 URL） |
| 响应归一化 | `src/normalize.js`（**各类的字段名以此为准**） |
| 地图窗口（OSM） | `src/mapwindow.js` |
| 离线测试 | `npm test`（`test/*.test.js`） |
| 探测脚本 | `npm run probe` / `npm run probe:new` |
| 环境变量模板 | `.env.example` |
| 接口字段详解 | [`README.md`](README.md) |
| 设计原因与合规 | [`../docs/nav-backend.md`](../docs/nav-backend.md) |
