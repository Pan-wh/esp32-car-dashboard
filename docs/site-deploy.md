# 部署落地页到 Cloudflare Pages（路线 A：连 Git 自动部署）

> 目标：`https://<你的域名>` 打开暗黑风格落地页，**以后每次 push 自动更新**。
>
> 站点是纯静态（`site/index.html` + `style.css` + `img/`），**没有构建步骤** ——
> 所以 CF 的「构建命令」必须**留空**，这是最容易填错的地方。

---

## 前置条件

- [x] 仓库已推送到 `Pan-wh/esp32-car-dashboard`
- [x] `site/` 目录已提交（含 12 张图）
- [ ] Cloudflare 账号（你已有，域名也在 CF）
- [ ] GitHub 已授权给 Cloudflare（首次会引导）

---

## 一、创建 Pages 项目

1. 打开 <https://dash.cloudflare.com/>
2. 左侧 **Workers & Pages**
3. 右上 **Create** → 选 **Pages** 标签
4. 点 **Connect to Git**
5. 首次会跳 GitHub 授权 → 点 **Authorize Cloudflare Pages**
   - 授权范围选 **Only select repositories** → 勾 `esp32-car-dashboard`
6. 回到 CF，选中 **`Pan-wh/esp32-car-dashboard`** → **Begin setup**

---

## 二、构建设置（⚠️ 这里最容易填错）

| 字段 | 填什么 | 说明 |
|------|--------|------|
| **Project name** | `cardash`（或任意）| 会变成 `cardash.pages.dev` |
| **Production branch** | `main` | |
| **Framework preset** | **None** | ❗别选框架 |
| **Build command** | **（完全留空）** | ❗**关键**：没有构建步骤，填了会失败 |
| **Build output directory** | **`site`** | ❗**关键**：不是 `/`，不是 `public` |

> **为什么是这个组合**：仓库根目录是固件/App/网关，静态站只在 `site/` 子目录，
> 而且它不需要编译。CF 只要把这个目录原样发布即可。

7. 点 **Save and Deploy**
8. 等 30 秒左右，看到 **Success** → 访问 `https://cardash.pages.dev`

**验收**：页面能打开、6 个表盘图都在、GitHub 按钮能跳。

---

## 三、绑定自己的域名

1. Pages 项目页 → **Custom domains** 标签
2. **Set up a custom domain**
3. 输入你的域名（例如 `dash.example.com` 或 `example.com`）
4. CF 会自动加 DNS 记录（域名已在 CF 托管时）
5. 等状态变成 **Active**（通常 1–2 分钟）

> 如果域名**不在** CF 托管，需要去域名商加一条 `CNAME` 指向 `cardash.pages.dev`。

---

## 四、以后怎么更新

```bash
# 改 site/ 里的文件
git add site/ && git commit -m "update site" && git push
```

**推送后 CF 自动重新部署**（约 30 秒）。

---

## 五、常见问题

| 现象 | 原因 | 解决 |
|------|------|------|
| 部署成功但页面 **404** | Build output directory 填错了 | 改成 **`site`** 重新部署 |
| 构建 **失败**，日志说找不到 `package.json` | Build command 没留空 | 清空 Build command |
| 页面打开但**图片全裂** | `site/img/` 没提交 | `git ls-files site/img/` 确认；没有就 `git add site/img` |
| 域名一直 **Pending** | DNS 没生效 | 等几分钟；或检查 CNAME |
| 改了样式但页面没变 | 浏览器缓存 | 强制刷新（Cmd+Shift+R） |

---

## 六、上线后建议做的两件事

### ① 开启 Web Analytics（免费、无 Cookie）

Pages 项目 → **Analytics** → **Enable**

### ② 把 `og:image` 换成**绝对地址**

`site/index.html` 里现在是相对路径：

```html
<meta property="og:image" content="img/all-faces-v2.png">
```

微信 / Twitter 分享卡片要求**绝对 URL**，域名定了之后改成：

```html
<meta property="og:image" content="https://你的域名/img/all-faces-v2.png">
```

---

## 附：项目结构对照

```
esp32-car-dashboard/
├── firmware/        固件（不参与网页部署）
├── simulator/       模拟器（不参与）
├── ios/             App（不参与）
├── gateway/         网关（不参与）
├── docs/            文档（不参与）
└── site/            ← 只有这个目录会被发布
    ├── index.html
    ├── style.css
    ├── _headers
    ├── robots.txt
    └── img/         12 张图
```
