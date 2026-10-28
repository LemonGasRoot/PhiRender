//! Private GPU worker for the desktop application. The chart renderer remains
//! the shared Phira Pro engine; this process owns its OpenGL event loop.
use anyhow::{anyhow, Context, Result};
use macroquad::{file::set_pc_assets_folder, prelude::*};
use prpr::{
    chart_video::{ChartVideoRenderer, VideoEncoder, VideoOptions},
    core::{BOLD_FONT, HUD_INFO_FONT, PGR_FONT},
    ext::poll_future,
    ui::{FontArc, TextPainter},
};
use serde::Deserialize;
use serde_json::json;
use std::{
    io::{BufRead, Read, Write},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    time::{Instant, SystemTime, UNIX_EPOCH},
};

#[derive(Deserialize)]
struct Job {
    kind: String,
    chart: PathBuf,
    output: PathBuf,
    project_root: PathBuf,
    skin: Option<PathBuf>,
    font: Option<PathBuf>,
    click_sound: Option<PathBuf>,
    drag_sound: Option<PathBuf>,
    flick_sound: Option<PathBuf>,
    width: u32,
    height: u32,
    fps: u32,
    time: f64,
    #[serde(default)]
    start: f64,
    #[serde(default)]
    end: Option<f64>,
    show_hud: bool,
    include_audio: bool,
    hitsounds: bool,
    music_volume: f32,
    hitsound_volume: f32,
    nvenc: bool,
}

fn conf() -> Conf {
    Conf {
        window_title: "PhiRender GPU worker".into(),
        window_width: 960,
        window_height: 720,
        headless: true,
        platform: miniquad::conf::Platform {
            swap_interval: Some(0),
            ..Default::default()
        },
        ..Default::default()
    }
}

fn event(value: serde_json::Value) {
    println!("{value}");
    let _ = std::io::stdout().flush();
}

fn read_font(path: &std::path::Path) -> Result<FontArc> {
    let bytes = std::fs::read(path).with_context(|| format!("无法读取字体 {}", path.display()))?;
    FontArc::try_from_vec(bytes).map_err(|e| anyhow!("无效的字体 {}: {e}", path.display()))
}

enum SkinSource {
    Directory(PathBuf),
    Archive(zip::ZipArchive<std::fs::File>, String),
}
impl SkinSource {
    fn open(path: &Path) -> Result<Self> {
        if path.is_dir() {
            return Ok(Self::Directory(path.to_path_buf()));
        }
        let mut zip = zip::ZipArchive::new(std::fs::File::open(path)?)?;
        let info: Vec<_> = zip
            .file_names()
            .filter(|n| n.rsplit('/').next() == Some("info.yml"))
            .map(str::to_owned)
            .collect();
        if info.len() != 1 {
            return Err(anyhow!("皮肤 ZIP 必须包含一个 info.yml"));
        }
        let prefix = info[0].strip_suffix("info.yml").unwrap_or("").to_owned();
        // Read once so an invalid or encrypted info file is reported early.
        let _ = zip.by_name(&info[0])?;
        Ok(Self::Archive(zip, prefix))
    }
    fn read(&mut self, name: &str) -> Result<Option<Vec<u8>>> {
        match self {
            Self::Directory(root) => match std::fs::read(root.join(name)) {
                Ok(bytes) => Ok(Some(bytes)),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
                Err(e) => Err(e.into()),
            },
            Self::Archive(zip, prefix) => {
                let mut entry = match zip.by_name(&format!("{prefix}{name}")) {
                    Ok(entry) => entry,
                    Err(zip::result::ZipError::FileNotFound) => return Ok(None),
                    Err(e) => return Err(e.into()),
                };
                if entry.size() > 256 * 1024 * 1024 {
                    return Err(anyhow!("皮肤资源过大: {name}"));
                }
                let mut bytes = Vec::new();
                entry.read_to_end(&mut bytes)?;
                Ok(Some(bytes))
            }
        }
    }
}

struct SkinStage {
    dir: PathBuf,
    files: Vec<PathBuf>,
}
impl SkinStage {
    fn create() -> Result<Self> {
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        let dir =
            std::env::temp_dir().join(format!("phirender-skin-{}-{stamp}", std::process::id()));
        std::fs::create_dir(&dir)?;
        Ok(Self {
            dir,
            files: Vec::new(),
        })
    }
    fn write(&mut self, name: &str, bytes: &[u8]) -> Result<PathBuf> {
        let path = self.dir.join(name);
        std::fs::write(&path, bytes)?;
        self.files.push(path.clone());
        Ok(path)
    }
}
impl Drop for SkinStage {
    fn drop(&mut self) {
        for path in &self.files {
            let _ = std::fs::remove_file(path);
        }
        let _ = std::fs::remove_dir(&self.dir);
    }
}

/// Build a temporary local resource pack with selected sound files unchanged.
fn prepare_skin(job: &Job, assets: &Path) -> Result<SkinStage> {
    if !job.hitsound_volume.is_finite() || !(0.0..=8.0).contains(&job.hitsound_volume) {
        return Err(anyhow!("打击音音量必须在 0 至 8 倍之间"));
    }
    let selected = job.skin.clone().unwrap_or_else(|| assets.join("respack"));
    let phira_default = selected == assets.join("respack");
    let mut source = SkinSource::open(&selected)
        .with_context(|| format!("无法打开皮肤 {}", selected.display()))?;
    let mut stage = SkinStage::create()?;
    for name in [
        "info.yml",
        "click.png",
        "click_mh.png",
        "drag.png",
        "drag_mh.png",
        "flick.png",
        "flick_mh.png",
        "hold.png",
        "hold_mh.png",
        "hit_fx.png",
    ] {
        let bytes = source
            .read(name)?
            .with_context(|| format!("皮肤缺少 {name}"))?;
        stage.write(name, &bytes)?;
    }
    for ext in ["ogg", "wav", "mp3"] {
        let name = format!("ending.{ext}");
        if let Some(bytes) = source.read(&name)? {
            stage.write(&name, &bytes)?;
            break;
        }
    }
    for (kind, override_path) in [
        ("click", &job.click_sound),
        ("drag", &job.drag_sound),
        ("flick", &job.flick_sound),
    ] {
        let mut chosen: Option<(Vec<u8>, String)> = None;
        if let Some(path) = override_path {
            let ext = path
                .extension()
                .and_then(|v| v.to_str())
                .unwrap_or("ogg")
                .to_ascii_lowercase();
            chosen = Some((
                std::fs::read(path)
                    .with_context(|| format!("无法读取打击音 {}", path.display()))?,
                ext,
            ));
        } else {
            for ext in ["ogg", "wav", "mp3"] {
                if let Some(bytes) = source.read(&format!("{kind}.{ext}"))? {
                    chosen = Some((bytes, ext.into()));
                    break;
                }
            }
            if chosen.is_none() && phira_default {
                let file = assets.join(format!("{kind}.ogg"));
                if file.is_file() {
                    chosen = Some((std::fs::read(file)?, "ogg".into()));
                }
            }
            if chosen.is_none() {
                for ext in ["ogg", "wav", "mp3"] {
                    let file = assets.join("respack-open").join(format!("{kind}.{ext}"));
                    if file.is_file() {
                        chosen = Some((std::fs::read(file)?, ext.into()));
                        break;
                    }
                }
            }
        }
        let (bytes, ext) = chosen.with_context(|| format!("未找到 {kind} 打击音"))?;
        if !["ogg", "wav", "mp3"].contains(&ext.as_str()) {
            return Err(anyhow!("不支持的打击音格式：{ext}"));
        }
        stage.write(&format!("{kind}.{ext}"), &bytes)?;
    }
    Ok(stage)
}

#[macroquad::main(conf)]
async fn main() {
    let result = run().await;
    if let Err(error) = result {
        event(json!({"type":"error", "message":format!("{error:#}")}));
        std::process::exit(1);
    }
}

async fn run() -> Result<()> {
    let job_path = std::env::args_os().nth(1).context("缺少任务文件")?;
    let job: Job = serde_json::from_slice(&std::fs::read(job_path)?)?;
    if !job.music_volume.is_finite() || !(0.0..=8.0).contains(&job.music_volume) {
        return Err(anyhow!("音乐音量必须在 0 至 8 倍之间"));
    }
    let cancelled = Arc::new(AtomicBool::new(false));
    let listener = cancelled.clone();
    std::thread::spawn(move || {
        for line in std::io::stdin().lock().lines().map_while(|line| line.ok()) {
            if line.trim() == "cancel" {
                listener.store(true, Ordering::Relaxed);
                break;
            }
        }
    });
    let runtime = tokio::runtime::Runtime::new().context("无法启动任务运行环境")?;
    let _guard = runtime.enter();
    let assets = job.project_root.join("work/Phira-Pro/assets");
    set_pc_assets_folder(assets.to_string_lossy().as_ref());
    let fallback = read_font(&assets.join("harmonyos.ttf"))?;
    let mut painter = TextPainter::new(fallback.clone(), None);
    let reference_font = read_font(&assets.join("phigros.ttf"))?;
    let font_path = job
        .font
        .clone()
        .unwrap_or_else(|| assets.join("phigros.ttf"));
    let font = read_font(&font_path)?;
    let mut gameplay_font = TextPainter::new(font, Some(fallback.clone()));
    gameplay_font.normalize_to(&reference_font);
    HUD_INFO_FONT.with(|slot| *slot.borrow_mut() = Some(gameplay_font));
    PGR_FONT.with(|slot| {
        *slot.borrow_mut() = Some(TextPainter::new(reference_font, Some(fallback.clone())))
    });
    let bold = read_font(&assets.join("bold.ttf"))?;
    BOLD_FONT.with(|slot| *slot.borrow_mut() = Some(TextPainter::new(bold, Some(fallback))));
    if std::env::var_os("PHIRA_FFMPEG").is_none() {
        for path in [
            "work/ffmpeg-package/extracted/ffmpeg-9.0.1-essentials_build/bin/ffmpeg.exe",
            "work/ffmpeg-8.0.1-extracted/ffmpeg-8.0.1-essentials_build/bin/ffmpeg.exe",
        ] {
            let path = job.project_root.join(path);
            if path.is_file() {
                std::env::set_var("PHIRA_FFMPEG", path);
                break;
            }
        }
    }
    let skin_stage = prepare_skin(&job, &assets)?;
    let opts = VideoOptions {
        width: job.width,
        height: job.height,
        fps: job.fps,
        start: job.start,
        end: job.end,
        show_hud: job.show_hud,
        include_audio: job.include_audio,
        hitsound: job.hitsounds,
        music_volume: job.music_volume,
        hitsound_volume: if job.kind == "preview" {
            0.
        } else {
            job.hitsound_volume
        },
        sample_count: 1,
        res_pack_path: Some(skin_stage.dir.clone()),
        encoder: if job.nvenc {
            VideoEncoder::H264Nvenc
        } else {
            VideoEncoder::H264Software
        },
        ..Default::default()
    };
    next_frame().await;
    if job.kind == "preview" {
        let mut load = Box::pin(ChartVideoRenderer::new_preview(job.chart, opts));
        let mut renderer = loop {
            if cancelled.load(Ordering::Relaxed) {
                event(json!({"type":"cancelled"}));
                return Ok(());
            }
            if let Some(result) = poll_future(load.as_mut()) {
                break result?;
            }
            next_frame().await;
        };
        let end = job.time.clamp(0., renderer.track_length());
        let mut t = (end - 1.).max(0.);
        renderer.render_preview_at(&mut painter, t)?;
        // Let old effects expire without modifying the shared rendering engine.
        if t > 0. {
            for _ in 0..job.fps {
                renderer.render_preview_at(&mut painter, t)?;
            }
        }
        while t < end {
            t = (t + 1. / f64::from(job.fps)).min(end);
            renderer.render_preview_at(&mut painter, t)?;
        }
        renderer
            .texture()
            .get_texture_data()
            .export_png(&job.output.to_string_lossy());
        event(
            json!({"type":"preview", "path":job.output, "title":renderer.title(), "level":renderer.level(), "duration":renderer.track_length(), "time":job.time}),
        );
    } else if job.kind == "audio_export" {
        let file = std::fs::File::open(&job.chart)?;
        let mut archive = zip::ZipArchive::new(file)?;
        let info_name = archive
            .file_names()
            .find(|n| {
                n.rsplit('/')
                    .next()
                    .is_some_and(|b| b.eq_ignore_ascii_case("info.txt"))
            })
            .map(str::to_owned)
            .context("谱面归档缺少 info.txt")?;
        let mut info = String::new();
        archive.by_name(&info_name)?.read_to_string(&mut info)?;
        let song = info
            .lines()
            .find_map(|line| {
                let (key, value) = line.split_once(':')?;
                key.trim()
                    .eq_ignore_ascii_case("song")
                    .then(|| value.trim().to_owned())
            })
            .filter(|s| !s.is_empty())
            .context("谱面信息中没有歌曲音频路径")?;
        let song = song.replace('\\', "/");
        let song_lower = song.to_ascii_lowercase();
        let song_entry = archive
            .file_names()
            .find(|name| {
                let lower = name.to_ascii_lowercase();
                lower == song_lower || lower.ends_with(&format!("/{song_lower}"))
            })
            .map(str::to_owned)
            .with_context(|| format!("找不到歌曲音频：{song}"))?;
        let mut entry = archive
            .by_name(&song_entry)
            .with_context(|| format!("找不到歌曲音频：{song}"))?;
        let ext = Path::new(&song)
            .extension()
            .and_then(|e| e.to_str())
            .unwrap_or("audio");
        let source =
            std::env::temp_dir().join(format!("phirender-audio-{}-{ext}", std::process::id()));
        std::fs::write(&source, {
            let mut bytes = Vec::new();
            entry.read_to_end(&mut bytes)?;
            bytes
        })?;
        let duration = job.end.map(|end| (end - job.start).max(0.));
        let ffmpeg = std::env::var_os("PHIRA_FFMPEG")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("ffmpeg"));
        let mut command = std::process::Command::new(ffmpeg);
        command
            .arg("-hide_banner")
            .arg("-loglevel")
            .arg("error")
            .arg("-y");
        if job.start > 0. {
            command.arg("-ss").arg(format!("{:.6}", job.start));
        }
        command.arg("-i").arg(&source);
        if let Some(duration) = duration {
            command.arg("-t").arg(format!("{duration:.6}"));
        }
        let mut child = command
            .args(["-vn", "-c:a", "aac", "-b:a", "192k"])
            .arg("-af")
            .arg(format!("volume={:.6}", job.music_volume))
            .arg(&job.output)
            .stdin(std::process::Stdio::null())
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .spawn()
            .context("无法启动 FFmpeg 音频导出")?;
        let status = loop {
            if cancelled.load(Ordering::Relaxed) {
                let _ = child.kill();
                let _ = child.wait();
                let _ = std::fs::remove_file(&source);
                let _ = std::fs::remove_file(&job.output);
                event(json!({"type":"cancelled"}));
                return Ok(());
            }
            if let Some(status) = child.try_wait()? {
                break status;
            }
            std::thread::sleep(std::time::Duration::from_millis(100));
        };
        let _ = std::fs::remove_file(source);
        if !status.success() {
            return Err(anyhow!("FFmpeg 音频导出失败：{status}"));
        }
        event(json!({"type":"complete", "path":job.output}));
    } else if job.kind == "export" {
        let mut load = Box::pin(ChartVideoRenderer::new(job.chart, opts, job.output.clone()));
        let mut renderer = loop {
            if cancelled.load(Ordering::Relaxed) {
                event(json!({"type":"cancelled"}));
                return Ok(());
            }
            if let Some(result) = poll_future(load.as_mut()) {
                break result?;
            }
            next_frame().await;
        };
        let begun = Instant::now();
        while !renderer.finished() {
            if cancelled.load(Ordering::Relaxed) {
                renderer.cancel();
                event(json!({"type":"cancelled"}));
                return Ok(());
            }
            renderer.render_next_frame(&mut painter)?;
            let (frame, total) = renderer.progress();
            if frame % 30 == 0 || frame == total {
                event(
                    json!({"type":"progress", "frame":frame, "total":total, "elapsed":begun.elapsed().as_secs_f64()}),
                );
            }
        }
        let output = renderer.finish()?;
        event(json!({"type":"complete", "path":output}));
    } else {
        return Err(anyhow!("不支持的渲染任务"));
    }
    Ok(())
}
