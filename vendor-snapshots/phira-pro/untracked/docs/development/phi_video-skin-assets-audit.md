# PhiVideo 皮肤素材与许可核查

核查对象：本地 PhiVideo checkout，HEAD `cfccda63c0f3e5dd00c5a02d17923dd5714c037a`（2026-10-25）。

## 结论

- PhiVideo 的源码许可证是 MIT；仓库 README 另行声明 `resources` 中的项目素材归南京鸽游网络有限公司所有、作者不拥有版权。因此 MIT 不能作为图片、音频和字体素材的再分发授权。
- 当前没有将 PhiVideo 的资源复制到 Phira Pro 或本项目的发行文件中。可在本地研究其用途；发行版需要使用有明确授权的资源或用户自行导入的素材。
- PhiVideo 的代码可在遵守 MIT 声明的条件下复用；本项目本次没有复制其 C++ 实现。渲染继续使用 Phira Pro 内核。
- PhiVideo 仓库未发现 GLSL Shader、噪域纹理或独立判定线贴图。噪域效果不能从它的素材目录中直接复用。
- 新建的 `assets/respack-open/` 是独立绘制的几何 Note/Hold/HitFx 贴图和合成短音效，附带 CC0 1.0 声明；默认 `ResourcePack::from_path(None)` 使用该目录。

## 素材用途（以加载代码为准）

| 资源 | 格式、尺寸 | 用途与加载证据 |
| --- | --- | --- |
| `resources/Notes/Tap.png` | PNG RGBA，989×100，单帧，含透明像素 | `NoteImgs::NoteImgs` 作为普通 Tap 贴图加载；渲染按音符类型选择贴图。 |
| `resources/Notes/Drag.png` | PNG RGBA，989×60，单帧，含透明像素 | 普通 Drag 贴图。 |
| `resources/Notes/Flick.png` | PNG RGBA，989×200，单帧，含透明像素 | 普通 Flick 贴图。 |
| `resources/Notes/Hold.png` | PNG RGBA，989×2000，单帧，含透明像素 | Hold 图集；`LoadImgs` 按 `UI.json` 的 `holdAtlas` 切成头、身、尾。 |
| `resources/Notes/TapMH.png` | PNG RGBA，1089×200，单帧，含透明像素 | 多押 Tap 贴图。 |
| `resources/Notes/DragMH.png` | PNG RGBA，1089×160，单帧，含透明像素 | 多押 Drag 贴图。 |
| `resources/Notes/FlickMH.png` | PNG RGBA，1089×300，单帧，含透明像素 | 多押 Flick 贴图。 |
| `resources/Notes/HoldMH.png` | PNG RGBA，1062×2048，单帧，含透明像素 | 多押 Hold 图集；按 `holdAtlasMH` 切片。 |
| `resources/Notes/HitFx.png` | PNG RGBA，1825×2190，单帧，含透明像素 | `LoadFxImgs` 按 5×6 配置切成 30 格打击动画帧，每格为 365×365。 |
| `resources/Font.ttf` | TTF，12,832,716 字节 | `Application::Initialize` 载入为渲染字体；`Framebuffer::LoadFontTTF` 用 stb_truetype 栅格化文字。 |
| `resources/Notes/Tap.wav` | WAV，16,842 字节 | Tap 打击音。 |
| `resources/Notes/Drag.wav` | WAV，83,354 字节 | Drag 打击音。 |
| `resources/Notes/Flick.wav` | WAV，71,774 字节 | Flick 打击音。 |
| `resources/UI.json` | JSON | 配置 Hold 图集切片、HitFx 网格、渲染开关、UI 文案和其他参数；由 `UI::UI` 解析。 |
| `resources/Languages/*` | 文本 | 控制台和日志本地化文本，不是游戏字体或渲染贴图。 |

以上 PNG 均是单帧 RGBA，Alpha 通道范围为 0–255。图像尺寸与透明度由本地 Pillow 检查得到。仓库没有单独的判定线贴图；`RenderJudgeLines` 调用 `Framebuffer::DrawLine` 画实色线。背景图来自谱面曲绘，`LoadImgs` 另调用 FFmpeg 生成高斯模糊背景。仓库也没有独立 UI 图标、噪域遮罩或噪声图集。

PhiVideo 的 `Framebuffer` 使用 `Vec3[]` CPU 像素缓冲和逐像素混合；字体由 stb_truetype CPU 光栅化。它不是可复用的 GPU 渲染后端。`BlockArea` 在谱面数据与渲染准备中作为区域几何处理，不依赖资源目录中的噪域 Shader。

## 本项目的皮肤接入

Phira Pro 已有 `prpr::core::ResourcePack`：从目录或 ZIP 读取 `info.yml`、Tap/Drag/Hold/Flick 普通及多押贴图、HitFx 和打击音，并验证 Hold 图集参数。默认路径已切到项目自制的 `assets/respack-open/`。`ChartVideoRenderer` 使用同一个 `GameScene` 渲染路径服务单帧、视频和可供预览读取的纹理。本次新增 `VideoOptions::res_pack_path` 与脚本 `-SkinPath`，将用户资源包路径传给既有 `Config::res_pack_path`，不改变 Note、判定线或噪域运动计算。

缺失必需贴图或配置时由 ResourcePack 加载器返回明确错误；自定义皮肤缺失打击音时回退到 `respack-open` 的 CC0 合成音效。没有指定自定义目录时使用同一个项目自制 CC0 资源包。目录中的自定义资源仍需确认各自的授权。本次不包含 PhiVideo 的专有资源。

当前自定义资源包覆盖 Note 外观、Hold 图集、打击图集及打击音。判定线仍按现有代码绘制，背景仍取自谱面曲绘；自定义字体与全局 UI 主题尚未接入 `ChartVideoRenderer`，需要后续单独扩展。Phira Pro 原仓库里原有 `assets/respack/` 的资源来源也没有独立授权文件；它不再作为运行时默认皮肤，但发行包还需通过打包清单排除它，或取得该资源包作者的再分发许可。

## 关键源码位置

- `src/PhiVideo/Application/Note.cpp`：NoteImgs 图片资源路径。
- `src/PhiVideo/Application/LoadFiles.cpp`：UI 配置读取、Hold 图集切片、背景模糊、HitFx 网格切片。
- `src/PhiVideo/Application/RenderFrame.cpp`：背景、判定线、Note、打击效果和 UI 的绘制调用顺序。
- `src/PhiVideo/Render/Framebuffer.cpp`：CPU 像素缓冲、文字与图片绘制。
- Phira Pro `prpr/src/core/resource.rs`：`ResourcePack`、`NoteStyle` 和 `ParticleEmitter`。
- Phira Pro `prpr/src/chart_video.rs`：统一视频/帧渲染器的资源包配置入口。

