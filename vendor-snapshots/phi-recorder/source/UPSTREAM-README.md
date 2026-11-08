# phi-recorder

把 Phigros 谱面渲染成视频的无头渲染器：**C++17 + OpenGL 3.3**，读入 `chart.json`、曲绘和音乐，直接在 GPU 上逐帧画出判定线、四种音符、长条、打击特效和噪域（Noise Area），再经 ffmpeg 编码成 1920×1080@60 的 MP4。不需要打开游戏、不需要窗口、不需要用户输入。

![Director add-on：副歌段](docs/shots/director_120s.png)

---

## 目录

- [能力](#能力)
- [快速开始](#快速开始)
- [命令行参数](#命令行参数)
- [Add-ons](#add-ons)
- [示例渲染配方](#示例渲染配方)
- [长条长度的算法](#长条长度的算法)
- [噪域（Noise Area）](#噪域noise-area)
- [架构](#架构)
- [性能](#性能)
- [已知限制](#已知限制)
- [版本历史](#版本历史)
- [素材与许可](#素材与许可)

---

## 能力

| 能力 | 说明 |
| --- | --- |
| 谱面格式 | 官方 phi 格式 3 / 4.0（含 `blockAreaList`）、RPE / RPE-next（自动嗅探） |
| 噪域 | 官方 NoiseField 材质逐 pass 复刻：6-bit 块遮罩（屏幕 1/8 分辨率）+ 14 个全屏 pass（compose / edge / 5×glow / glowExtract / disabled / ready / copy / touchMask / main） |
| 音符 | Tap / Drag / Flick / Hold，above / below 两层，多押 MH 宽贴图，Hold 三段切图 |
| 打击特效 | 对齐 PhiVideo：5×6 HitFx 图集、正常透明度合成、屏幕朝向光圈、四个方形粒子；长条每半拍补发完整特效 |
| 音频 | 音乐 + 官方打击音自动混音（RMS 平衡 + 峰值限制器 + AAC 44.1k） |
| 视频 | GPU 回读 → NV12 → 多进程并行 x264 → 封装 MP4；支持 PNG 序列与单帧 |
| Add-ons | 全屏后处理插件，**并且可以改谱面**：注入噪域、隐藏音符/判定线、追加音符与判定线 |
| 观感开关 | 镜像、判定线金色/白色、音符尺寸、特效尺寸、背景模糊与明暗、流速、多押提示 |
| HUD | 默认关闭；可用 `--hud` 显示暂停符号、分数、自动连击、歌曲名和难度 |

### HUD（PhiRender 对照移植）

此分支加入可选 HUD。未传 `--hud` 时维持原有纯谱面输出。HUD 使用 OpenGL 纹理叠加到最终帧；歌曲信息默认使用随附的 Saira 字体。游戏内 HUD 字体可通过路径和字体家族参数指定：

```bash
out/phirender.exe --chart chart.json --bg illustration.png --time 50 --out frame.png \
  --w 1920 --h 1080 --hud \
  --hud-font path/to/font.ttf --hud-font-family "Font Family" \
  --info-font assets/default/hud/saira-regular.ttf --info-font-family Saira
```

HUD 的位置与字体按 PhiRender 对照校准。计数始终按绝对时间戳计算：普通音符在头判计入，长条在尾判完成时计入，因此单帧预览、跳转、分段导出与连续视频保持一致。Phigros 字体未随上游打包；本地已有授权字体可通过 `--hud-font` 指定。

---

## 快速开始

```bash
# 构建（MinGW-w64 g++ 8.1+；OpenGL 函数用自写加载器）
bash build.sh

# 运行时依赖：PATH 上的 ffmpeg（必须带 libx264）。2013 年的老构建与 9.x 新构建都可用：
# 音频编码器在新版没有 libvo_aacenc 时会自动降级到原生 aac，细节见 ffmpeg-compat-notes.md
# build.sh 会把所需的 libwinpthread-1.dll 和对应许可文本复制到 out/

# 单帧
./out/phirender.exe --chart test/desultory/chart.json --time 100 --out frame.png

# 整曲（背景和音乐会自动在谱面同目录找 illustration[Blur].png / music.ogg）
./out/phirender.exe --chart test/desultory/chart.json --video out/run.mp4

# 套一个后处理插件
./out/phirender.exe --chart test/desultory/chart.json --video out/run.mp4 --addon director
```

谱面目录约定：

```
test/desultory/
  chart.json              谱面
  music.ogg               音乐（同目录自动发现）
  illustrationBlur.png    背景（模糊版优先；没有就用 illustration.png 自动模糊）
  info.json               可选：duration / 曲名等
```

**素材是必需的**：`assets/default/` 里的音符贴图、判定线贴图、NoiseField GLSL 缺任何一个，`init` 都会直接失败（不是降级）。素材需从你自己的游戏安装中提取。

---

## 命令行参数

| 参数 | 说明 |
| --- | --- |
| `--chart <json>` | 谱面（官方 phi 或 RPE，自动识别） |
| `--bg <image>` | 背景图；**可省略**，自动找 `illustrationBlur.png` → `illustration.png` → `bg.png` → `bg.jpg` |
| `--assets <dir>` | 资源包根目录（默认 `assets/default`） |
| `--time <sec>` / `--out <png>` | 单帧渲染 |
| `--outdir <dir>` | PNG 序列 |
| `--video <mp4>` / `--from` / `--to` / `--fps` | 视频输出（默认 0..曲长，60fps） |
| `--w` / `--h` | 分辨率（默认 1920×1080） |
| `--crf` / `--preset` | x264 质量（默认 18 / medium） |
| `--encoders <n>` / `--pipe <nv12\|rgba>` | 并行编码进程数（0=自动）/ 回读格式 |
| `--audio` / `--mix-audio <wav>` / `--music-volume` / `--notes-volume` / `--no-hits` | 音频与混音 |
| `--addon <id\|dir>` / `--addon-param k=v` / `--list-addons` | 后处理插件（见下节） |
| `--mirror` | 左右镜像谱面（判定线、音符、噪域一起） |
| `--line-color <gold\|white>` | 判定线颜色（**打击特效恒为金色**） |
| `--note-size <x>` / `--fx-scale <x>` | 音符 / 特效尺寸倍率 |
| `--mh-hint <0\|1\|2>` | 多押贴图：跟随谱面 / 强制开 / 强制关 |
| `--bg-blur <px>` / `--bg-brightness <0..1>` / `--bg-dim` / `--bg-base-dim` | 背景明暗；模糊**恒为自动**（半径 = 源图宽/40），`--bg-blur 0` 是唯一关闭方式 |
| `--flow-speed <x>` | 音符流速倍率 |
| `--no-lines` / `--no-notes` / `--no-noise` | 分层隐藏（调试） |
| `--debug-noise <prefix>` | dump 噪域每个 pass 的中间纹理 |
| `--bench <n>` | 只渲染不落盘，报 ms/帧 |
| `--version` | 版本号 |

---

## Add-ons

一个 add-on 就是 `addons/<id>/` 里的 `addon.json` + `shader.frag`。它有两种作用，可以同时用：

1. **画面上叠后处理**：渲染完场景后跑一次全屏 pass；
2. **改谱面**：往谱面里注噪域、隐藏音符/判定线、加音符和判定线，然后走**正常的渲染管线**——注入的噪域会挡在音符前面，隐藏的音符连打击特效都不再产生。

**不开 add-on 时零开销**：渲染路径只多一个 `std::vector::empty()` 判断。

### manifest

```json
{
  "name": "Director (Desultory Signals)",
  "author": "phi-recorder",
  "version": "1.0",
  "shader": "shader.frag",
  "params": { "glitch": 0.01, "chroma": 2.0 },
  "textures": { "cover": "cover.png" },
  "blocks": [
    { "appear": 83.0, "disappear": 89.2,
      "enable": 84.2, "disable": 88.2,
      "x": 0.0, "y": 0.02, "w": 2.2, "h": 0.42, "rot": 0.0, "subtract": false }
  ],
  "chart": {
    "hideNotes": [[84.0, 88.4]],
    "hideLines": [[84.0, 88.4]],
    "notes": [{ "time": 90.0, "x": 0.0, "type": 1, "line": 0, "size": 1.0 }],
    "lines": [{ "appear": 84.2, "disappear": 88.4, "x": 0.0, "y": 0.0, "rot": 0.0 }]
  }
}
```

- `blocks` 的时间单位是秒，`x/y/w/h` 是谱面坐标（x∈[-1,1]，y≈[-0.56,0.56]，`w/h` 为完整尺寸）。
- `chart.notes[].time` 也是秒；`type` 用官方编号（1 tap / 2 drag / 3 hold / 4 flick）。
- `chart.lines[]` 造出来的是静态判定线，`appear/disappear` 之间 alpha=1，其余时间不画。

### shader 可用的 uniform

shader 直接写 `main()`，不需要 `#version`——头文件会自动补上。

| uniform | 含义 |
| --- | --- |
| `uScene` / `uResolution` / `uAspect` | 场景贴图 / 分辨率 / 宽高比 |
| `uTime` / `uFrame` / `uFps` | 秒 / 帧号 / 输出帧率 |
| `uProgress` / `uDuration` | 播放进度 0..1 / 总时长 |
| `uCombo` / `uNotes` | 已命中音符数 / 谱面音符总数 |
| `uBeat` / `uBpm` | 四分音符拍号 / BPM |
| `uSpeed` / `uLines` / `uZones` | 当前最快判定线流速 / 判定线数 / 可见噪域数 |
| `uHit` / `uLastNote` | 距上次命中秒数 / 该音符屏幕坐标（-1..1，y 向上） |
| `p_<name>` / `t_<name>` | `params` / `textures` 的每一项，自动声明 |

### 命令

```bash
--list-addons                        # 列出 addons/ 下所有插件
--addon vignette                     # 按 id
--addon bloom --addon vhs            # 可重复，按顺序链式叠加
--addon ./my/addon --addon-param strength=0.8
```

### 谱面自带插件（chart addons）

谱面目录下若有 `addons/`，其中**每个子文件夹都会被当作一个插件自动加载**（不需要 `--addon`），顺序按文件夹名排序：

```
<chart dir>/
  chart.json
  addons/
    config.json            # 各插件的参数
    vignette/              # addon.json + shader.frag
    glitch/
```

`config.json` 的键是文件夹名：

```json
{
  "vignette": { "params": { "strength": 0.9, "radius": 0.4 } },
  "glitch":   { "params": { "intensity": 1.4 } }
}
```

也可以省略 `params` 一层，直接写 `{"vignette": {"strength": 0.9}}`。

规则：

- 没有 `addons/` 文件夹 → 不加载任何谱面插件（也不会报错）。
- 谱面插件的参数**以 config.json 为准，命令行改不动**（`--addon-param` 只作用于 `--addon` 加载的全局插件）。
- 单个插件加载失败只打印原因并跳过，其余插件与整条渲染继续。
- 加载顺序：先 `--addon`（命令行），后谱面自带的，二者按此顺序叠加。

### 内置插件

| 插件 | 做什么 |
| --- | --- |
| `vignette` | 暗角 |
| `grain` | 胶片颗粒 |
| `chromatic` | 径向色差 |
| `bloom` | 5×5 阈值辉光 |
| `vhs` | 横向抖动 + 色差 + 扫描线 + 噪点 |
| `mask` | 用贴图当蒙版 |
| `progress` | 底边进度条（用 `uProgress`） |
| `hitflash` | 命中处辉光（用 `uHit` / `uLastNote`） |
| `anomaly` | 信号干扰：按拍触发的撕裂、色差、雪花 |
| `director` | 按音乐结构编排的整套演出（见下） |

### director：按音乐结构编排

先用 RMS 和音符密度把整曲分成 10 段，再给每段配不同的叠加：

| 段落 | 时间 | 手法 |
| --- | --- | --- |
| 前奏 | 0–10 s | 暗角收拢、轻微 grain |
| 铺底 | 10–25 s | 缓慢色差呼吸 |
| 进入 | 25–35 s | 逐拍 zoom 脉冲开始 |
| 主歌 | 35–55 s | 扫描线 + 轻微撕裂 |
| 副歌 1 | 55–83 s | 强 zoom 脉冲、撕裂、色差 |
| **无解段** | **83–89 s** | **注入噪域挡住判定区 + 隐藏全部音符 + 加一条竖判定线** |
| 回落 | 89–110 s | 只留 vignette 与 grain |
| 副歌 2 | 110–140 s | 撕裂 + 色差 + 闪光 |
| 终段 | 140–147 s | 辉光 + 慢速 zoom |
| 收尾 | 147 s+ | 淡出 |

「无解段」是这个插件最特别的部分：它不是画一层假贴图，而是**真的往谱面里插入了三个噪域块**（84.2–88.2 s 处于 Active 态，会遮挡音符），同时 `hideNotes` 把 84.0–88.4 s 的音符全部隐藏、`lines` 加了一条竖线。切换用 2.2 s 的 smoothstep 淡入淡出，不突兀。

![无解段](docs/shots/director_86s.png)

---

## 示例渲染配方

本机（i9-12900H + Iris Xe 核显）实测：

| 用途 | 命令 | 实测 |
| --- | --- | --- |
| AT 整曲 | `--chart test/desultory/chart.json --video out/at.mp4` | 9045 帧 / **45.9 fps wall** / 303 MB |
| AT + director | 上一行加 `--addon director` | 9045 帧 / 28.5 fps wall / 354 MB |
| IN 快速演示 | `--chart test/desultory_in/chart.json --video out/in.mp4 --preset ultrafast` | 8856 帧 / **290.6 fps wall** / 541 MB |
| 单帧 | `--time 145 --out preview.png` | 3.1 ms/帧（仅渲染） |
| 性能自检 | `--from 40 --to 60 --bench 150` | 1080p 3.08 ms＝324.8 fps；`--no-noise` 2.56 ms；720p 1.95 ms |
| 20 秒片段 | `--from 60 --to 80 --video out/clip.mp4` | 1200 帧 / 34.6 fps wall |

`--preset` 只影响 x264：`ultrafast` 比 `medium` 快约 6 倍，代价是同 crf 下码率约 ×1.8。

---

## 长条长度的算法

官谱长条使用 PhiVideo 的规则：主体长度 = 长条时长 × 长条自身 speed × 0.6 × 输出高度。未命中时，头部仍沿判定线速度积分移动；命中后头部固定在线上，主体按剩余时间缩短。改变判定线速度不会改变长条固有长度。

官方 JSON 直接使用原始 speed。官谱经 PhiRender 转换成 RPEN 后，`phiRenderOfficialHold: {"version":1,"speed":原始速度}` 保留这项信息，标准版与归档版均支持；RPE 的普通 speed 保持为 1，避免误改头部流速。没有此标记的原生 RPE 长条仍按其积分尾部规则处理。旧标准版若已丢失原始 speed，需要从原始官谱重新转换，不能可靠猜回。

回归样本：DS IN 65–84 秒的 68 个长条，原始 speed=5、时长约 74.257ms，1080p 的主体均约 240.594px。旧积分法错误算成 42.705、64.359 或 136.537px。详见 tests/rpe_regression.cpp。

## 噪域（Noise Area）

`blockAreaList`（官方 4.0）与 RPE-next 的噪域轨道统一成 `BlockArea` 模型，按五阶段 `Hidden / Disabled / Ready / Active` 推进；渲染分两层：

- **背景层**：Disabled / Ready 块，画在判定线**后面**（官方 sorting layer 2 < 3）
- **前景层**：Active 块的 compose + edge + 5 次 ping-pong glow + glowExtract，叠回场景（layer 4 > 3）

块遮罩是 R8 图、每像素 6 bit（ANormal / ASubtract / D+R Normal / D+R Subtract / R Normal / R Subtract），分辨率为屏幕的 1/8，用实例化四边形加色累加；glow 五次权重 `0.458568 / 0.282861 / 0.156589 / 0.073059 / 0.024948` 取自官方 config。

新生白闪按「区域进入可见窗口的起点」触发（`BlockArea::phaseStart`），逐区域独立，不是整屏同步。

---

## 架构

```
src/
  main.cpp      CLI、单帧/序列/视频/基准、进程编排
  gl.h/.cpp     Win32 隐藏窗口 + WGL 3.3 core + 手写函数加载
  gpu.h/.cpp    Shader / Texture / RenderTarget / 全屏 pass / 实例化仿射四边形
  chart.h/.cpp  官方 phi 解析、事件轨采样、floorPosition、BlockArea 变换
  rpe.h/.cpp    RPE / RPE-next 解析
  noise.h/.cpp  噪域 14 pass 管线、mask 光栅、三态合成
  renderer.h/.cpp 场景：背景/判定线/音符/hold/特效/噪域调度 + add-on 挂钩
  addon.h/.cpp  add-on 加载、manifest 解析、全屏 pass、谱面注入
  framesink.h/.cpp  回读 → NV12 → 并行 x264 → 封装
  audio.h/.cpp  ffmpeg 解码 + 打击音混音
  common.h      Vec/Mat4、官方 0..14 缓动
third_party/    cJSON + stb_image/stb_image_write
assets/default/ 官方样式资源包（Notes、hud/JudgeLine.png、noise/NoiseField、blockarea）
addons/         10 个内置插件
```

每帧顺序：背景 → 压暗 → 噪域 mask → Disabled/Ready → 判定线 → hold → tap → 打击特效 → 噪域 Active → add-on 链 → 回读。

---

## 性能

- 纯渲染 1080p：**2.1–3.1 ms/帧**（`--bench`，`glFinish` 同步计时，核显）
- 端到端 1080p60：**45.9 fps wall**（3 个并行编码器，x264 medium crf18）
- 编码器数量默认 `cpuCount()/6`（上限 8），可用 `--encoders N` 或 `PHI_SEGMENTS=N` 覆盖；每段线程数可用 `PHI_ENCODER_THREADS=N` 覆盖
- 回读走 PBO + NV12（GPU 内转换），不占 CPU

---

## 已知限制

- 只做**自动演示**：没有判定文字、没有 Miss 表现、没有触摸指示。
- 画面里没有 HUD 与结算画面（v0.99.9 起移除，是有意为之）。
- 噪域不绘制玩家触摸态（`touchMask` 管线保留但传空蒙版）。
- 仅 Windows（Win32 + WGL），需要真 GPU，无软件回退。
- 谱面素材需自备；缺文件会直接启动失败。
- 8-bit sRGB、无 HDR/色彩管理、输出 4:2:0，暗部可能有 banding。
- RPE 普通格式是近似支持，部分 alpha / size 关键帧可能丢失。

---

## 版本历史

| 版本 | 内容 |
| --- | --- |
| 0.1 – 0.12 | 基础渲染、噪域、官方 HUD（后删除）、打击音混音、背景参数 |
| 0.99.9 | 删除 HUD / 结算 / 文本栈，成为纯谱面渲染器；新增 `--mirror`、`--line-color` |
| 1.0.0-rc.1 | 长条首击特效、单编码器死锁修复、mux `-t` 修复 |
| 1.0.0-rc.2 | 长条长度改为积分式；背景模糊自动化、`--bg` 可省略 |
| 1.0.1-rc.1 – rc.3 | Add-ons：链式、贴图参数、`uProgress`/`uBeat`/`uHit` 等谱面状态 uniform |
| 1.0.1-rc.4 | `anomaly` 插件（异象） |
| 1.0.1-rc.5 | 长条长度修正（1:57 段） |
| 1.0.2-rc.1 | add-on 可改谱面：注入噪域、隐藏音符/判定线、追加音符与判定线；`director` 插件 |
| **1.1.0-stable** | **背景模糊恒自动；谱面自带 addons/ + config.json；错误捕获完善；隐藏/新增音符同步作用于打击音** |
| 1.1.1 | ffmpeg 新旧版本全适配：音频编码器按错误自动降级（libvo_aacenc → 原生 aac），concat 列表写法改为两代都认的形式；见 `ffmpeg-compat-notes.md` |

---

## 素材与许可

代码中与 [PhiVideo](https://github.com/phigrostl/PhiVideo)（MIT）、[Phira-Pro / prpr](https://github.com/Phira-Pro/prpr)（GPL-3.0）对齐的部分（判定线/音符数学、RPE 解析、噪域五阶段语义、NoiseField GLSL）遵循其原始许可；若要对外分发，请自行合规处理。

`assets/` 与 `test/` 下的贴图、谱面、音乐版权属 Pigeon Games，**不在本仓库代码许可范围内**，仅用于本地测试。本项目与官方无关。
