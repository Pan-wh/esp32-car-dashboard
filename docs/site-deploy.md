# 上线落地页（零基础版）

> 站点已经在仓库里了（`site/`），这一步只是把它**发布到互联网**。
>
> 下面给你**两条路**，建议先走 **路线 B**（2 条命令，5 分钟），
> 上线后再考虑路线 A（自动部署，不用管了）。

---

# 🅰 路线 B：命令行部署（**最简单，推荐先做这个**）

## 你要打开的窗口

**终端**（Terminal）—— 就是那个黑框框。
Mac 上按 `Command + 空格`，输入 `终端`（或 `Terminal`），回车。

## 第 1 步：登录 Cloudflare

在终端里粘贴这一行，回车：

```bash
cd /Users/pwh/Data/PWH/code/esp32-car-dashboard
npx wrangler login
```

**会发生什么**：
1. 第一次会问 `Need to install the following packages: wrangler@... Ok to proceed? (y)`
   → 输入 **`y`** 回车
2. 浏览器**自动弹出** Cloudflare 登录页
3. 点 **Allow**（允许）
4. 浏览器显示 `Successfully logged in` → 关掉浏览器，回终端

**终端里应该出现**：
```
Successfully logged in.
```

> ❗如果没弹浏览器：终端里会打印一个链接，复制到浏览器打开即可。

## 第 2 步：部署

```bash
npx wrangler pages deploy site --project-name cardash
```

**第一次会问几个问题**，一路回车（用默认值）即可：

```
✔ Enter the production branch name: › main        ← 直接回车
✔ ... 
```

**成功后会打印**：
```
✨ Deployment complete! Take a peek over at
   https://cardash.pages.dev
```

## 第 3 步：打开看看

浏览器访问 **`https://cardash.pages.dev`**

**应该看到**：暗黑背景、网格、"一块圆屏 六个表盘"、6 张表盘图、GitHub 按钮。

---

## ❗如果上一步报了 `Project not found`

说明项目还没创建过。先建一次：

```bash
npx wrangler pages project create cardash --production-branch main
```

然后再跑一次部署命令。

---

# 🅱 路线 A：连 Git 自动部署（上线后再做，以后不用管）

> **好处**：以后你 `git push`，网站**自动更新**，不用再敲命令。

## 第 1 步：进 Cloudflare 后台

1. 浏览器打开 <https://dash.cloudflare.com/>
2. 用**同一个账号**登录（刚才 wrangler 用的那个）
3. 左边菜单找 **Workers & Pages**（图标是个橙色小三角 + 白色方框）
   - 找不到的话：直接在页面顶部搜索框打 `Workers`

## 第 2 步：创建 Pages 项目

在 **Workers & Pages** 页面：

1. 右上角点 **Create**（蓝色按钮）
2. 出现两个选项卡：**Workers** / **Pages** → 点 **Pages**
3. 点 **Connect to Git**（左边第一个，带 GitHub 图标）

## 第 3 步：授权 GitHub

1. 弹出 `Install & Authorize Cloudflare Pages` 页面
2. 如果问 **仓库范围**，选 **Only select repositories**
3. 在下拉里勾 **`esp32-car-dashboard`**
4. 点绿色 **Install & Authorize**

> 这一步只授权**这一个仓库**，CF 看不到你其他项目。

## 第 4 步：选仓库

回到 CF，列表里出现你的仓库：

```
Pan-wh/esp32-car-dashboard      Last updated: just now
```

选中它 → 点右下 **Begin setup**

## 第 5 步：⭐ 填构建设置（**这一步最容易错**）

页面是 **Set up builds and deployments**，有一个表单。**照下面填**：

| 表单字段 | 填什么 | ❗注意 |
|---------|--------|-------|
| **Project name** | `cardash` | 会变成 `cardash.pages.dev` |
| **Production branch** | `main` | 默认就是 main |
| **Framework preset** | 下拉选 **None** | ❗别选 Next.js/Vite 等 |
| **Build command** | **删空！一个字都别留** | ❗❗**最关键** |
| **Build output directory** | 输入 **`site`** | ❗❗**第二关键** |

**为什么这两个最关键**：
- 这个站**不需要编译**（是纯 HTML/CSS），所以 Build command 必须空；
- 网页文件在仓库的 `site/` 子目录，不是根目录。

**填错会怎样**：
- Build command 填了东西 → **部署失败**（红叉，有报错日志）
- Build output directory 填错 → **部署"成功"但打开是 404**（更迷惑）

## 第 6 步：部署

点 **Save and Deploy**，然后等。你会看到一个进度列表：

```
✔ Initializing build environment
✔ Cloning git repository
✔ Building application       ← 这步几秒就好
✔ Deploying to Cloudflare's global network
✔ Success! Your site is live at https://cardash.pages.dev
```

**约 30 秒**。

## 第 7 步：验证

点页面上的 `cardash.pages.dev` 链接，**检查 4 件事**：

- [ ] 页面能打开（深色背景 + 细网格）
- [ ] 标题「一块圆屏 六个表盘」是**橙+青渐变**
- [ ] **6 张表盘图**都显示（不是裂图）
- [ ] 点 **GitHub 仓库**按钮 → 跳到你的仓库

---

# 🌐 绑定自己的域名（A、B 都能用）

## 前提

你的域名**已经在 Cloudflare**（改过 NS 指向 CF）。

## 步骤

1. CF 后台 → **Workers & Pages** → 点进你的 **cardash** 项目
2. 顶部标签选 **Custom domains**
3. 点 **Set up a custom domain**
4. 输入你要用的域名：
   - 想用**子域名**：`dash.你的域名.com`
   - 想用**主域名**：`你的域名.com`
5. 点 **Continue**
6. CF 会显示要添加的 DNS 记录 → **自动加好了** → 点 **Activate domain**
7. 等状态从 `Pending` 变成 **`Active`**（1–2 分钟）

**验证**：浏览器打开你的域名，应该看到同一个页面。

> ⚠️ **主域名如果已经在用**（比如有邮箱、别的主页），
> 建议用**子域名** `dash.你的域名.com`，不影响现有的东西。

---

# 🔧 出问题了看这里

| 你看到的 | 真正的原因 | 怎么修 |
|---------|-----------|--------|
| 部署**成功**但打开 **404** | Build output directory 填错了 | 项目 → **Settings** → **Builds & deployments** → 改成 `site` → **Retry deployment** |
| 部署**失败**，日志有 `npm ERR!` / `package.json` | Build command 没清空 | 同上，把 **Build command 清空** → Retry |
| 页面上**图片全是裂的** | `site/img/` 没推上去 | 在本地跑 `git ls-files site/img/` 确认有 12 个文件 |
| 改了样式但页面没变 | 浏览器缓存 | `Cmd + Shift + R` 强制刷新 |
| 域名一直 **Pending** | DNS 还没生效 | 等 5–10 分钟；或检查 CF 的 DNS 页面有没有那条记录 |
| `npx wrangler login` 卡住 | 浏览器没弹出来 | 看终端打印的链接，手动复制到浏览器 |
| `Project not found` | 项目还没建 | 先跑 `npx wrangler pages project create cardash --production-branch main` |

---

# ✅ 做完之后

告诉我**域名**，我把这两处补上：

1. **`og:image` 改成绝对地址** —— 否则分享到微信/Twitter 没有缩略图
2. **README 顶部加一行网站链接** —— 仓库访客能直接点

---

# 📌 两条路的区别（选一个做就行）

| | 路线 B（命令行）| 路线 A（连 Git）|
|---|---|---|
| 操作量 | **2 条命令** | 约 15 次点击 |
| 首次上线 | **1 分钟** | 3–5 分钟 |
| 以后更新 | 每次手动跑一次命令 | **push 后自动** |
| 适合 | **先跑起来** | 长期维护 |

**建议**：先 B 上线看看效果，再补 A。
（A 建好后 URL 是同一个 `cardash.pages.dev`，不影响）
