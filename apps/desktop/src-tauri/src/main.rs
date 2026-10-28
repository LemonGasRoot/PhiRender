#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use phigros_rpen_converter::{
    archive, convert_json_with_mode, parse_document, ChartFormat, OutputMode,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    io::{BufRead, BufReader, Write},
    path::{Path, PathBuf},
    process::{ChildStdin, Command, Stdio},
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex,
    },
};
use tauri::{Manager, State};

static NEXT_ID: AtomicU64 = AtomicU64::new(1);

#[derive(Clone, Serialize, Deserialize)]
#[serde(default)]
struct Settings {
    theme: String,
    skin: Option<String>,
    font: Option<String>,
    click_sound: Option<String>,
    drag_sound: Option<String>,
    flick_sound: Option<String>,
    hitsound_volume: f32,
    hitsounds: bool,
    show_hud: bool,
    include_audio: bool,
    width: u32,
    height: u32,
    fps: u32,
    nvenc: bool,
    auto_preview: bool,
    preview_resolution: u32,
    settings_version: u32,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            theme: "system".into(),
            skin: None,
            font: None,
            click_sound: None,
            drag_sound: None,
            flick_sound: None,
            hitsound_volume: 1.0,
            hitsounds: true,
            show_hud: true,
            include_audio: true,
            width: 1920,
            height: 1080,
            fps: 60,
            nvenc: true,
            auto_preview: true,
            preview_resolution: 480,
            settings_version: 0,
        }
    }
}

#[derive(Clone, Serialize)]
struct ChartView {
    source: String,
    title: String,
    level: String,
    format: String,
    notes: usize,
    lines: usize,
    noise: usize,
    warnings: Vec<String>,
    music: bool,
}
struct Chart {
    view: ChartView,
    prepared: PathBuf,
}

#[derive(Clone, Serialize, Default)]
struct TaskView {
    kind: String,
    status: String,
    message: String,
    frame: u64,
    total: u64,
    elapsed: f64,
    duration: f64,
    time: f64,
    output: Option<String>,
    frame_ready: bool,
}
struct Inner {
    root: PathBuf,
    cache: PathBuf,
    settings: Settings,
    chart: Option<Chart>,
    task: TaskView,
    stdin: Option<ChildStdin>,
    preview_task: TaskView,
    preview_stdin: Option<ChildStdin>,
    preview_path: Option<PathBuf>,
    preview_video_path: Option<PathBuf>,
    music: Option<PathBuf>,
}
struct AppState(Arc<Mutex<Inner>>);

fn msg(e: impl std::fmt::Display) -> String {
    e.to_string()
}
fn format_name(f: ChartFormat) -> String {
    match f {
        ChartFormat::OfficialV3 => "Phigros 官方 v3",
        ChartFormat::Rpe => "RPE",
        ChartFormat::Rpen => "RPEN",
    }
    .into()
}
fn unique() -> u64 {
    NEXT_ID.fetch_add(1, Ordering::Relaxed)
}
fn project_root() -> PathBuf {
    if let Ok(exe) = std::env::current_exe() {
        for root in exe.ancestors().take(4) {
            if root.join("work/Phira-Pro/assets").is_dir() {
                return root.to_path_buf();
            }
        }
    }
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(3)
        .unwrap()
        .to_path_buf()
}
fn app_cache() -> PathBuf {
    let candidate = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|p| p.join("phirender-cache")));
    if let Some(path) = candidate {
        if std::fs::create_dir_all(&path).is_ok() {
            return path;
        }
    }
    let path = std::env::temp_dir().join("phirender-desktop");
    let _ = std::fs::create_dir_all(&path);
    path
}
fn write_settings(inner: &Inner) -> Result<(), String> {
    let bytes = serde_json::to_vec_pretty(&inner.settings).map_err(msg)?;
    std::fs::write(inner.cache.join("settings.json"), bytes).map_err(msg)
}
fn initial_state() -> Inner {
    let root = project_root();
    let cache = app_cache();
    let mut settings: Settings = std::fs::read(cache.join("settings.json"))
        .ok()
        .and_then(|b| serde_json::from_slice(&b).ok())
        .unwrap_or_default();
    if settings.settings_version == 0 {
        settings.nvenc = true;
    }
    if settings.settings_version < 4 {
        settings.hitsound_volume = settings.hitsound_volume.min(1.0);
        settings.settings_version = 4;
    }
    if settings.settings_version < 5 {
        settings.preview_resolution = 480;
        settings.settings_version = 5;
    }
    if settings.settings_version < 6 {
        settings.hitsound_volume = settings.hitsound_volume.min(0.35);
        settings.settings_version = 6;
    }
    if settings.skin.is_none() {
        let phira = root.join("work/Phira-Pro/assets/respack");
        if phira.is_dir() {
            settings.skin = Some(phira.to_string_lossy().into_owned());
        }
    }
    if settings.font.is_none() {
        if let Some(font) = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(|p| p.join("saira-regular.ttf")))
            .filter(|p| p.is_file())
        {
            settings.font = Some(font.to_string_lossy().into_owned());
        }
    }
    let inner = Inner {
        root,
        cache,
        settings,
        chart: None,
        task: TaskView::default(),
        stdin: None,
        preview_task: TaskView::default(),
        preview_stdin: None,
        preview_path: None,
        preview_video_path: None,
        music: None,
    };
    let _ = write_settings(&inner);
    inner
}
fn is_archive(path: &Path) -> bool {
    path.extension()
        .and_then(|s| s.to_str())
        .is_some_and(|s| s.eq_ignore_ascii_case("pez") || s.eq_ignore_ascii_case("zip"))
}
fn info_value(files: &BTreeMap<String, Vec<u8>>, key: &str) -> Option<String> {
    files
        .iter()
        .filter(|(name, _)| {
            name.rsplit('/').next().is_some_and(|n| {
                n.eq_ignore_ascii_case("info.txt") || n.eq_ignore_ascii_case("info.yml")
            })
        })
        .filter_map(|(_, bytes)| std::str::from_utf8(bytes).ok())
        .flat_map(str::lines)
        .filter_map(|line| line.split_once(':'))
        .find_map(|(k, v)| {
            if k.trim().eq_ignore_ascii_case(key) {
                Some(v.trim().trim_matches(['\'', '"']).to_owned())
            } else {
                None
            }
        })
        .filter(|s| !s.is_empty())
}
fn count_notes(doc: &Value) -> usize {
    doc["judgeLineList"].as_array().map_or(0, |lines| {
        lines
            .iter()
            .map(|line| {
                line["notes"].as_array().map_or(0, Vec::len)
                    + line["notesAbove"].as_array().map_or(0, Vec::len)
                    + line["notesBelow"].as_array().map_or(0, Vec::len)
            })
            .sum()
    })
}
fn prepare_chart(source: &Path, cache: &Path, music: Option<&Path>) -> Result<Chart, String> {
    let bytes = std::fs::read(source).map_err(msg)?;
    let fallback_title = source
        .file_stem()
        .unwrap_or_default()
        .to_string_lossy()
        .into_owned();
    let (packed, format, notes, lines, noise, warnings, title, level, has_music) = if is_archive(
        source,
    ) {
        let (packed, report) =
            archive::convert_archive_with_mode(&bytes, OutputMode::Standard).map_err(msg)?;
        let files = archive::read_zip(&packed).map_err(msg)?;
        let title = info_value(&files, "Name").unwrap_or_else(|| fallback_title.clone());
        let level = info_value(&files, "Level").unwrap_or_default();
        let music = info_value(&files, "Song")
            .or_else(|| info_value(&files, "music"))
            .is_some();
        (
            packed,
            report.format,
            report.notes,
            report.judge_lines,
            report.noise_areas,
            report.warnings,
            title,
            level,
            music,
        )
    } else {
        let text = std::str::from_utf8(&bytes).map_err(msg)?;
        let parsed = parse_document(text).map_err(msg)?;
        let (converted, report) =
            convert_json_with_mode(text, OutputMode::Standard).map_err(msg)?;
        let doc: Value = serde_json::from_str(&converted).map_err(msg)?;
        let title = doc["META"]["name"]
            .as_str()
            .filter(|s| !s.is_empty())
            .unwrap_or(&fallback_title)
            .to_owned();
        let level = doc["META"]["level"].as_str().unwrap_or("").to_owned();
        let notes = count_notes(&doc);
        let lines = doc["judgeLineList"].as_array().map_or(0, Vec::len);
        let noise = doc["blockAreaList"].as_array().map_or(0, Vec::len);
        let warnings = report
            .diagnostics
            .iter()
            .filter(|d| {
                matches!(
                    d.severity,
                    phigros_rpen_converter::converter::Severity::Warning
                )
            })
            .map(|d| format!("{}: {}", d.path, d.message))
            .collect();
        let mut files = BTreeMap::new();
        files.insert("chart.json".to_string(), converted.into_bytes());
        let music_name = if let Some(path) = music {
            let ext = path.extension().and_then(|s| s.to_str()).unwrap_or("ogg");
            let name = format!("music.{ext}");
            files.insert(name.clone(), std::fs::read(path).map_err(msg)?);
            name
        } else {
            String::new()
        };
        let info = format!("#\nName: {title}\nPath: 00000000\nSong: {music_name}\nPicture: \nChart: chart.json\nLevel: {level}\nComposer: \nCharter: \nIllustrator: \n");
        files.insert("info.txt".into(), info.into_bytes());
        (
            archive::write_zip(&files).map_err(msg)?,
            parsed.format,
            notes,
            lines,
            noise,
            warnings,
            title,
            level,
            music.is_some(),
        )
    };
    let prepared = cache.join(format!("chart-{}-{}.pez", std::process::id(), unique()));
    std::fs::write(&prepared, packed).map_err(msg)?;
    Ok(Chart {
        view: ChartView {
            source: source.to_string_lossy().into_owned(),
            title,
            level,
            format: format_name(format),
            notes,
            lines,
            noise,
            warnings,
            music: has_music,
        },
        prepared,
    })
}

#[tauri::command]
fn get_settings(state: State<'_, AppState>) -> Settings {
    state.0.lock().unwrap().settings.clone()
}
#[tauri::command]
fn save_settings(state: State<'_, AppState>, settings: Settings) -> Result<(), String> {
    if !["light", "dark", "system"].contains(&settings.theme.as_str())
        || !settings.hitsound_volume.is_finite()
        || !(0.0..=1.0).contains(&settings.hitsound_volume)
        || ![30, 60, 120].contains(&settings.fps)
        || ![480, 720, 1080].contains(&settings.preview_resolution)
        || ![(1280, 720), (1920, 1080), (2560, 1440), (3840, 2160)]
            .contains(&(settings.width, settings.height))
    {
        return Err("设置值无效".into());
    }
    let mut inner = state.0.lock().unwrap();
    inner.settings = settings;
    write_settings(&inner)
}
#[tauri::command]
async fn open_chart(state: State<'_, AppState>) -> Result<Option<ChartView>, String> {
    let shared = state.0.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let Some(path) = rfd::FileDialog::new()
            .add_filter("谱面", &["json", "pez", "zip"])
            .pick_file()
        else {
            return Ok(None);
        };
        let cache = {
            let inner = shared.lock().unwrap();
            if inner.stdin.is_some() || inner.preview_stdin.is_some() {
                return Err("请先结束当前渲染任务和后台预览视频".into());
            }
            inner.cache.clone()
        };
        let chart = prepare_chart(&path, &cache, None)?;
        let view = chart.view.clone();
        let mut inner = shared.lock().unwrap();
        if let Some(previous) = inner.chart.replace(chart) {
            let _ = std::fs::remove_file(previous.prepared);
        }
        inner.music = None;
        inner.preview_path = None;
        if let Some(old) = inner.preview_video_path.take() {
            let _ = std::fs::remove_file(old);
        }
        inner.task = TaskView::default();
        inner.preview_task = TaskView::default();
        Ok(Some(view))
    })
    .await
    .map_err(msg)?
}
#[tauri::command]
async fn attach_music(state: State<'_, AppState>) -> Result<Option<ChartView>, String> {
    let shared = state.0.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let Some(music) = rfd::FileDialog::new()
            .add_filter("音乐", &["ogg", "mp3", "wav", "flac"])
            .pick_file()
        else {
            return Ok(None);
        };
        let (source, cache) = {
            let inner = shared.lock().unwrap();
            if inner.stdin.is_some() || inner.preview_stdin.is_some() {
                return Err("请先结束当前渲染任务和后台预览视频".into());
            }
            let chart = inner.chart.as_ref().ok_or("请先打开 JSON 谱面")?;
            (PathBuf::from(&chart.view.source), inner.cache.clone())
        };
        if is_archive(&source) {
            return Err("PEZ 使用包内音乐；请选择 JSON 谱面后关联音乐".into());
        }
        let chart = prepare_chart(&source, &cache, Some(&music))?;
        let view = chart.view.clone();
        let mut inner = shared.lock().unwrap();
        if let Some(old) = inner.chart.replace(chart) {
            let _ = std::fs::remove_file(old.prepared);
        }
        inner.music = Some(music);
        inner.preview_path = None;
        if let Some(old) = inner.preview_video_path.take() {
            let _ = std::fs::remove_file(old);
        }
        Ok(Some(view))
    })
    .await
    .map_err(msg)?
}
#[tauri::command]
async fn choose_font() -> Result<Option<String>, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let Some(path) = rfd::FileDialog::new()
            .add_filter("字体", &["ttf", "otf"])
            .pick_file()
        else {
            return Ok(None);
        };
        let b = std::fs::read(&path).map_err(msg)?;
        if b.len() < 12 || !(&b[..4] == b"OTTO" || &b[..4] == [0, 1, 0, 0]) {
            return Err("请选择有效的 TTF 或 OTF 字体".into());
        }
        Ok(Some(path.to_string_lossy().into_owned()))
    })
    .await
    .map_err(msg)?
}
#[tauri::command]
async fn choose_sound() -> Result<Option<String>, String> {
    tauri::async_runtime::spawn_blocking(move || {
        Ok(rfd::FileDialog::new()
            .add_filter("打击音", &["ogg", "wav", "mp3"])
            .pick_file()
            .map(|p| p.to_string_lossy().into_owned()))
    })
    .await
    .map_err(msg)?
}
#[tauri::command]
async fn choose_skin(folder: bool) -> Result<Option<String>, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let dialog = rfd::FileDialog::new();
        let path = if folder {
            dialog.pick_folder()
        } else {
            dialog.add_filter("皮肤 ZIP", &["zip"]).pick_file()
        };
        let Some(path) = path else { return Ok(None) };
        let required = [
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
        ];
        if path.is_dir() {
            if let Some(missing) = required.iter().find(|n| !path.join(n).is_file()) {
                return Err(format!("皮肤缺少 {missing}"));
            }
        } else {
            let files = archive::read_zip(&std::fs::read(&path).map_err(msg)?).map_err(msg)?;
            let infos: Vec<_> = files
                .keys()
                .filter(|n| n.rsplit('/').next() == Some("info.yml"))
                .collect();
            if infos.len() != 1 {
                return Err("皮肤 ZIP 必须包含一个 info.yml".into());
            }
            let prefix = infos[0].strip_suffix("info.yml").unwrap_or("");
            if let Some(missing) = required
                .iter()
                .find(|n| !files.contains_key(&format!("{prefix}{n}")))
            {
                return Err(format!("皮肤缺少 {missing}"));
            }
        }
        Ok(Some(path.to_string_lossy().into_owned()))
    })
    .await
    .map_err(msg)?
}
#[tauri::command]
fn default_skin(state: State<'_, AppState>) -> Result<String, String> {
    let path = state
        .0
        .lock()
        .unwrap()
        .root
        .join("work/Phira-Pro/assets/respack");
    if !path.is_dir() {
        return Err("找不到本机 Phira 默认皮肤，请选择资源包".into());
    }
    Ok(path.to_string_lossy().into_owned())
}
#[tauri::command]
fn get_task(state: State<'_, AppState>) -> TaskView {
    state.0.lock().unwrap().task.clone()
}
#[tauri::command]
fn get_preview_task(state: State<'_, AppState>) -> TaskView {
    state.0.lock().unwrap().preview_task.clone()
}
#[tauri::command]
fn get_preview_frame(state: State<'_, AppState>) -> Result<Vec<u8>, String> {
    let path = state
        .0
        .lock()
        .unwrap()
        .preview_path
        .clone()
        .ok_or("尚无预览画面")?;
    std::fs::read(path).map_err(msg)
}
#[tauri::command]
fn get_preview_video(app: tauri::AppHandle, state: State<'_, AppState>) -> Result<String, String> {
    let path = state
        .0
        .lock()
        .unwrap()
        .preview_video_path
        .clone()
        .ok_or("尚无预览视频")?;
    app.asset_protocol_scope().allow_file(&path).map_err(msg)?;
    Ok(path.to_string_lossy().into_owned())
}
fn worker_path() -> Result<PathBuf, String> {
    let exe = std::env::current_exe().map_err(msg)?;
    let path = exe
        .parent()
        .ok_or("无法定位应用目录")?
        .join("phirender-gpu-worker.exe");
    if !path.is_file() {
        return Err(format!("找不到渲染组件：{}", path.display()));
    }
    Ok(path)
}
fn start_task(
    shared: Arc<Mutex<Inner>>,
    kind: &str,
    time: f64,
    output: Option<PathBuf>,
    range: Option<(f64, Option<f64>)>,
) -> Result<(), String> {
    let mut inner = shared.lock().unwrap();
    let background = kind == "preview_video";
    if if background {
        inner.preview_stdin.is_some()
    } else {
        inner.stdin.is_some()
    } {
        return Err("已有渲染任务正在运行".into());
    }
    let chart = inner.chart.as_ref().ok_or("请先打开谱面")?;
    if !chart.view.music {
        return Err("谱面没有音乐：请在工作区关联音乐文件后再渲染".into());
    }
    let id = unique();
    let dest = if kind == "preview" {
        inner
            .cache
            .join(format!("preview-{}-{id}.png", std::process::id()))
    } else if kind == "preview_video" {
        inner
            .cache
            .join(format!("preview-{}-{id}.mp4", std::process::id()))
    } else if kind == "audio_export" {
        inner
            .cache
            .join(format!("audio-{}-{id}.m4a", std::process::id()))
    } else {
        let final_path = output.as_ref().ok_or("没有选择输出文件")?;
        if final_path.exists() {
            return Err("输出文件已存在，请选择一个新文件名".into());
        }
        final_path.with_file_name(format!(".phirender-{}-{id}.mp4", std::process::id()))
    };
    let settings = &inner.settings;
    let (width, height, fps) = if kind == "preview" || kind == "preview_video" {
        let h = settings.preview_resolution;
        let w = match h {
            480 => 854,
            720 => 1280,
            _ => 1920,
        };
        (w, h, 30)
    } else {
        (settings.width, settings.height, settings.fps)
    };
    let phira_skin = inner.root.join("work/Phira-Pro/assets/respack");
    let use_phira_sounds = settings
        .skin
        .as_ref()
        .is_some_and(|p| Path::new(p) == phira_skin);
    let sound_path = |selected: &Option<String>, name: &str| {
        selected.clone().or_else(|| {
            if !use_phira_sounds {
                return None;
            }
            let p = inner
                .root
                .join("work/Phira-Pro/assets")
                .join(format!("{name}.ogg"));
            p.is_file().then(|| p.to_string_lossy().into_owned())
        })
    };
    let worker_kind = if kind == "preview_video" {
        "export"
    } else {
        kind
    };
    let (start, end) = range.unwrap_or((0., None));
    let using_nvenc = settings.nvenc;
    let job = json!({"kind":worker_kind,"chart":chart.prepared,"output":dest,"project_root":inner.root,"skin":settings.skin,"font":settings.font,
        "width":width,"height":height,"fps":fps,"time":time.max(0.),"start":start,"end":end,"show_hud":settings.show_hud,"include_audio":settings.include_audio,
        "hitsounds":settings.hitsounds,"hitsound_volume":settings.hitsound_volume,"nvenc":settings.nvenc,
        "click_sound":sound_path(&settings.click_sound,"click"),"drag_sound":sound_path(&settings.drag_sound,"drag"),"flick_sound":sound_path(&settings.flick_sound,"flick")});
    let job_path = inner.cache.join(format!("job-{id}.json"));
    std::fs::write(&job_path, serde_json::to_vec(&job).map_err(msg)?).map_err(msg)?;
    let mut cmd = Command::new(worker_path()?);
    cmd.arg(&job_path)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .env("TEMP", &inner.cache)
        .env("TMP", &inner.cache);
    if settings.nvenc && std::env::var_os("PHIRA_FFMPEG").is_none() {
        let compatible_ffmpeg = inner
            .root
            .join("work/ffmpeg-8.0.1-extracted/ffmpeg-8.0.1-essentials_build/bin/ffmpeg.exe");
        if compatible_ffmpeg.is_file() {
            cmd.env("PHIRA_FFMPEG", compatible_ffmpeg);
        }
    }
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000);
    }
    let mut child = cmd
        .spawn()
        .map_err(|e| format!("无法启动 GPU 渲染组件：{e}"))?;
    let stdout = child.stdout.take().ok_or("无法读取渲染任务状态")?;
    let task = TaskView {
        kind: kind.into(),
        status: "loading".into(),
        message: if kind == "audio_export" {
            "正在导出歌曲音频…".into()
        } else {
            "正在加载渲染场景…".into()
        },
        ..Default::default()
    };
    if background {
        inner.preview_stdin = child.stdin.take();
        inner.preview_task = task;
    } else {
        inner.stdin = child.stdin.take();
        inner.task = task;
    }
    drop(inner);
    let kind = kind.to_owned();
    std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(|l| l.ok()) {
            let Ok(v) = serde_json::from_str::<Value>(&line) else {
                continue;
            };
            let mut inner = shared.lock().unwrap();
            match v["type"].as_str().unwrap_or("") {
                "progress" => {
                    let task = if background {
                        &mut inner.preview_task
                    } else {
                        &mut inner.task
                    };
                    task.status = "running".into();
                    task.message = if background {
                        "正在生成 720p 预览视频"
                    } else {
                        "正在导出视频"
                    }
                    .into();
                    task.frame = v["frame"].as_u64().unwrap_or(0);
                    task.total = v["total"].as_u64().unwrap_or(0);
                    task.elapsed = v["elapsed"].as_f64().unwrap_or(0.);
                }
                "preview" => {
                    if let Some(old) = inner.preview_path.replace(dest.clone()) {
                        let _ = std::fs::remove_file(old);
                    }
                    inner.task.status = "ready".into();
                    inner.task.message = "预览已就绪".into();
                    inner.task.frame_ready = true;
                    inner.task.duration = v["duration"].as_f64().unwrap_or(0.);
                    inner.task.time = v["time"].as_f64().unwrap_or(0.);
                }
                "complete" => {
                    if kind == "preview_video" {
                        if let Some(old) = inner.preview_video_path.replace(dest.clone()) {
                            let _ = std::fs::remove_file(old);
                        }
                        inner.preview_task.status = "ready".into();
                        inner.preview_task.message = "预览视频已就绪".into();
                        inner.preview_task.output = Some(dest.to_string_lossy().into_owned());
                        continue;
                    }
                    let result =
                        output
                            .as_ref()
                            .ok_or("缺少输出路径".to_owned())
                            .and_then(|path| {
                                if path.exists() {
                                    return Err("目标文件已存在，成片保留在临时文件中".to_owned());
                                }
                                std::fs::rename(&dest, path).map_err(msg)?;
                                Ok(path.to_string_lossy().into_owned())
                            });
                    match result {
                        Ok(path) => {
                            inner.task.status = "done".into();
                            inner.task.message = "导出完成".into();
                            inner.task.output = Some(path)
                        }
                        Err(e) => {
                            inner.task.status = "error".into();
                            inner.task.message = format!("{e}：{}", dest.display())
                        }
                    }
                }
                "cancelled" => {
                    let task = if background {
                        &mut inner.preview_task
                    } else {
                        &mut inner.task
                    };
                    task.status = "cancelled".into();
                    task.message = "任务已取消".into();
                }
                "error" => {
                    let task = if background {
                        &mut inner.preview_task
                    } else {
                        &mut inner.task
                    };
                    task.status = "error".into();
                    let error = v["message"].as_str().unwrap_or("渲染失败");
                    task.message = if using_nvenc && background || using_nvenc && kind == "export" {
                        format!(
                            "{error}。如果 NVIDIA 编码不可用，请在设置中选择软件 H.264 后重试。"
                        )
                    } else {
                        error.into()
                    };
                }
                _ => {}
            }
        }
        let result = child.wait();
        let _ = std::fs::remove_file(job_path);
        let mut inner = shared.lock().unwrap();
        if background {
            inner.preview_stdin = None;
        } else {
            inner.stdin = None;
        }
        let task = if background {
            &mut inner.preview_task
        } else {
            &mut inner.task
        };
        if !matches!(
            task.status.as_str(),
            "ready" | "done" | "error" | "cancelled"
        ) {
            task.status = "error".into();
            task.message = format!("{}任务意外退出：{:?}", kind, result);
        }
    });
    Ok(())
}
#[tauri::command]
fn start_preview(state: State<'_, AppState>, time: f64) -> Result<(), String> {
    if !time.is_finite() {
        return Err("时间无效".into());
    }
    start_task(state.0.clone(), "preview", time, None, None)
}
#[tauri::command]
fn start_preview_video(state: State<'_, AppState>) -> Result<(), String> {
    start_task(state.0.clone(), "preview_video", 0., None, None)
}

fn output_filename(title: &str, level: &str) -> String {
    fn clean(s: &str) -> String {
        s.trim()
            .chars()
            .map(|c| {
                if c.is_whitespace() || "<>:\"/\\|?*".contains(c) || c.is_control() {
                    '_'
                } else {
                    c
                }
            })
            .collect::<String>()
            .trim_matches([' ', '.', '_'])
            .to_owned()
    }
    let title = clean(title);
    let title = if title.is_empty() {
        "PhiRender"
    } else {
        &title
    };
    let level = level.replace("Lv.", "").replace("lv.", "").replace(' ', "");
    let level = clean(&level);
    let level = if !level.is_empty()
        && level.chars().last().is_some_and(|c| c.is_ascii_digit())
        && !level.contains('.')
    {
        format!("{level}.0")
    } else {
        level
    };
    if level.is_empty() {
        format!("{title}.mp4")
    } else {
        format!("{title}_{level}.mp4")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn suggested_video_filename_uses_song_and_difficulty() {
        assert_eq!(
            output_filename("Desultory Signals", "IN Lv.17"),
            "Desultory_Signals_IN17.0.mp4"
        );
        assert_eq!(output_filename("A/B: C", "EX Lv.15.7"), "A_B__C_EX15.7.mp4");
        assert_eq!(output_filename("Song", ""), "Song.mp4");
    }

    #[test]
    fn old_settings_get_new_preview_default() {
        let settings: Settings = serde_json::from_str(r#"{"nvenc":false}"#).unwrap();
        assert!(settings.auto_preview);
        assert_eq!(settings.settings_version, 0);
    }
}
#[tauri::command]
async fn start_export(
    state: State<'_, AppState>,
    mode: String,
    start: f64,
    end: Option<f64>,
) -> Result<(), String> {
    if !start.is_finite() || start < 0. || end.is_some_and(|v| !v.is_finite() || v <= start) {
        return Err("片段起止时间无效，结束时间必须大于开始时间".into());
    }
    if !["video", "audio"].contains(&mode.as_str()) {
        return Err("不支持的导出类型".into());
    }
    let shared = state.0.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let (filename, kind, filter, extensions) = {
            let inner = shared.lock().unwrap();
            let chart = inner.chart.as_ref().ok_or("请先打开谱面")?;
            if !chart.view.music {
                return Err("谱面没有关联音乐".into());
            }
            if mode == "audio" {
                (
                    output_filename(&chart.view.title, &chart.view.level).replace(".mp4", ".m4a"),
                    "audio_export",
                    "音频文件",
                    vec!["m4a"],
                )
            } else {
                (
                    output_filename(&chart.view.title, &chart.view.level),
                    "export",
                    "MP4 视频",
                    vec!["mp4"],
                )
            }
        };
        let Some(path) = rfd::FileDialog::new()
            .add_filter(filter, &extensions)
            .set_file_name(filename)
            .save_file()
        else {
            return Ok(());
        };
        start_task(shared, kind, 0., Some(path), Some((start, end)))
    })
    .await
    .map_err(msg)?
}
#[tauri::command]
fn cancel_task(state: State<'_, AppState>) -> Result<(), String> {
    let mut inner = state.0.lock().unwrap();
    let stdin = inner.stdin.as_mut().ok_or("没有正在运行的任务")?;
    stdin.write_all(b"cancel\n").map_err(msg)?;
    stdin.flush().map_err(msg)?;
    inner.task.message = "正在取消…".into();
    Ok(())
}
#[tauri::command]
fn cancel_preview_video(state: State<'_, AppState>) -> Result<(), String> {
    let mut inner = state.0.lock().unwrap();
    let stdin = inner
        .preview_stdin
        .as_mut()
        .ok_or("没有正在生成的预览视频")?;
    stdin.write_all(b"cancel\n").map_err(msg)?;
    stdin.flush().map_err(msg)?;
    inner.preview_task.message = "正在取消预览视频…".into();
    Ok(())
}
#[tauri::command]
async fn convert_chart(
    state: State<'_, AppState>,
    archive_mode: bool,
) -> Result<Option<String>, String> {
    let shared = state.0.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let source = {
            let inner = shared.lock().unwrap();
            PathBuf::from(&inner.chart.as_ref().ok_or("请先打开谱面")?.view.source)
        };
        let mode = if archive_mode {
            OutputMode::Archive
        } else {
            OutputMode::Standard
        };
        let ext = if is_archive(&source) { "pez" } else { "json" };
        let Some(output) = rfd::FileDialog::new()
            .add_filter("谱面", &[ext])
            .set_file_name(format!("converted.{ext}"))
            .save_file()
        else {
            return Ok(None);
        };
        if output == source {
            return Err("请选择与输入不同的输出路径".into());
        }
        let input = std::fs::read(source).map_err(msg)?;
        let (data, warnings) = if ext == "pez" {
            let (b, r) = archive::convert_archive_with_mode(&input, mode).map_err(msg)?;
            (b, r.warnings)
        } else {
            let (s, r) = convert_json_with_mode(std::str::from_utf8(&input).map_err(msg)?, mode)
                .map_err(msg)?;
            (
                s.into_bytes(),
                r.diagnostics
                    .iter()
                    .filter(|d| {
                        matches!(
                            d.severity,
                            phigros_rpen_converter::converter::Severity::Warning
                        )
                    })
                    .map(|d| format!("{}: {}", d.path, d.message))
                    .collect(),
            )
        };
        std::fs::write(&output, data).map_err(msg)?;
        Ok(Some(format!(
            "已保存至 {}{}",
            output.display(),
            if warnings.is_empty() {
                String::new()
            } else {
                format!("\n{}", warnings.join("\n"))
            }
        )))
    })
    .await
    .map_err(msg)?
}
fn main() {
    let initial = initial_state();
    if std::env::var_os("WEBVIEW2_USER_DATA_FOLDER").is_none() {
        std::env::set_var("WEBVIEW2_USER_DATA_FOLDER", initial.cache.join("webview2"));
    }
    tauri::Builder::default()
        .manage(AppState(Arc::new(Mutex::new(initial))))
        .invoke_handler(tauri::generate_handler![
            get_settings,
            save_settings,
            open_chart,
            attach_music,
            choose_font,
            choose_sound,
            choose_skin,
            default_skin,
            get_task,
            get_preview_task,
            get_preview_frame,
            get_preview_video,
            start_preview,
            start_preview_video,
            start_export,
            cancel_task,
            cancel_preview_video,
            convert_chart
        ])
        .run(tauri::generate_context!())
        .expect("无法启动 PhiRender 桌面界面");
}
