# Passport Sync v1.5.0 · Mac 安装说明

Passport Sync 通过蓝牙把 Codex 剩余额度、重置卡和任务状态同步到刷入配套固件的 AI Passport。

首发预览包适用于 **Apple 芯片 Mac**。应用的最低构建目标是 **macOS 13**，首次打开、蓝牙配对和额度同步已在 macOS 15.7.1 实测。请先安装并登录 Codex CLI。下载包已包含编译好的 Passport Sync 应用。

## 下载与首次打开

1. 在项目的 GitHub Releases 下载 `Passport-Sync-v1.5.0-macOS-arm64.zip`。芯片类型可在「 → 关于本机」查看。
2. 对照同页的 `SHA256SUMS` 核对 ZIP。可以在终端运行 `shasum -a 256 ~/Downloads/Passport-Sync-v1.5.0-macOS-arm64.zip`，确认第一列与清单一致。
3. 解压 ZIP，把 **Passport Sync.app** 拖到「应用程序」，再双击打开。
4. 预览包采用临时签名，未经 Apple 公证。如果 macOS 提示无法验证开发者，确认来源及校验值后，到「系统设置 → 隐私与安全性」找到 Passport Sync，点「仍要打开」，再确认打开。如果提示应用已损坏或会损坏电脑，请停止安装并反馈原始提示。 [Apple 首次打开说明](https://support.apple.com/zh-cn/102445)
5. 在「连接 Codex CLI」窗口确认路径。应用找到常用路径时会自动填入。字段为空时，在终端运行 `command -v codex`，把输出的完整路径粘贴进去，再点「继续」。后续可在「应用设置 → Codex CLI」更换路径。

## 蓝牙配对与首次同步

1. 打开 Mac 蓝牙。在 Passport Sync 请求蓝牙权限时点「允许」。如果此前拒绝过，到「系统设置 → 隐私与安全性 → 蓝牙」开启 Passport Sync 的权限。
2. 在卡片上长按确认键打开设置。进入「连接 → 蓝牙同步」，打开「蓝牙开关」，再选「开始配对」。保持卡片停在该页。
3. Mac 请求配对码时，输入卡片屏幕上的六位数字。首次配对窗口约两分钟，超时后重新打开卡片的配对页。
4. 等 Mac 显示「已连接」，再确认出现「同步于…」及剩余额度。也可以点「立即同步」。切到卡片的 Codex 页，核对配对后第一次同步的额度。

已配对设备会自动重连。希望每次登录就开始同步，可以开启「登录 Mac 时自动启动」。关闭应用窗口后，Passport Sync 仍在菜单栏运行。

## 从源码安装

准备 Xcode 命令行工具及已登录的 Codex CLI。从 GitHub 克隆对应发布版本，在项目根目录执行：

```bash
bash mac/install.sh "$(command -v codex)"
```

安装完毕后，从「应用程序」打开 Passport Sync 并按上面的步骤配对。


## 应用语言与设备配置

打开「应用设置…」可管理语言、登录启动、Codex CLI 和 SSH 任务来源。Mac 界面语言独立保存。打开「设备配置…」后输入 Passport 屏幕上的局域网网址及访问码。菜单栏的「备份与恢复…」可直接进入同一配置页的备份窗口。
