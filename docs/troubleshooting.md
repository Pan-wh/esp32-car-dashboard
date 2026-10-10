# 常见问题和排查记录

> 状态：骨架。每次实际排障后在此追加记录（日期 + 现象 + 原因 + 解决）。

## 官方排障入口

`reference/ESP32-S3-Touch-AMOLED-1.75/docs/troubleshooting_ZH.md`（含烧录与运行故障）

## 环境类（本机已遇到）

### 2026-10-10 写 ~/Library/Arduino15 被沙盒拒绝

- 现象：直接 bash 写 `~/Library/Arduino15/arduino-cli.yaml` 报 `Operation not permitted`。
- 原因：AI 会话文件沙盒仅允许写项目工作区。
- 解决：经授权的操作完成写入与核心安装；后续手动操作不受影响。

## 记录模板

```
### 日期 - 简述
- 现象：
- 复现步骤：
- 原因：
- 解决：
- 验证结果：
```
