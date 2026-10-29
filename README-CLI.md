# PhiRender 命令行使用说明

本文介绍项目提供的三个命令行程序：谱面转换器、自动转换并渲染的视频 CLI，以及只渲染 RPEN 的 CLI。

## 选哪个程序

| 程序 | 用途 | 支持的输入 |
| --- | --- | --- |
| `phigros-rpen` | 只转换谱面格式，不渲染视频 | 官方 JSON、RPE/RPEN JSON、PEZ、ZIP |
| `phigros-render-cli` | 官方谱面自动转换为 RPEN 后渲染；RPE/RPEN 则直接渲染 | JSON、PEZ、ZIP |
| `phigros-render-pure` | 纯渲染，不做格式转换 | RPEN JSON、RPEN PEZ/ZIP |

如果输入是官方谱面并要直接生成视频，使用 `phigros-render-cli`。如果已经有 RPEN 谱面并且希望跳过转换，使用 `phigros-render-pure`。

## Windows 构建

在项目根目录打开 PowerShell。首次构建需要 Rust 工具链和网络连接，以下载 Cargo 依赖：

```powershell
cargo build --release --bin phigros-rpen
cargo build --release --bin phigros-render-cli
cargo build --release --bin phigros-render-pure
```

生成的程序位于 `target\release\`，文件名分别为：

```text
phigros-rpen.exe
phigros-render-cli.exe
phigros-render-pure.exe
```

渲染程序还需要项目中的 `work\Phira-Pro\assets` 资源目录，至少包含 `harmonyos.ttf`、`phigros.ttf` 和 `bold.ttf`。建议从项目根目录运行命令，并保留 `work\Phira-Pro\assets` 的原有结构。若将 EXE 单独复制到其他目录，程序可能找不到这些资源。

渲染需要 FFmpeg。程序会尝试使用项目内已知位置的 FFmpeg，之后也会查找系统 `PATH`。也可以用 `--ffmpeg` 指定可执行文件，或设置 `PHIRA_FFMPEG` 环境变量。

## 生成视频

### 官方 JSON、PEZ 或 ZIP：自动转换后渲染

```powershell
.\target\release\phigros-render-cli.exe `
  ".\charts\Desultory_Signals_IN.pez" `
  ".\output\Desultory_Signals_IN.mp4" `
  --width 1920 --height 1080 --fps 60
```

也可以直接输入官方 JSON：

```powershell
.\target\release\phigros-render-cli.exe `
  ".\charts\official.json" `
  ".\output\chart.mp4" `
  --width 1920 --height 1080 --fps 60
```

对 PEZ/ZIP，程序会转换被识别为谱面的 JSON，并保留其他归档资源。对普通 RPE 或 RPEN 输入，程序会识别格式并直接用于渲染。

如果输入的是没有附带音乐或曲绘的裸 JSON，可额外提供资源和显示信息：

```powershell
.\target\release\phigros-render-cli.exe `
  ".\charts\official.json" ".\output\chart.mp4" `
  --name "Desultory Signals" --level "IN 17.0" `
  --music ".\assets\song.ogg" --picture ".\assets\illustration.png"
```

`--music` 和 `--picture` 只用于裸 JSON，不能与 PEZ/ZIP 输入一起使用。未提供 `--name` 时使用输入文件名；未提供 `--level` 时留空。

### 已有 RPEN：不转换，直接渲染

```powershell
.\target\release\phigros-render-pure.exe `
  ".\charts\chart.rpen.json" ".\output\chart.mp4" `
  --width 1920 --height 1080 --fps 60
```

RPEN PEZ/ZIP 也可以直接输入：

```powershell
.\target\release\phigros-render-pure.exe `
  ".\charts\chart.pez" ".\output\chart.mp4"
```

纯渲染程序遇到官方谱面或普通 RPE 会报错；这类输入请改用 `phigros-render-cli`。

## 渲染参数

渲染程序支持以下主要选项，可通过 `phigros-render-cli.exe --help` 或 `phigros-render-pure.exe --help` 查看当前版本的帮助：

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `--width` | `1920` | 输出视频宽度；支持范围 16–7680 |
| `--height` | `1080` | 输出视频高度；支持范围 16–4320 |
| `--fps` | `60` | 输出帧率；支持范围 1–240 |
| `--start` | `0` | 片段开始时间，单位秒 |
| `--end` | 曲目结束 | 片段结束时间，必须大于 `--start` |
| `--no-hud` | 关闭 | 隐藏分数、连击、曲名和难度等游戏 HUD |
| `--show-accuracy` | 关闭 | 显示实时准确率百分比 |
| `--no-hitsound` | 关闭 | 不混入打击音，仍保留歌曲音乐 |
| `--music-volume` | `1.0` | 音乐音量倍率，范围 0–8 |
| `--hitsound-volume` | `0.5` | 打击音音量倍率，范围 0–8 |
| `--no-audio` | 关闭 | 输出无音轨视频 |
| `--samples` | `1` | MSAA 采样数，范围 1–8；较高值可能增加渲染时间 |
| `--skin` | 默认皮肤 | Phira 资源包目录或 ZIP 文件 |
| `--font` | 随附 Saira 字体；若不存在则回退 | HUD 曲名和难度使用的 TTF/OTF 字体 |
| `--ffmpeg` | 自动查找 | FFmpeg 可执行文件路径 |
| `--encoder` | `software` | `software` 或 `nvenc` |

渲染指定片段示例：

```powershell
phigros-render-cli.exe chart.pez clip.mp4 --start 30 --end 45
```

不包含打击音但保留音乐：

```powershell
phigros-render-cli.exe chart.pez video.mp4 --no-hitsound --music-volume 1 --hitsound-volume 0
```

指定皮肤、字体和 FFmpeg：

```powershell
phigros-render-cli.exe chart.pez video.mp4 `
  --skin ".\skins\my-skin.zip" `
  --font ".\fonts\Saira-Regular.ttf" `
  --ffmpeg ".\tools\ffmpeg.exe"
```

## 编码器与 GPU

CLI 默认使用软件 H.264 编码。NVIDIA 显卡支持 NVENC 且 FFmpeg 包含相应编码器时，可选硬件编码：

```powershell
phigros-render-cli.exe chart.pez video.mp4 --encoder nvenc
```

CLI 使用 Macroquad/OpenGL 在 GPU 上绘制离屏画面，之后把每帧读回 CPU，再交给 FFmpeg。`nvenc` 只加速视频压缩阶段；这不是全程 GPU 编码。若 NVENC 不可用，改用 `--encoder software`。CLI 不会因 NVENC 失败而自动切换编码器。

## 谱面转换

转换器不会生成视频：

```powershell
phigros-rpen.exe official.json converted.json
phigros-rpen.exe official.pez converted.pez
```

默认 `--mode standard` 输出 RPEN 运行所需数据，不附带完整官方谱面备份。需要保留备份时使用归档模式：

```powershell
phigros-rpen.exe official.json archived.json --mode archive
phigros-rpen.exe official.pez archived.pez --mode archive
```

RPE/RPEN 输入会被识别并保留，不会重复转换。PEZ/ZIP 输出会保留归档内其他资源。

`phigros-render-cli` 也接受 `--mode standard|archive`，用于选择官方谱面临时转换时是否附带原始谱面备份；默认是 `standard`。该参数是高级选项，当前不会出现在渲染 CLI 的自动帮助列表中。

## 常见问题

- **提示缺少 `harmonyos.ttf`、`phigros.ttf` 或 `bold.ttf`：** 从项目根目录运行，并确认 `work\Phira-Pro\assets` 资源目录完整。
- **提示无法启动 FFmpeg：** 安装 FFmpeg 并加入 `PATH`，或通过 `--ffmpeg` 指定路径。
- **NVENC 初始化或编码失败：** 确认使用 NVIDIA 显卡及支持 NVENC 的 FFmpeg；也可以切换 `--encoder software`。
- **纯渲染拒绝输入文件：** `phigros-render-pure` 只接受 RPEN；使用 `phigros-render-cli` 自动识别并转换官方谱面。
- **如何查看参数：** 运行 `phigros-render-cli.exe --help`、`phigros-render-pure.exe --help` 或 `phigros-rpen.exe --help`。

## 许可

本项目声明使用 PolyForm Noncommercial 1.0.0。使用和分发时请遵守仓库中的 `LICENSE`；第三方组件仍受各自许可证约束。
