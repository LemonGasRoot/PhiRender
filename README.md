# PhiRender

![PhiRender logo](apps/desktop/src/assets/phirender-logo.svg)

## 中文

PhiRender 是面向 Windows 10/11 x64 的 Phigros 谱面转换与 GPU 渲染工具，提供桌面 GUI、命令行程序、实时预览和视频导出。

### 下载与使用

从 [GitHub Releases](https://github.com/LemonGasRoot/PhiRender/releases/latest) 下载 Windows x64 压缩包，解压后运行 `PhiRender.exe`。请保留压缩包中的 `phirender-gpu-worker.exe` 和 `work` 资源目录。视频导出需要另行安装 FFmpeg，并确保 `ffmpeg.exe` 在系统 `PATH` 中；CLI 也支持通过参数指定 FFmpeg 路径。更多命令行用法见 [CLI 使用说明](README-CLI.md)。

### 功能

- 导入 Phigros 官方 JSON/PEZ、普通 RPE 和 RPEN 谱面。
- 将官方谱面转换为 RPEN 数据；现有 RPE/RPEN 谱面直接进入渲染流程。
- GUI 管理谱面、皮肤、字体、打击音效和视频导出设置。
- CLI 支持谱面转换、纯 RPEN 渲染、片段导出、音频导出和视频编码选项。
- 画面由 Phira Pro 渲染核心通过 OpenGL 绘制。NVENC 可加速视频编码；帧仍会从 GPU 回读到 CPU，因此整个视频管线并非全 GPU。

### 性能对比

**在相同谱面、1920×1080、60 FPS、H.264 NVENC 和近似输出码率（约 12 Mbps）下，PhiRender 比 PhiVideo 快 2.7×：58.28 秒对 157.71 秒。**

基准测试使用同一份《PANIC PARADISE》原始 PEZ，完整谱面共 7751 帧、时长约 129.17 秒。两边都是 H.264 NVENC，最终平均总码率分别约 12.34 Mbps 和 12.31 Mbps。计时从渲染命令启动到 MP4 完成；两种渲染器算法不同，近似码率也不等于主观画质完全一致，结果不代表逐像素相同。

### 已知渲染差异

本渲染引擎以 Phira Pro 渲染核心为基础，因此不可避免地可能出现部分 Hold 长条错位和流速表现差异（Phira 特性）。

### 从源码构建

需要 Rust、Node.js、Windows C++ Build Tools 和 WebView2 Runtime。按 [开发构建说明](apps/desktop/README.md)准备依赖并构建 GUI、CLI 与渲染 Worker。开发环境通过 `tools/restore-phira.ps1` 还原指定的 Phira Pro 源码版本。

### 许可证

PhiRender 源码以 GPL-3.0-only 发布。RPEN 作者已明确同意相关转换代码随 PhiRender 按 GPL-3.0-only 使用、修改和分发；项目自身以非商业方式维护。GPL 接收者仍拥有 GPL 明确授予的权利，包括商业使用。字体、皮肤和依赖的具体许可见 [第三方声明](THIRD_PARTY_NOTICES.md)。

---

## English

PhiRender is a Phigros chart converter and GPU renderer for Windows 10/11 x64. It includes a desktop GUI, command-line tools, preview rendering, and video export.

### Download and run

Download the Windows x64 archive from [GitHub Releases](https://github.com/LemonGasRoot/PhiRender/releases/latest), extract it, and run `PhiRender.exe`. Keep `phirender-gpu-worker.exe` and the bundled `work` resources beside it. Video export requires FFmpeg on your system `PATH`; the CLI can also accept an explicit FFmpeg path. See the [CLI guide](README-CLI.md).

### Features

- Imports official Phigros JSON/PEZ, regular RPE, and RPEN charts.
- Converts official charts to RPEN data and sends existing RPE/RPEN charts directly to rendering.
- GUI controls for charts, skins, fonts, hit sounds, and video export.
- CLI tools for conversion, pure RPEN rendering, segment/audio export, and video encoder settings.
- The Phira Pro rendering core draws through OpenGL. NVENC can accelerate encoding, but frames are read back from GPU to CPU, so the entire video pipeline is not GPU-only.

### Performance comparison

**On the same chart at 1920×1080, 60 FPS, H.264 NVENC, and a similar output bitrate (about 12 Mbps), PhiRender was 2.7× faster than PhiVideo: 58.28 seconds vs. 157.71 seconds.**

The benchmark used the same original *PANIC PARADISE* PEZ for both renderers: 7,751 frames and about 129.17 seconds. Both used H.264 NVENC; the final average total bitrates were about 12.34 Mbps and 12.31 Mbps. Timing covers the full command from launch until the MP4 was completed. The renderers use different visual algorithms; similar bitrate does not establish identical perceived image quality, and the output is not pixel-identical.

### Known rendering differences

This renderer is based on the Phira Pro rendering core. Some Hold-note bars may be misaligned, and scroll-speed behavior may differ; these are known Phira-core characteristics.

### Build from source

You need Rust, Node.js, Windows C++ Build Tools, and the WebView2 Runtime. Follow the [development build guide](apps/desktop/README.md) to build the GUI, CLI tools, and rendering worker. Development builds restore the pinned Phira Pro source with `tools/restore-phira.ps1`.

### License

PhiRender source is released under GPL-3.0-only. The RPEN author explicitly authorized the relevant conversion code to be used, modified, and distributed with PhiRender under GPL-3.0-only. PhiRender itself is maintained for noncommercial use; GPL recipients retain all rights granted by GPL, including commercial use. See [third-party notices](THIRD_PARTY_NOTICES.md) for fonts, skins, and dependencies.
