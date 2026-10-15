# 导航网关部署文档

> **读者**：负责部署的人（龙虾）。
> **目标**：把 `gateway/` 部署成可通过 **HTTPS** 访问的线上网关，供 iPhone App 调用。
> 本文只陈述事实与操作步骤，不含任何"官方推荐"表述。

---

## 〇、本次部署要做的事（TL;DR）

线上当前跑的是**旧版本**（只有 `/route` + `/geocode`）。本次要更新到新版本，新增 5 个接口：

| 现状 | 目标 |
|------|------|
| 旧版：`/health` `/route` `/geocode` | 新版：再加 `/regeo` `/traffic` `/around` `/tips` `/bicycling` `/mapwindow` |
| 零依赖（无需 npm install） | **有依赖**（必须 `npm install`） |
| 暴露面：公网 IP + 明文 HTTP | **只保留 HTTPS 隧道**，关掉 IP:8080 直连 |

**本机自检命令**（部署前可先在本地跑，确认代码本身没问题）：

```bash
cd gateway
npm install          # 新版本需要，见 §三
npm test             # 期望：88 项全过，全离线，不联网、不需要真实 Key
```

---

## 一、这个网关是什么

为 iPhone 侧代理**高德 Web 服务**：高德 Key 只在服务端，App 不接触 Key。

```
App ──(带 GATEWAY_TOKEN)──▶ 本网关 ──(带 AMAP_KEY)──▶ 高德 Web 服务
   └─(HTTPS 隧道)─┘
```

`GATEWAY_TOKEN` 是**本项目自建的访问令牌**，与高德无关；作用是挡住"能连到网关的人"
盗用服务端的 `AMAP_KEY` 刷配额。详见 §四。

---

## 二、接口清单

除 `GET /health` 外**全部需要**认证头：

```
authorization: Bearer <GATEWAY_TOKEN>
```

| 方法 | 路径 | 说明 | 消耗高德额度 |
|------|------|------|-------------|
| `GET` | `/health` | 健康检查 + 今日用量统计（**不回显 Key**）| 否 |
| `POST` | `/route` | 驾车路线规划 | ✅ |
| `POST` | `/geocode` | 地址文本 → 坐标 | ✅ |
| `POST` | `/regeo` | 坐标 → 地址 / **当前道路名** | ✅ |
| `POST` | `/traffic` | 周边**拥堵**情况（⚠️ **不是红绿灯**）| ✅ |
| `POST` | `/around` | 周边搜索（加油站 / 服务区等）| ✅ |
| `POST` | `/tips` | 地址联想（搜索框输入提示）| ✅ |
| `POST` | `/bicycling` | 骑行路线规划 | ✅ |
| `POST` | `/mapwindow` | 周边地图窗口（±500m 路网几何）| ❌ 走 OSM |

> **所有接口的入参与返回坐标一律 WGS84**（网关内部负责往返 GCJ-02 转换）。
> 各接口的请求 / 响应字段示例见 [`README.md`](README.md)。

### 认证与错误码

| 状态 | `error` | 含义 |
|------|---------|------|
| 400 | `invalid_coordinates` / `invalid_keywords` | 参数非法（会附 `expect` 说明格式）|
| 401 | `unauthorized` | token 缺失或错误 |
| 404 | `not_found` | 路径不在白名单 |
| 413 | `BODY_TOO_LARGE` | 请求体超过 8KB |
| 429 | `rate_limited` | 触发限流 |
| 502 | `amap_error` | 高德业务错误（附 `info` / `infocode` / `hint`）|
| 502 | `upstream_failed` | 网络 / 超时 |

---

## 三、依赖与运行环境（⚠️ 本次有变化）

- **Node.js ≥ 18**（开发环境实测 v24）。
- **必须执行 `npm install`**：
  `/mapwindow` 依赖 `@mapbox/vector-tile` 与 `pbf`。
  早期版本是"零依赖"，**本次更新后不再是** —— 跳过这步会导致启动即报错：
  `Cannot find package '@mapbox/vector-tile'`。

```bash
cd /opt/esp32-car-dashboard/gateway   # 或实际部署路径
npm install
```

---

## 四、环境变量

写入 `gateway/.env`（**该文件已被 `.gitignore` 忽略，绝不要提交**）。

| 变量 | 必填 | 默认 | 说明 |
|------|------|------|------|
| `AMAP_KEY` | ✅ | — | 高德「Web 服务」类型 Key；**缺失则启动即失败** |
| `GATEWAY_TOKEN` | ✅ | — | 客户端访问令牌；**缺失则启动即失败** |
| `HOST` | | `127.0.0.1` | 监听地址。**默认只回环**（见下）|
| `PORT` | | `8080` | 监听端口 |
| `TRUST_PROXY` | | `0` | 信任的反代跳数；同机反代可设 `1` |
| `RATE_LIMIT_PER_MIN` | | `60` | 每分钟请求上限 |
| `UPSTREAM_TIMEOUT_MS` | | `8000` | 单次上游超时（毫秒）|
| `MAP_CACHE_DIR` | | `.cache/map-tiles` | 地图瓦片缓存目录（需可写）|
| `VERBOSE_ERRORS` | | — | 设为 `1` 才回传内部错误细节（仅调试用）|

### ⚠️ 关于 `HOST`（本次新增，重要）

新版本的默认值是 **`127.0.0.1`（只监听回环）**，这是刻意的安全默认：

- **本项目采用隧道 / 反向代理终止 TLS**（见 §五）→
  隧道客户端与网关同机，**用默认值 `127.0.0.1` 即可**，公网流量进不来，
  网关不可能被绕过隧道直连。
- 只有当网关**自己要直接对外**（无隧道、无代理）时，才需要显式设 `HOST=0.0.0.0`。

> 早期版本写死"绑所有网卡"，属默认不安全，已改。
> 启动日志会明确打印当前暴露面：
> `[网关] 仅本机可访问（HOST=127.0.0.1）` 或
> `[网关] ⚠️ 正在监听所有网卡（HOST=0.0.0.0）`。

---

## 五、HTTPS 部署方式

本项目代码**不处理 TLS**（只监听明文 HTTP）。HTTPS 必须由**外部**提供，两种方式：

### 方式 A：隧道（**本项目当前使用的方式**）

当前线上用的是 **Cloudflare Tunnel（Quick Tunnel）**：

```
https://eos-deviation-soldiers-diagram.trycloudflare.com
```

启动方式（示意，确切命令以实际部署脚本为准）：

```bash
# 网关用默认 HOST=127.0.0.1 监听本地 8080
cd /opt/esp32-car-dashboard/gateway && npm start

# 另起隧道，把本地 8080 暴露为 HTTPS
cloudflared tunnel --url http://127.0.0.1:8080
```

> #### 🔴 `trycloudflare.com` 是**临时隧道**，务必知悉
>
> Quick Tunnel 是 Cloudflare 的**临时演示**功能，**每次重启 `cloudflared`
> 都会生成一个全新的随机域名**，旧的立即失效。这意味着：
>
> - **App 里填的地址会突然连不上**，且新域名是一串随机词，
>   排查时很难意识到"是隧道域名换了"。
> - 官方明确说明**不用于生产**，无 SLA。
>
> 若要保持长期可用，应改用**具名隧道（Named Tunnel）**绑定自有域名，
> 或改用**方式 B**。此事需与项目负责人确认后再动。

### 方式 B：反向代理（nginx / Caddy 终止 TLS）

网关与反代同机，网关用默认 `HOST=127.0.0.1` 监听 8080，反代负责证书与转发。

**必须转发认证头**，否则网关一律 401：

```nginx
# nginx 示例
location / {
    proxy_pass http://127.0.0.1:8080;
    proxy_set_header Host              $host;
    proxy_set_header X-Real-IP         $remote_addr;
    proxy_set_header X-Forwarded-For   $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header Authorization     $http_authorization;   # ← 关键，别漏
}
```

用 Caddy 则更简单（自动签发证书）：

```
your-domain.example {
    reverse_proxy 127.0.0.1:8080
}
```

---

## 六、防火墙与端口（本次要做的收尾）

**要求：只保留 HTTPS 隧道，关掉 IP:8080 直连。**

原因：明文 HTTP 直连既无加密、又绕过隧道的额外一层隐蔽性；
且 App 走 HTTPS 后，明文入口再无用途，留着只是多余的攻击面。

1. **关闭云主机安全组的入站 `8080` 放行**（以及此前放行的 `80` / `443` 若未使用）。
2. 确认外网**无法**直连：
   ```bash
   curl -m 10 http://<公网IP>:8080/health     # 期望：连接超时 / 拒绝，而不是 200
   ```
3. 确认 HTTPS 隧道**可用**：
   ```bash
   curl -s https://<隧道域名>/health
   # 期望：{"ok":true,"hasKey":true,"amapCallsToday":{...}}
   ```

> 若使用方式 B（反代），安全组只需放行 `443`（和 `80` 用于证书续期），
> **不要**放行 `8080`。

---

## 七、systemd 服务

服务名沿用 `car-gateway`。若此前已配置，**主要改动是加上 `npm install` 的前置步骤**
（新版本有依赖），并确认工作目录正确。

```ini
[Unit]
Description=Car Dashboard Navigation Gateway
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/esp32-car-dashboard/gateway
# .env 由 npm start 自动加载（--env-file-if-exists），无需在此重复注入
ExecStart=/usr/bin/npm start
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

更新步骤：

```bash
cd /opt/esp32-car-dashboard/gateway
git pull                      # 或按实际方式更新代码
npm install                   # ← 本次新增的必需步骤
sudo systemctl restart car-gateway
sudo systemctl status car-gateway     # 确认 active (running)
sudo journalctl -u car-gateway -n 30  # 看启动日志
```

**启动日志里应当能看到**（不含任何 Key / token）：

```
[网关] 已监听 http://127.0.0.1:8080
[网关] 仅本机可访问（HOST=127.0.0.1）；如需外部访问请用反向代理
[网关] 上游 https://restapi.amap.com/v3/direction/driving
[网关] 限流 60/分钟；无缓存（协议 §3.5）
```

---

## 八、部署后验收（**请逐条执行**）

用真实隧道域名与令牌替换占位符：

```bash
BASE="https://<隧道域名>"
TOKEN="<GATEWAY_TOKEN>"
```

### 1. 健康检查（无需认证）

```bash
curl -s $BASE/health
```

期望：`{"ok":true,"hasKey":true,"amapCallsToday":{...}}`

> ⚠️ **`amapCallsToday` 这个字段是"新版本"的标志。**
> 若返回里**没有**它，说明部署的还是旧版本，请回到 §三、§七 重来。

### 2. 认证是否生效

```bash
# 无令牌 → 期望 401
curl -s -o /dev/null -w "%{http_code}\n" -X POST $BASE/route \
  -H 'content-type: application/json' \
  -d '{"origin":{"lon":116.397428,"lat":39.90923},"destination":{"lon":116.481028,"lat":39.989643}}'
```

期望输出：`401`

### 3. 5 个新接口是否都已上线

```bash
AUTH="authorization: Bearer $TOKEN"
CT="content-type: application/json"
P='{"lon":116.397428,"lat":39.90923}'

curl -s -o /dev/null -w "/regeo     %{http_code}\n" -X POST $BASE/regeo     -H "$AUTH" -H "$CT" -d "$P"
curl -s -o /dev/null -w "/traffic   %{http_code}\n" -X POST $BASE/traffic   -H "$AUTH" -H "$CT" -d "$P"
curl -s -o /dev/null -w "/around    %{http_code}\n" -X POST $BASE/around    -H "$AUTH" -H "$CT" -d "$P"
curl -s -o /dev/null -w "/tips      %{http_code}\n" -X POST $BASE/tips      -H "$AUTH" -H "$CT" -d '{"keywords":"天安"}'
curl -s -o /dev/null -w "/bicycling %{http_code}\n" -X POST $BASE/bicycling -H "$AUTH" -H "$CT" \
  -d '{"origin":{"lon":116.397428,"lat":39.90923},"destination":{"lon":116.481028,"lat":39.989643}}'
```

**期望：五行全部 `200`。**
（本次部署前实测：这 5 行全是 `404`，因为线上是旧版本。）

### 4. 真实数据抽查（确认不只是返回 200，内容也对）

```bash
# 逆地理：应能解析出中文道路名
curl -s -X POST $BASE/regeo -H "$AUTH" -H "$CT" -d "$P"

# 拥堵：status 应为中文（如"拥堵"），roads 应有具体路段
curl -s -X POST $BASE/traffic -H "$AUTH" -H "$CT" -d "$P"
```

期望要点：
- `/regeo` → `road` 是中文字符串（如 `"北湾子胡同"`），不是 `null`
- `/traffic` → `status` 是**中文**（如 `"拥堵"`），不是数字 `"3"`

> 若 `/traffic` 的 `status` 返回 `"3"` 这种数字，说明跑的是**未修正的中间版本**，
> 请确认 `src/amap.js` 里含 `STATUS_TEXT` 映射表。

### 5. 明文入口已关闭

```bash
curl -m 10 -o /dev/null -w "%{http_code}\n" http://<公网IP>:8080/health
```

期望：**超时或连接被拒**（不是 `200`）。

---

## 九、安全须知

- `AMAP_KEY` 与 `GATEWAY_TOKEN` **只存在于服务端 `.env`**（建议权限 `600`），
  绝不写入代码、日志、Git 或下发给 App。
- 网关所有日志与错误响应都经 `redact()` 脱敏；`/health` 只回 `hasKey` 布尔值。
- **无缓存层**：遵守高德协议禁止存储 / 缓存服务数据的要求。
  例外是 `/mapwindow` 的 OSM 瓦片缓存（`.cache/map-tiles`），
  那是 OSM 系数据、允许离线，与高德无关。
- `gateway/.cache/` 属**运行时缓存**，已在 `.gitignore` 中忽略，不应纳入版本控制。

### ⚠️ 令牌的固有局限（务必知悉）

当前实现为**共享密钥 + 明文等值比较 + 永不过期**，定位是**个人自用、单一客户端**。

令牌一旦写进分发的 App，严格意义上也处于"可反编译提取"的境地；
它的作用是**提高盗用门槛、挡住随手扫描**，而非强认证。

另需注意：**隧道域名本身不是秘密**（会出现在 Cloudflare 日志等处），
真正起防护作用的是 `GATEWAY_TOKEN`。

若将来要面对公网多用户 / 分发场景，建议补强其一：
**令牌轮换或加过期**、**来源 IP 白名单**、或改用短期令牌 / mTLS。

---

## 十、故障排查

| 现象 | 可能原因 | 处理 |
|------|---------|------|
| 启动即退出，报缺 `AMAP_KEY` / `GATEWAY_TOKEN` | `.env` 未配置或未被加载 | 检查 `gateway/.env` 与 systemd 的 `WorkingDirectory` |
| 启动报 `Cannot find package '@mapbox/vector-tile'` | 漏了 `npm install` | 回到 §三 执行 |
| 5 个新接口返回 **404** | 跑的还是旧版本代码 | 确认代码已更新、服务已 `restart` |
| `/health` 无 `amapCallsToday` 字段 | 同上（旧版本）| 同上 |
| 所有接口 401 | 反代/隧道**未转发 `authorization` 头** | 见 §五 方式 B 的 `proxy_set_header` |
| `/traffic` 的 `status` 是数字 | 跑的是未修正版本 | 确认 `src/amap.js` 含 `STATUS_TEXT` |
| 隧道域名突然失效 | **Quick Tunnel 重启换了域名** | 见 §五 的 🔴 提示；取新域名并更新 App |
| 高德返回 `10003` 配额用尽 | 用量超限 | 看 `/health` 的 `amapCallsToday` 定位是哪个接口 |

---

## 附：相关代码位置

| 文件 | 作用 |
|------|------|
| `gateway/src/config.js` | 环境变量读取与校验（缺 Key/token 即启动失败、`HOST` 默认只回环、日志脱敏 `redact`）|
| `gateway/src/server.js` | HTTP 入口：Bearer 校验、限流、参数校验、错误透传、端点白名单 `KNOWN_PATHS` |
| `gateway/src/amap.js` | 高德客户端（在此处把 `AMAP_KEY` 拼进上游 URL）|
| `gateway/src/mapwindow.js` | 周边地图窗口（OSM 矢量瓦片，不消耗高德额度）|
| `gateway/.env.example` | 环境变量模板 |
| `gateway/README.md` | 接口字段示例 + 真实响应实测记录 |
