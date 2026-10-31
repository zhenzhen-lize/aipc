# aipc

`aipc` 是一个面向 Windows 的命令行视频下载工具。它用 C 提供统一命令行入口，借助 yt-dlp 下载其支持的平台，并为抖音提供 Python 专用下载通道。

本仓库只包含源码和构建所需文件，不包含 Cookie、账号数据、已下载视频或预编译程序。

## 功能

- 支持一次传入多个链接，或用 `--file` 从文本文件读取链接。
- 支持抖音短链接解析、清晰度选择、重试和下载目录配置。
- 抖音可使用专用下载通道，并可通过 `--no-watermark` 请求无水印地址。
- 使用用户显式提供的 Netscape 格式 Cookie；程序不会从浏览器或桌面应用提取登录凭证。
- 默认下载到 `%USERPROFILE%\Videos\aipc`。

平台兼容性和可下载性取决于 yt-dlp、目标平台和当前网络状态。

## 环境要求

- Windows 10 或更新版本
- MinGW-w64 GCC（支持 Windows API）
- Python 3.9 或更新版本
- 有效的 Netscape 格式 Cookie 文件（下载需要登录的平台内容时）

## 构建与安装

在 PowerShell 中进入本目录：

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
.\build.bat
```

也可以直接运行 `build.bat` 构建 `aipc.exe`。脚本调用：

```text
gcc aipc.c -o aipc.exe -luser32 -lcrypt32 -lbcrypt
```

如果 PowerShell 阻止激活虚拟环境，可跳过激活，直接运行 `.venv\Scripts\python.exe -m pip install -r requirements.txt`。

## 使用

```powershell
# 查看帮助和版本
.\aipc.exe --help
.\aipc.exe --version

# 下载抖音视频
.\aipc.exe "https://v.douyin.com/example/" --cookie "$HOME\Downloads\cookies_netscape.txt"

# 指定下载目录和清晰度
.\aipc.exe "https://www.bilibili.com/video/BV..." --quality 720 --dir "$HOME\Videos\收藏"

# 批量下载，每行一个链接；空行和 # 注释行会被跳过
.\aipc.exe --file .\links.txt --retry 3 --retry-delay 2
```

支持的参数：

| 参数 | 说明 |
| --- | --- |
| `--quality N` | 最高画质限制：360、480、720、1080 或 2160 |
| `-q`, `--quiet` | 减少输出 |
| `--no-watermark` | 抖音专用通道请求无水印地址 |
| `--no-playlist`, `-np` | 不下载播放列表/合集 |
| `--dir PATH` | 指定输出目录 |
| `--cookie PATH` | 指定 Netscape Cookie 文件 |
| `-f`, `--file PATH` | 从文件读取链接 |
| `--retry N` | 失败重试次数，默认 2 |
| `--retry-delay S` | 重试等待秒数，默认 2 |
| `--dry-run` | 检查输入与 Cookie 后跳过下载 |
| `-h`, `--help` | 显示帮助 |
| `--version` | 显示版本 |

环境变量：`AIPC_DOWNLOAD_DIR` 设置默认输出目录，`AIPC_COOKIES` 设置默认 Cookie 路径，`AIPC_QUIET=1` 开启安静模式。命令行参数优先级更高。

## Cookie 与隐私

Cookie 等同于账号登录凭证。请自行从可信来源准备 Netscape 格式 Cookie，使用 `--cookie` 或 `AIPC_COOKIES` 指定；不要把 Cookie 放入仓库，也不要在 issue、日志或截图中公开它。Cookie 文件会被复制到输出目录供下载组件使用。

公开版不会扫描浏览器配置文件或读取桌面应用数据来提取登录凭证。Playwright 回退使用临时浏览器上下文，只注入用户显式提供的 Cookie。Cookie 无效时，请自行更新文件。

## 故障排查

- `gcc` 找不到：安装 MinGW-w64，并确认 `gcc` 在 `PATH` 中。
- 找不到 yt-dlp：依赖安装后确认 `python -m yt_dlp --version` 可运行。
- 抖音备用通道提示缺少 Playwright 浏览器：运行 `python -m playwright install chromium`。
- 下载失败或 Cookie 无效：确认 Cookie 尚未过期，并检查目标内容是否可由当前账号访问。

## 许可

本项目采用 MIT License，详见 [LICENSE](LICENSE)。

请遵守目标平台服务条款和适用法律，仅下载你有权保存的内容。
