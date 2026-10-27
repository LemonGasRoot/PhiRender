#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use anyhow::{anyhow, Context, Result};
use macroquad::{
    file::set_pc_assets_folder,
    prelude::*,
    texture::{draw_texture_ex, DrawTextureParams},
};
use phigros_rpen_converter::{
    archive, convert_json_with_mode, parse_document, ChartFormat, OutputMode,
};
use prpr::{
    chart_video::{ChartVideoRenderer, VideoEncoder, VideoOptions},
    core::{BOLD_FONT, PGR_FONT},
    ext::poll_future,
    ui::{FontArc, TextPainter},
};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    collections::BTreeMap,
    future::Future,
    path::{Path, PathBuf},
    pin::Pin,
    sync::mpsc::{self, Receiver, TryRecvError},
    thread,
    time::{Instant, SystemTime, UNIX_EPOCH},
};

type LoadVideo = Pin<Box<dyn Future<Output = Result<ChartVideoRenderer>>>>;
type PreparedResult = std::result::Result<PreparedChart, String>;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
enum ThemeMode {
    #[default]
    System,
    Light,
    Dark,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Page {
    Work,
    Convert,
    Skins,
    Export,
    Settings,
    About,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(default)]
struct Settings {
    theme: ThemeMode,
    archive_mode: bool,
    resolution: usize,
    fps: usize,
    use_nvenc: bool,
    show_hud: bool,
    show_accuracy: bool,
    hitsounds: bool,
    volume: f32,
    skin_path: Option<PathBuf>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            theme: ThemeMode::System,
            archive_mode: false,
            resolution: 1,
            fps: 1,
            use_nvenc: false,
            show_hud: true,
            show_accuracy: false,
            hitsounds: true,
            volume: 0.8,
            skin_path: std::env::var_os("PHIGROS_SKIN_PATH").map(PathBuf::from),
        }
    }
}

#[derive(Clone, Copy)]
struct Palette {
    bg: Color,
    surface: Color,
    surface2: Color,
    surface3: Color,
    text: Color,
    secondary: Color,
    muted: Color,
    border: Color,
    accent: Color,
    accent_hover: Color,
    selection: Color,
    hover: Color,
    success: Color,
    warning: Color,
    danger: Color,
    canvas: Color,
}

impl Palette {
    fn light() -> Self {
        Self {
            bg: hex(0xF5F5F7),
            surface: hex(0xFFFFFF),
            surface2: hex(0xF0F1F3),
            surface3: hex(0xE8EAED),
            text: hex(0x1D1D1F),
            secondary: hex(0x6E6E73),
            muted: hex(0x8A8A8E),
            border: hex(0xD9DADD),
            accent: hex(0x0A84FF),
            accent_hover: hex(0x0077ED),
            selection: hex(0xE5EFFD),
            hover: hex(0xECEEF1),
            success: hex(0x16835D),
            warning: hex(0xB7791F),
            danger: hex(0xD92D20),
            canvas: hex(0x101114),
        }
    }

    fn dark() -> Self {
        Self {
            bg: hex(0x17181A),
            surface: hex(0x202124),
            surface2: hex(0x26282B),
            surface3: hex(0x303236),
            text: hex(0xF5F5F7),
            secondary: hex(0xA1A1AA),
            muted: hex(0x777B82),
            border: hex(0x383A40),
            accent: hex(0x0A84FF),
            accent_hover: hex(0x3698FF),
            selection: hex(0x24364B),
            hover: hex(0x2B2D31),
            success: hex(0x35C98A),
            warning: hex(0xF4B740),
            danger: hex(0xFF5A5F),
            canvas: hex(0x101114),
        }
    }
}

#[derive(Clone)]
struct ChartStats {
    format: Option<ChartFormat>,
    notes: usize,
    lines: usize,
    noise: usize,
}

impl Default for ChartStats {
    fn default() -> Self {
        Self {
            format: None,
            notes: 0,
            lines: 0,
            noise: 0,
        }
    }
}

struct PreparedChart {
    chart_path: PathBuf,
    temp_path: Option<PathBuf>,
    stats: ChartStats,
    warnings: Vec<String>,
}

impl Drop for PreparedChart {
    fn drop(&mut self) {
        if let Some(path) = self.temp_path.take() {
            let _ = std::fs::remove_file(path);
        }
    }
}

enum PreviewLoad {
    Idle,
    Preparing(Receiver<PreparedResult>),
    Loading(LoadVideo, PreparedChart),
}

enum ExportJob {
    Idle,
    Preparing {
        receiver: Receiver<PreparedResult>,
        output: PathBuf,
        options: VideoOptions,
    },
    Loading(LoadVideo, PreparedChart),
    Rendering(ChartVideoRenderer, PreparedChart, Instant),
    Done,
    Error(String),
}

struct App {
    page: Page,
    settings: Settings,
    settings_file: PathBuf,
    input_path: Option<PathBuf>,
    music_path: Option<PathBuf>,
    preview: Option<ChartVideoRenderer>,
    preview_prepared: Option<PreparedChart>,
    preview_load: PreviewLoad,
    export: ExportJob,
    conversion: Option<Receiver<std::result::Result<String, String>>>,
    stats: ChartStats,
    status: String,
    song_title: String,
    song_level: String,
    last_preview_time: f64,
}

impl App {
    fn new() -> Self {
        let settings_file = settings_path();
        let mut settings = load_settings(&settings_file);
        if settings
            .skin_path
            .as_ref()
            .is_some_and(|path| !path.exists())
        {
            settings.skin_path = None;
        }
        Self {
            page: Page::Work,
            settings,
            settings_file,
            input_path: None,
            music_path: None,
            preview: None,
            preview_prepared: None,
            preview_load: PreviewLoad::Idle,
            export: ExportJob::Idle,
            conversion: None,
            stats: ChartStats::default(),
            status: "打开谱面以开始预览".to_owned(),
            song_title: "尚未加载谱面".to_owned(),
            song_level: "—".to_owned(),
            last_preview_time: -1.,
        }
    }

    fn save_settings(&self) {
        if let Some(parent) = self.settings_file.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        if let Ok(bytes) = serde_json::to_vec_pretty(&self.settings) {
            let _ = std::fs::write(&self.settings_file, bytes);
        }
    }

    fn select_chart(&mut self, path: PathBuf) {
        self.preview.take();
        self.preview_prepared.take();
        self.preview_load = PreviewLoad::Idle;
        self.input_path = Some(path.clone());
        self.stats = ChartStats::default();
        self.song_title = path
            .file_stem()
            .and_then(|it| it.to_str())
            .unwrap_or("谱面")
            .to_owned();
        self.song_level.clear();
        let mode = output_mode(&self.settings);
        let music = self.music_path.clone();
        self.preview_load = PreviewLoad::Preparing(prepare_channel(path, mode, music));
        self.status = "正在准备谱面…".to_owned();
    }

    fn start_preview_reload(&mut self) {
        if let Some(path) = self.input_path.clone() {
            self.select_chart(path);
        }
    }
}

fn window_conf() -> Conf {
    Conf {
        window_title: "PhiRender".into(),
        window_width: 1440,
        window_height: 900,
        window_resizable: true,
        ..Default::default()
    }
}

fn hex(rgb: u32) -> Color {
    Color::from_rgba((rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8, 255)
}

fn system_is_light() -> bool {
    #[cfg(windows)]
    {
        let output = std::process::Command::new("reg.exe")
            .args([
                "query",
                r"HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize",
                "/v",
                "AppsUseLightTheme",
            ])
            .output();
        return output.is_ok_and(|result| {
            String::from_utf8_lossy(&result.stdout)
                .to_ascii_lowercase()
                .contains("0x1")
        });
    }
    #[cfg(not(windows))]
    false
}

fn palette(mode: ThemeMode, system_light: bool) -> Palette {
    let light = match mode {
        ThemeMode::Light => true,
        ThemeMode::Dark => false,
        ThemeMode::System => system_light,
    };
    if light {
        Palette::light()
    } else {
        Palette::dark()
    }
}

fn settings_path() -> PathBuf {
    if let Some(appdata) = std::env::var_os("APPDATA") {
        PathBuf::from(appdata)
            .join("PhiRender")
            .join("settings.json")
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join("PhiRender-settings.json")
    }
}

fn load_settings(path: &Path) -> Settings {
    std::fs::read(path)
        .ok()
        .and_then(|bytes| serde_json::from_slice(&bytes).ok())
        .unwrap_or_default()
}

fn output_mode(settings: &Settings) -> OutputMode {
    if settings.archive_mode {
        OutputMode::Archive
    } else {
        OutputMode::Standard
    }
}

fn is_archive(path: &Path) -> bool {
    path.extension()
        .and_then(|it| it.to_str())
        .is_some_and(|it| it.eq_ignore_ascii_case("pez") || it.eq_ignore_ascii_case("zip"))
}

fn is_json(path: &Path) -> bool {
    path.extension()
        .and_then(|it| it.to_str())
        .is_some_and(|it| it.eq_ignore_ascii_case("json"))
}

fn format_label(format: ChartFormat) -> &'static str {
    match format {
        ChartFormat::OfficialV3 => "官方 Phigros v3",
        ChartFormat::Rpe => "普通 RPE",
        ChartFormat::Rpen => "RPEN",
    }
}

fn stats_from_json(text: &str, format: ChartFormat) -> Result<ChartStats> {
    let value: Value = serde_json::from_str(text)?;
    let lines = value["judgeLineList"].as_array();
    let line_count = lines.map_or(0, Vec::len);
    let notes = lines.map_or(0, |lines| {
        lines
            .iter()
            .map(|line| match format {
                ChartFormat::OfficialV3 => ["notesAbove", "notesBelow"]
                    .iter()
                    .map(|key| line[*key].as_array().map_or(0, Vec::len))
                    .sum(),
                ChartFormat::Rpe | ChartFormat::Rpen => {
                    line["notes"].as_array().map_or(0, Vec::len)
                }
            })
            .sum()
    });
    Ok(ChartStats {
        format: Some(format),
        notes,
        lines: line_count,
        noise: value["blockAreaList"].as_array().map_or(0, Vec::len),
    })
}

fn temp_path_near(input: &Path) -> PathBuf {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|it| it.as_nanos())
        .unwrap_or_default();
    let parent = input.parent().filter(|it| !it.as_os_str().is_empty());
    let dir = parent.unwrap_or_else(|| Path::new("."));
    dir.join(format!(".phirender-{}-{stamp}.pez", std::process::id()))
}

fn write_temp_chart(input: &Path, bytes: &[u8]) -> Result<PathBuf> {
    let primary = temp_path_near(input);
    match std::fs::write(&primary, bytes) {
        Ok(()) => Ok(primary),
        Err(first) => {
            let fallback = std::env::temp_dir().join(format!(
                "phirender-{}-{}.pez",
                std::process::id(),
                SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos()
            ));
            std::fs::write(&fallback, bytes).with_context(|| {
                format!(
                    "Cannot write temporary chart beside input ({first}) or to {}",
                    fallback.display()
                )
            })?;
            Ok(fallback)
        }
    }
}

fn prepare_chart(path: &Path, mode: OutputMode, music: Option<&Path>) -> Result<PreparedChart> {
    if is_archive(path) {
        let bytes =
            std::fs::read(path).with_context(|| format!("Cannot read {}", path.display()))?;
        let (converted, report) = archive::convert_archive_with_mode(&bytes, mode)?;
        let temp_path = write_temp_chart(path, &converted)?;
        return Ok(PreparedChart {
            chart_path: temp_path.clone(),
            temp_path: Some(temp_path),
            stats: ChartStats {
                format: Some(report.format),
                notes: report.notes,
                lines: report.judge_lines,
                noise: report.noise_areas,
            },
            warnings: report.warnings,
        });
    }
    if !is_json(path) {
        return Err(anyhow!("请选择 JSON、PEZ 或 ZIP 谱面"));
    }
    let source =
        std::fs::read_to_string(path).with_context(|| format!("Cannot read {}", path.display()))?;
    let parsed = parse_document(&source)?;
    let (converted, report) = convert_json_with_mode(&source, mode)?;
    let output_format = phigros_rpen_converter::detect_format(&serde_json::from_str(&converted)?)?;
    let stats = stats_from_json(&converted, output_format)?;
    let stem = path
        .file_stem()
        .and_then(|it| it.to_str())
        .unwrap_or("chart");
    let chart_name = format!("{stem}.json");
    let mut files = BTreeMap::new();
    files.insert(chart_name.clone(), converted.into_bytes());
    let (song_name, song_bytes) = if let Some(music) = music {
        let name = format!(
            "music.{}",
            music
                .extension()
                .and_then(|it| it.to_str())
                .unwrap_or("ogg")
        );
        (
            name,
            Some(std::fs::read(music).with_context(|| format!("Cannot read {}", music.display()))?),
        )
    } else {
        (String::new(), None)
    };
    if let Some(bytes) = song_bytes {
        files.insert(song_name.clone(), bytes);
    }
    let info = format!(
        "#\nName: {stem}\nPath: 00000000\nSong: {song_name}\nPicture: \nChart: {chart_name}\nLevel: \nComposer: \nCharter: \nIllustrator: \n"
    );
    files.insert("info.txt".to_owned(), info.into_bytes());
    let packed = archive::write_zip(&files)?;
    let temp_path = write_temp_chart(path, &packed)?;
    Ok(PreparedChart {
        chart_path: temp_path.clone(),
        temp_path: Some(temp_path),
        stats,
        warnings: report
            .diagnostics
            .iter()
            .filter(|it| {
                matches!(
                    it.severity,
                    phigros_rpen_converter::converter::Severity::Warning
                )
            })
            .map(|it| format!("{}: {}", it.path, it.message))
            .collect(),
    })
}

fn prepare_channel(
    path: PathBuf,
    mode: OutputMode,
    music: Option<PathBuf>,
) -> Receiver<PreparedResult> {
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        let result = prepare_chart(&path, mode, music.as_deref()).map_err(|it| format!("{it:#}"));
        let _ = tx.send(result);
    });
    rx
}

fn conversion_channel(
    input: PathBuf,
    output: PathBuf,
    mode: OutputMode,
    music: Option<PathBuf>,
) -> Receiver<std::result::Result<String, String>> {
    let (tx, rx) = mpsc::channel();
    thread::spawn(move || {
        let result =
            convert_input(&input, &output, mode, music.as_deref()).map_err(|it| format!("{it:#}"));
        let _ = tx.send(result);
    });
    rx
}

fn convert_input(
    input_path: &Path,
    output_path: &Path,
    mode: OutputMode,
    music: Option<&Path>,
) -> Result<String> {
    let bytes = std::fs::read(input_path)
        .with_context(|| format!("Cannot read {}", input_path.display()))?;
    if is_archive(input_path) {
        if !is_archive(output_path) {
            return Err(anyhow!("PEZ/ZIP 输入需要保存为 .pez 或 .zip"));
        }
        let (converted, report) = archive::convert_archive_with_mode(&bytes, mode)?;
        std::fs::write(output_path, converted)
            .with_context(|| format!("Cannot write {}", output_path.display()))?;
        return Ok(format!(
            "{} · {} Notes · {} 判定线 · {} 噪域",
            format_label(report.format),
            report.notes,
            report.judge_lines,
            report.noise_areas
        ));
    }
    let source = String::from_utf8(bytes).context("谱面 JSON 不是 UTF-8")?;
    let parsed = parse_document(&source)?;
    let (converted, report) = convert_json_with_mode(&source, mode)?;
    if is_archive(output_path) {
        let stem = input_path
            .file_stem()
            .and_then(|it| it.to_str())
            .unwrap_or("chart");
        let chart_name = format!("{stem}.json");
        let (song_name, song_bytes) = if let Some(music) = music {
            let name = format!(
                "music.{}",
                music
                    .extension()
                    .and_then(|it| it.to_str())
                    .unwrap_or("ogg")
            );
            (
                name,
                Some(
                    std::fs::read(music)
                        .with_context(|| format!("Cannot read {}", music.display()))?,
                ),
            )
        } else {
            (String::new(), None)
        };
        let mut files = BTreeMap::new();
        files.insert(chart_name.clone(), converted.into_bytes());
        files.insert(
            "info.txt".to_owned(),
            format!(
                "#\nName: {stem}\nPath: 00000000\nSong: {song_name}\nPicture: \nChart: {chart_name}\nLevel: \nComposer: \nCharter: \nIllustrator: \n"
            )
            .into_bytes(),
        );
        if let Some(song) = song_bytes {
            files.insert(song_name, song);
        }
        std::fs::write(output_path, archive::write_zip(&files)?)
            .with_context(|| format!("Cannot write {}", output_path.display()))?;
    } else {
        std::fs::write(output_path, converted)
            .with_context(|| format!("Cannot write {}", output_path.display()))?;
    }
    Ok(format!(
        "{} · {} Notes · {} 判定线 · {} 噪域",
        format_label(parsed.format),
        report.notes,
        report.judge_lines,
        report.noise_areas
    ))
}

fn default_conversion_name(path: &Path) -> String {
    let stem = path
        .file_stem()
        .and_then(|it| it.to_str())
        .unwrap_or("chart");
    if is_archive(path) {
        format!("{stem}-RPEN.pez")
    } else {
        format!("{stem}-RPEN.json")
    }
}

fn draw_text_ui(font: &Font, text: &str, x: f32, y: f32, size: u16, color: Color) {
    draw_text_ex(
        text,
        x,
        y,
        TextParams {
            font: *font,
            font_size: size,
            font_scale: 1.,
            font_scale_aspect: 1.,
            color,
            ..Default::default()
        },
    );
}

fn fit_text(font: &Font, text: &str, x: f32, y: f32, max_width: f32, size: u16, color: Color) {
    let mut draw_size = size;
    while draw_size > 10 && measure_text(text, Some(*font), draw_size, 1.).width > max_width {
        draw_size -= 1;
    }
    draw_text_ui(font, text, x, y, draw_size, color);
}

fn panel(rect: Rect, p: Palette) {
    draw_rectangle(rect.x, rect.y, rect.w, rect.h, p.surface);
    draw_rectangle_lines(rect.x, rect.y, rect.w, rect.h, 1., p.border);
}

fn button(font: &Font, p: Palette, rect: Rect, label: &str, primary: bool, selected: bool) -> bool {
    let (mx, my) = mouse_position();
    let hover = mx >= rect.x && mx <= rect.x + rect.w && my >= rect.y && my <= rect.y + rect.h;
    let fill = if primary {
        if hover {
            p.accent_hover
        } else {
            p.accent
        }
    } else if selected {
        p.selection
    } else if hover {
        p.hover
    } else {
        p.surface2
    };
    draw_rectangle(rect.x, rect.y, rect.w, rect.h, fill);
    if !primary {
        draw_rectangle_lines(rect.x, rect.y, rect.w, rect.h, 1., p.border);
    }
    let color = if primary { WHITE } else { p.text };
    let measured = measure_text(label, Some(*font), 14, 1.).width;
    draw_text_ui(
        font,
        label,
        rect.x + (rect.w - measured).max(16.) / 2.,
        rect.y + rect.h / 2. + 5.,
        14,
        color,
    );
    hover && is_mouse_button_pressed(MouseButton::Left)
}

fn label(font: &Font, p: Palette, text: &str, x: f32, y: f32, size: u16) {
    draw_text_ui(font, text, x, y, size, p.text);
}

fn sublabel(font: &Font, p: Palette, text: &str, x: f32, y: f32, size: u16) {
    draw_text_ui(font, text, x, y, size, p.secondary);
}

fn checkbox(font: &Font, p: Palette, x: f32, y: f32, text: &str, checked: bool) -> bool {
    let r = Rect::new(x, y - 16., 18., 18.);
    draw_rectangle(
        r.x,
        r.y,
        r.w,
        r.h,
        if checked { p.accent } else { p.surface2 },
    );
    draw_rectangle_lines(r.x, r.y, r.w, r.h, 1., p.border);
    if checked {
        draw_line(r.x + 4., r.y + 9., r.x + 8., r.y + 13., 2., WHITE);
        draw_line(r.x + 8., r.y + 13., r.x + 15., r.y + 4., 2., WHITE);
    }
    label(font, p, text, x + 28., y, 14);
    let (mx, my) = mouse_position();
    mx >= x
        && mx <= x + 260.
        && my >= r.y
        && my <= r.y + 22.
        && is_mouse_button_pressed(MouseButton::Left)
}

fn draw_texture_fit(texture: Texture2D, rect: Rect) {
    let ratio = texture.width() / texture.height().max(1.);
    let mut w = rect.w;
    let mut h = w / ratio;
    if h > rect.h {
        h = rect.h;
        w = h * ratio;
    }
    draw_texture_ex(
        texture,
        rect.x + (rect.w - w) / 2.,
        rect.y + (rect.h - h) / 2.,
        WHITE,
        DrawTextureParams {
            dest_size: Some(vec2(w, h)),
            flip_y: true,
            ..Default::default()
        },
    );
}

fn save_file_dialog(path: &Path, extension: &str) -> Option<PathBuf> {
    rfd::FileDialog::new()
        .add_filter(extension, &[extension])
        .set_directory(path.parent().unwrap_or_else(|| Path::new(".")))
        .set_file_name(
            path.file_name()
                .and_then(|it| it.to_str())
                .unwrap_or("output"),
        )
        .save_file()
}

fn start_preview(app: &mut App) {
    if let Some(path) = app.input_path.clone() {
        app.preview.take();
        app.preview_prepared.take();
        app.preview_load = PreviewLoad::Preparing(prepare_channel(
            path,
            output_mode(&app.settings),
            app.music_path.clone(),
        ));
        app.status = "正在准备 GPU 预览…".to_owned();
    }
}

fn update_preview(app: &mut App, painter: &mut TextPainter) {
    let state = std::mem::replace(&mut app.preview_load, PreviewLoad::Idle);
    match state {
        PreviewLoad::Idle => {}
        PreviewLoad::Preparing(receiver) => match receiver.try_recv() {
            Ok(Ok(prepared)) => {
                app.stats = prepared.stats.clone();
                if let Some(warning) = prepared.warnings.first() {
                    app.status = format!("转换提示：{warning}");
                }
                let mut options = VideoOptions::default();
                options.width = 1280;
                options.height = 720;
                options.sample_count = 1;
                options.include_audio = false;
                options.show_hud = app.settings.show_hud;
                options.show_accuracy = app.settings.show_accuracy;
                options.show_offset_indicator = false;
                options.res_pack_path = app.settings.skin_path.clone();
                app.preview_load = PreviewLoad::Loading(
                    Box::pin(ChartVideoRenderer::new_preview(
                        prepared.chart_path.clone(),
                        options,
                    )),
                    prepared,
                );
                app.status = "正在创建渲染预览…".to_owned();
            }
            Ok(Err(error)) => app.status = format!("预览准备失败：{error}"),
            Err(TryRecvError::Empty) => app.preview_load = PreviewLoad::Preparing(receiver),
            Err(TryRecvError::Disconnected) => app.status = "预览准备任务已中断".to_owned(),
        },
        PreviewLoad::Loading(mut future, prepared) => {
            if let Some(result) = poll_future(future.as_mut()) {
                match result {
                    Ok(mut renderer) => {
                        let _ = renderer.preview_pause();
                        let _ = renderer.preview_set_volume(app.settings.volume);
                        let _ = renderer.render_preview_at(painter, 0.);
                        app.song_title = renderer.title().to_owned();
                        app.song_level = renderer.level().to_owned();
                        app.status = format!("预览已就绪 · {:.1} 秒", renderer.track_length());
                        app.preview = Some(renderer);
                        app.preview_prepared = Some(prepared);
                        app.last_preview_time = -1.;
                    }
                    Err(error) => {
                        app.status = format!("无法加载预览：{error:#}");
                    }
                }
            } else {
                app.preview_load = PreviewLoad::Loading(future, prepared);
            }
        }
    }
}

fn update_export(app: &mut App, painter: &mut TextPainter) {
    let state = std::mem::replace(&mut app.export, ExportJob::Idle);
    match state {
        ExportJob::Idle => {}
        ExportJob::Done => app.export = ExportJob::Done,
        ExportJob::Error(message) => app.export = ExportJob::Error(message),
        ExportJob::Preparing {
            receiver,
            output,
            options,
        } => match receiver.try_recv() {
            Ok(Ok(prepared)) => {
                let future = Box::pin(ChartVideoRenderer::new(
                    prepared.chart_path.clone(),
                    options,
                    output,
                ));
                app.export = ExportJob::Loading(future, prepared);
                app.status = "正在载入导出场景…".to_owned();
            }
            Ok(Err(error)) => {
                app.status = format!("导出准备失败：{error}");
                app.export = ExportJob::Error(error);
            }
            Err(TryRecvError::Empty) => {
                app.export = ExportJob::Preparing {
                    receiver,
                    output,
                    options,
                };
            }
            Err(TryRecvError::Disconnected) => {
                app.export = ExportJob::Error("导出准备任务已中断".to_owned());
            }
        },
        ExportJob::Loading(mut future, prepared) => {
            if let Some(result) = poll_future(future.as_mut()) {
                match result {
                    Ok(renderer) => {
                        app.status = "正在导出视频…".to_owned();
                        app.export = ExportJob::Rendering(renderer, prepared, Instant::now());
                    }
                    Err(error) => {
                        app.status = format!("导出场景载入失败：{error:#}");
                        app.export = ExportJob::Error(format!("{error:#}"));
                    }
                }
            } else {
                app.export = ExportJob::Loading(future, prepared);
            }
        }
        ExportJob::Rendering(mut renderer, prepared, started) => {
            let mut failure = None;
            for _ in 0..2 {
                if renderer.finished() {
                    break;
                }
                if let Err(error) = renderer.render_next_frame(painter) {
                    failure = Some(format!("{error:#}"));
                    break;
                }
            }
            if let Some(error) = failure {
                renderer.cancel();
                app.status = format!("渲染失败：{error}");
                app.export = ExportJob::Error(error);
            } else if renderer.finished() {
                let output = match renderer.finish() {
                    Ok(path) => path,
                    Err(error) => {
                        app.status = format!("视频编码失败：{error:#}");
                        app.export = ExportJob::Error(format!("{error:#}"));
                        return;
                    }
                };
                app.status = format!("导出完成：{}", output.display());
                app.export = ExportJob::Done;
                drop(prepared);
            } else {
                app.export = ExportJob::Rendering(renderer, prepared, started);
            }
        }
    }
}

fn update_conversion(app: &mut App) {
    if let Some(receiver) = app.conversion.take() {
        match receiver.try_recv() {
            Ok(Ok(summary)) => app.status = format!("转换完成 · {summary}"),
            Ok(Err(error)) => app.status = format!("转换失败 · {error}"),
            Err(TryRecvError::Empty) => app.conversion = Some(receiver),
            Err(TryRecvError::Disconnected) => app.status = "转换任务已中断".to_owned(),
        }
    }
}

fn start_export(app: &mut App) {
    let Some(input) = app.input_path.clone() else {
        app.status = "请先打开一份谱面".to_owned();
        return;
    };
    let default_name = format!(
        "{}-render.mp4",
        input
            .file_stem()
            .and_then(|it| it.to_str())
            .unwrap_or("chart")
    );
    let Some(output) = rfd::FileDialog::new()
        .add_filter("MP4 视频", &["mp4"])
        .set_file_name(default_name)
        .save_file()
    else {
        return;
    };
    let resolutions = [(1280, 720), (1920, 1080), (2560, 1440), (3840, 2160)];
    let fps = [30, 60, 120];
    let (width, height) = resolutions[app.settings.resolution.min(resolutions.len() - 1)];
    let mut options = VideoOptions::default();
    options.width = width;
    options.height = height;
    options.fps = fps[app.settings.fps.min(fps.len() - 1)];
    options.show_hud = app.settings.show_hud;
    options.show_accuracy = app.settings.show_accuracy;
    options.show_offset_indicator = false;
    options.hitsound = app.settings.hitsounds;
    options.include_audio = true;
    options.sample_count = 1;
    options.encoder = if app.settings.use_nvenc {
        VideoEncoder::H264Nvenc
    } else {
        VideoEncoder::H264Software
    };
    options.res_pack_path = app.settings.skin_path.clone();
    let receiver = prepare_channel(input, output_mode(&app.settings), app.music_path.clone());
    app.export = ExportJob::Preparing {
        receiver,
        output,
        options,
    };
    app.status = "正在准备导出…".to_owned();
}

fn draw_sidebar(app: &mut App, font: &Font, p: Palette, width: f32) {
    draw_rectangle(0., 52., width, screen_height() - 52., p.surface);
    draw_line(width, 52., width, screen_height(), 1., p.border);
    draw_circle(28., 30., 12., p.accent);
    draw_text_ui(font, "φ", 20., 35., 19, WHITE);
    draw_text_ui(font, "PhiRender", 49., 35., 17, p.text);
    let entries = [
        (Page::Work, "工作台"),
        (Page::Convert, "谱面转换"),
        (Page::Skins, "皮肤"),
        (Page::Export, "视频导出"),
        (Page::Settings, "设置"),
        (Page::About, "关于"),
    ];
    for (index, (page, title)) in entries.into_iter().enumerate() {
        let y = 82. + index as f32 * 44.;
        let rect = Rect::new(12., y, width - 24., 36.);
        if button(font, p, rect, title, false, app.page == page) {
            app.page = page;
        }
    }
    if width > 120. {
        sublabel(font, p, "GPU 谱面播放器", 18., screen_height() - 28., 12);
    }
}

fn draw_topbar(app: &mut App, font: &Font, p: Palette) {
    draw_rectangle(0., 0., screen_width(), 52., p.surface);
    draw_line(0., 51., screen_width(), 51., 1., p.border);
    let title = match app.page {
        Page::Work => "工作台",
        Page::Convert => "谱面转换",
        Page::Skins => "皮肤",
        Page::Export => "视频导出",
        Page::Settings => "设置",
        Page::About => "关于 PhiRender",
    };
    label(font, p, title, 204., 32., 17);
    let w = screen_width();
    if button(
        font,
        p,
        Rect::new(w - 400., 10., 96., 32.),
        "打开谱面",
        false,
        false,
    ) {
        if let Some(path) = rfd::FileDialog::new()
            .add_filter("Phigros 谱面", &["json", "pez", "zip"])
            .pick_file()
        {
            app.select_chart(path);
        }
    }
    if button(
        font,
        p,
        Rect::new(w - 296., 10., 96., 32.),
        "转换",
        false,
        false,
    ) {
        app.page = Page::Convert;
    }
    if button(
        font,
        p,
        Rect::new(w - 192., 10., 96., 32.),
        "皮肤",
        false,
        false,
    ) {
        app.page = Page::Skins;
    }
    if button(
        font,
        p,
        Rect::new(w - 88., 10., 78., 32.),
        "渲染",
        true,
        false,
    ) {
        app.page = Page::Export;
    }
}

fn draw_info_panel(app: &App, font: &Font, p: Palette, rect: Rect) {
    panel(rect, p);
    label(font, p, "谱面信息", rect.x + 16., rect.y + 27., 15);
    fit_text(
        font,
        &app.song_title,
        rect.x + 16.,
        rect.y + 62.,
        rect.w - 32.,
        19,
        p.text,
    );
    sublabel(font, p, &app.song_level, rect.x + 16., rect.y + 87., 13);
    let format = app.stats.format.map(format_label).unwrap_or("未识别");
    sublabel(font, p, format, rect.x + 16., rect.y + 117., 13);
    draw_line(
        rect.x + 16.,
        rect.y + 134.,
        rect.x + rect.w - 16.,
        rect.y + 134.,
        1.,
        p.border,
    );
    let rows = [
        ("Note", app.stats.notes.to_string()),
        ("判定线", app.stats.lines.to_string()),
        ("噪域", app.stats.noise.to_string()),
        (
            "时长",
            app.preview
                .as_ref()
                .map(|it| format!("{:.1} 秒", it.track_length()))
                .unwrap_or_else(|| "—".to_owned()),
        ),
        (
            "音乐",
            if app.preview.as_ref().is_some_and(|it| it.has_music()) || app.music_path.is_some() {
                "已关联".to_owned()
            } else {
                "未检测到".to_owned()
            },
        ),
        (
            "皮肤",
            app.settings
                .skin_path
                .as_ref()
                .and_then(|it| it.file_stem())
                .and_then(|it| it.to_str())
                .unwrap_or("默认")
                .to_owned(),
        ),
    ];
    for (i, (key, value)) in rows.iter().enumerate() {
        let y = rect.y + 160. + i as f32 * 30.;
        sublabel(font, p, key, rect.x + 16., y, 13);
        let tw = measure_text(value, Some(*font), 13, 1.).width;
        draw_text_ui(font, value, rect.x + rect.w - 16. - tw, y, 13, p.text);
    }
    let button_y = rect.y + rect.h - 92.;
    if button(
        font,
        p,
        Rect::new(rect.x + 16., button_y, rect.w - 32., 34.),
        "关联音乐文件",
        false,
        false,
    ) {
        if let Some(path) = rfd::FileDialog::new()
            .add_filter("音频", &["ogg", "mp3", "wav", "flac", "m4a"])
            .pick_file()
        {
            app_music_set(path);
        }
    }
    if button(
        font,
        p,
        Rect::new(rect.x + 16., button_y + 42., rect.w - 32., 34.),
        "导出视频",
        true,
        false,
    ) {
        // The page action is handled in the main render function.
        REQUEST_EXPORT.with(|slot| slot.set(true));
    }
}

thread_local! {
    static REQUEST_EXPORT: std::cell::Cell<bool> = const { std::cell::Cell::new(false) };
}

fn app_music_set(path: PathBuf) {
    APP_ACTION.with(|slot| *slot.borrow_mut() = Some(UiAction::Music(path)));
}

enum UiAction {
    Music(PathBuf),
}

thread_local! {
    static APP_ACTION: std::cell::RefCell<Option<UiAction>> = const { std::cell::RefCell::new(None) };
}

fn draw_work_page(app: &mut App, font: &Font, p: Palette, layout: Layout) {
    let main = layout.content;
    label(font, p, "实时预览", main.x, main.y + 20., 15);
    let canvas = Rect::new(main.x, main.y + 34., main.w, main.h - 112.);
    draw_rectangle(canvas.x, canvas.y, canvas.w, canvas.h, p.canvas);
    if let Some(texture) = match &app.export {
        ExportJob::Rendering(renderer, _, _) => Some(renderer.texture()),
        _ => app.preview.as_ref().map(ChartVideoRenderer::texture),
    } {
        draw_texture_fit(
            texture,
            Rect::new(canvas.x + 4., canvas.y + 4., canvas.w - 8., canvas.h - 8.),
        );
    } else {
        let message = match &app.preview_load {
            PreviewLoad::Preparing(_) => "正在准备谱面…",
            PreviewLoad::Loading(_, _) => "正在创建 GPU 预览…",
            PreviewLoad::Idle => "打开 JSON 或 PEZ 谱面开始预览",
        };
        let tw = measure_text(message, Some(*font), 17, 1.).width;
        draw_text_ui(
            font,
            message,
            canvas.x + (canvas.w - tw) / 2.,
            canvas.y + canvas.h / 2.,
            17,
            p.secondary,
        );
    }
    draw_rectangle_lines(canvas.x, canvas.y, canvas.w, canvas.h, 1., p.border);
    let transport_y = canvas.y + canvas.h + 14.;
    let play = Rect::new(main.x, transport_y, 72., 34.);
    if button(
        font,
        p,
        play,
        if app.preview.as_mut().is_some_and(|it| it.preview_paused()) {
            "播放"
        } else {
            "暂停"
        },
        true,
        false,
    ) {
        if let Some(preview) = app.preview.as_mut() {
            if preview.preview_paused() {
                let _ = preview.preview_play();
            } else {
                let _ = preview.preview_pause();
            }
        }
    }
    let current = app.preview.as_ref().map_or(0., |it| it.preview_time());
    let duration = app
        .preview
        .as_ref()
        .map_or(1., |it| it.track_length().max(1.));
    let time_text = format!("{} / {}", time_string(current), time_string(duration));
    sublabel(font, p, &time_text, main.x + 86., transport_y + 23., 12);
    let timeline = Rect::new(
        main.x + 230.,
        transport_y + 9.,
        (main.w - 420.).max(140.),
        12.,
    );
    draw_rectangle(timeline.x, timeline.y, timeline.w, timeline.h, p.surface3);
    let progress = (current / duration).clamp(0., 1.) as f32;
    draw_rectangle(
        timeline.x,
        timeline.y,
        timeline.w * progress,
        timeline.h,
        p.accent,
    );
    let (mx, my) = mouse_position();
    if is_mouse_button_down(MouseButton::Left)
        && mx >= timeline.x
        && mx <= timeline.x + timeline.w
        && my >= timeline.y - 6.
        && my <= timeline.y + timeline.h + 6.
    {
        if let Some(preview) = app.preview.as_mut() {
            let _ = preview.preview_seek(((mx - timeline.x) / timeline.w) as f64 * duration);
        }
    }
    if checkbox(
        font,
        p,
        main.x + main.w - 174.,
        transport_y + 22.,
        "HUD",
        app.settings.show_hud,
    ) {
        app.settings.show_hud = !app.settings.show_hud;
        app.save_settings();
        if let Some(preview) = app.preview.as_mut() {
            let _ = preview.set_preview_hud(app.settings.show_hud);
        }
    }
    if layout.inspector.w > 0. {
        draw_info_panel(app, font, p, layout.inspector);
    }
}

fn time_string(time: f64) -> String {
    let seconds = time.max(0.) as u64;
    format!("{:02}:{:02}", seconds / 60, seconds % 60)
}

#[derive(Clone, Copy)]
struct Layout {
    sidebar: f32,
    content: Rect,
    inspector: Rect,
}

fn layout() -> Layout {
    let width = screen_width();
    let height = screen_height();
    let sidebar = if width < 1120. { 76. } else { 176. };
    let inspector_width = if width < 1220. { 0. } else { 280. };
    let gap = 18.;
    let x = sidebar + gap;
    let right = width - inspector_width - if inspector_width > 0. { gap } else { 18. };
    Layout {
        sidebar,
        content: Rect::new(x, 74., (right - x).max(300.), height - 150.),
        inspector: if inspector_width > 0. {
            Rect::new(
                width - inspector_width - 18.,
                74.,
                inspector_width,
                height - 150.,
            )
        } else {
            Rect::new(0., 0., 0., 0.)
        },
    }
}

fn draw_convert_page(app: &mut App, font: &Font, p: Palette, rect: Rect) {
    panel(rect, p);
    label(font, p, "官方谱面 → RPEN", rect.x + 22., rect.y + 34., 20);
    sublabel(
        font,
        p,
        "自动识别 JSON / PEZ；RPE 与 RPEN 将原样保留。",
        rect.x + 22.,
        rect.y + 60.,
        13,
    );
    let row_y = rect.y + 104.;
    label(font, p, "输入谱面", rect.x + 22., row_y, 13);
    let path_rect = Rect::new(rect.x + 22., row_y + 12., rect.w - 44., 42.);
    panel(path_rect, p);
    let path_label = app
        .input_path
        .as_ref()
        .map(|it| it.display().to_string())
        .unwrap_or_else(|| "未选择文件".to_owned());
    fit_text(
        font,
        &path_label,
        path_rect.x + 12.,
        path_rect.y + 26.,
        path_rect.w - 24.,
        13,
        p.secondary,
    );
    if button(
        font,
        p,
        Rect::new(rect.x + 22., row_y + 65., 146., 34.),
        "选择谱面…",
        false,
        false,
    ) {
        if let Some(path) = rfd::FileDialog::new()
            .add_filter("Phigros 谱面", &["json", "pez", "zip"])
            .pick_file()
        {
            app.select_chart(path);
        }
    }
    if button(
        font,
        p,
        Rect::new(rect.x + 178., row_y + 65., 146., 34.),
        "关联音乐…",
        false,
        false,
    ) {
        if let Some(path) = rfd::FileDialog::new()
            .add_filter("音频", &["ogg", "mp3", "wav", "flac", "m4a"])
            .pick_file()
        {
            app.music_path = Some(path);
            app.start_preview_reload();
            app.save_settings();
        }
    }
    let music_y = row_y + 119.;
    sublabel(
        font,
        p,
        &format!(
            "音乐：{}",
            app.music_path.as_ref().map_or("使用 PEZ 内资源", |it| it
                .file_name()
                .and_then(|n| n.to_str())
                .unwrap_or("已关联"))
        ),
        rect.x + 22.,
        music_y,
        13,
    );
    draw_line(
        rect.x + 22.,
        music_y + 22.,
        rect.x + rect.w - 22.,
        music_y + 22.,
        1.,
        p.border,
    );
    let mode_y = music_y + 56.;
    label(font, p, "输出模式", rect.x + 22., mode_y, 13);
    if button(
        font,
        p,
        Rect::new(rect.x + 22., mode_y + 12., 150., 38.),
        "标准版",
        false,
        !app.settings.archive_mode,
    ) {
        app.settings.archive_mode = false;
        app.save_settings();
    }
    if button(
        font,
        p,
        Rect::new(rect.x + 184., mode_y + 12., 150., 38.),
        "归档版",
        false,
        app.settings.archive_mode,
    ) {
        app.settings.archive_mode = true;
        app.save_settings();
    }
    sublabel(
        font,
        p,
        "标准版不内嵌原始官谱；归档版会保存原始数据。",
        rect.x + 22.,
        mode_y + 68.,
        12,
    );
    let y = mode_y + 112.;
    if button(
        font,
        p,
        Rect::new(rect.x + 22., y, 180., 42.),
        "转换并保存…",
        true,
        false,
    ) {
        if let Some(input) = app.input_path.clone() {
            let default = default_conversion_name(&input);
            if let Some(output) = rfd::FileDialog::new().set_file_name(default).save_file() {
                app.conversion = Some(conversion_channel(
                    input,
                    output,
                    output_mode(&app.settings),
                    app.music_path.clone(),
                ));
                app.status = "正在转换…".to_owned();
            }
        } else {
            app.status = "请先选择谱面文件".to_owned();
        }
    }
    if button(
        font,
        p,
        Rect::new(rect.x + 216., y, 140., 42.),
        "渲染预览",
        false,
        false,
    ) {
        start_preview(app);
        app.page = Page::Work;
    }
}

fn draw_skins_page(app: &mut App, font: &Font, p: Palette, rect: Rect) {
    panel(rect, p);
    label(font, p, "渲染皮肤", rect.x + 22., rect.y + 34., 20);
    sublabel(
        font,
        p,
        "选择本地资源包，Note、Hold、打击特效与音效会一起加载。",
        rect.x + 22.,
        rect.y + 60.,
        13,
    );
    let skin_name = app
        .settings
        .skin_path
        .as_ref()
        .and_then(|it| it.file_name())
        .and_then(|it| it.to_str())
        .unwrap_or("内置默认皮肤");
    let box_rect = Rect::new(rect.x + 22., rect.y + 94., rect.w - 44., 100.);
    panel(box_rect, p);
    label(font, p, skin_name, box_rect.x + 16., box_rect.y + 29., 16);
    if let Some(path) = &app.settings.skin_path {
        fit_text(
            font,
            &path.display().to_string(),
            box_rect.x + 16.,
            box_rect.y + 56.,
            box_rect.w - 32.,
            12,
            p.secondary,
        );
    } else {
        sublabel(
            font,
            p,
            "使用程序内置的可分发皮肤",
            box_rect.x + 16.,
            box_rect.y + 57.,
            12,
        );
    }
    let y = box_rect.y + box_rect.h + 22.;
    let mut changed = false;
    if button(
        font,
        p,
        Rect::new(rect.x + 22., y, 172., 38.),
        "选择皮肤 ZIP…",
        true,
        false,
    ) {
        if let Some(path) = rfd::FileDialog::new()
            .add_filter("皮肤包", &["zip"])
            .pick_file()
        {
            app.settings.skin_path = Some(path);
            changed = true;
        }
    }
    if button(
        font,
        p,
        Rect::new(rect.x + 206., y, 172., 38.),
        "选择皮肤文件夹…",
        false,
        false,
    ) {
        if let Some(path) = rfd::FileDialog::new().pick_folder() {
            app.settings.skin_path = Some(path);
            changed = true;
        }
    }
    if button(
        font,
        p,
        Rect::new(rect.x + 390., y, 150., 38.),
        "恢复默认",
        false,
        false,
    ) {
        app.settings.skin_path = None;
        changed = true;
    }
    if changed {
        app.save_settings();
        app.start_preview_reload();
        app.status = "皮肤已切换，正在重载预览…".to_owned();
    }
    let note_y = y + 78.;
    sublabel(
        font,
        p,
        "皮肤仅从你选择的本地路径读取，不会复制进程序或公开发行包。",
        rect.x + 22.,
        note_y,
        12,
    );
    sublabel(
        font,
        p,
        "如资源包缺少某类打击音，会回退到内置资源包的对应音效。",
        rect.x + 22.,
        note_y + 22.,
        12,
    );
}

fn draw_export_page(app: &mut App, font: &Font, p: Palette, rect: Rect) {
    panel(rect, p);
    label(font, p, "视频导出", rect.x + 22., rect.y + 34., 20);
    sublabel(
        font,
        p,
        "离屏 GPU 绘制 · NVENC 可选 · 使用与预览相同的皮肤",
        rect.x + 22.,
        rect.y + 60.,
        13,
    );
    let res = ["1280 × 720", "1920 × 1080", "2560 × 1440", "3840 × 2160"];
    let fps = ["30 FPS", "60 FPS", "120 FPS"];
    let y = rect.y + 102.;
    label(font, p, "分辨率", rect.x + 22., y, 13);
    if button(
        font,
        p,
        Rect::new(rect.x + 22., y + 12., 176., 38.),
        res[app.settings.resolution.min(3)],
        false,
        false,
    ) {
        app.settings.resolution = (app.settings.resolution + 1) % res.len();
        app.save_settings();
    }
    label(font, p, "帧率", rect.x + 222., y, 13);
    if button(
        font,
        p,
        Rect::new(rect.x + 222., y + 12., 140., 38.),
        fps[app.settings.fps.min(2)],
        false,
        false,
    ) {
        app.settings.fps = (app.settings.fps + 1) % fps.len();
        app.save_settings();
    }
    label(font, p, "编码器", rect.x + 22., y + 78., 13);
    if button(
        font,
        p,
        Rect::new(rect.x + 22., y + 90., 176., 38.),
        if app.settings.use_nvenc {
            "NVIDIA NVENC"
        } else {
            "H.264 软件编码"
        },
        false,
        false,
    ) {
        app.settings.use_nvenc = !app.settings.use_nvenc;
        app.save_settings();
    }
    sublabel(
        font,
        p,
        "NVENC 只加速编码；帧仍由 GPU 渲染后回传给编码器。",
        rect.x + 22.,
        y + 143.,
        12,
    );
    if checkbox(
        font,
        p,
        rect.x + 22.,
        y + 191.,
        "保留分数、连击与歌曲信息",
        app.settings.show_hud,
    ) {
        app.settings.show_hud = !app.settings.show_hud;
        app.save_settings();
    }
    if checkbox(
        font,
        p,
        rect.x + 22.,
        y + 226.,
        "显示实时准确率百分比",
        app.settings.show_accuracy,
    ) {
        app.settings.show_accuracy = !app.settings.show_accuracy;
        app.save_settings();
    }
    if checkbox(
        font,
        p,
        rect.x + 22.,
        y + 261.,
        "混入皮肤打击音",
        app.settings.hitsounds,
    ) {
        app.settings.hitsounds = !app.settings.hitsounds;
        app.save_settings();
    }
    let job_y = y + 306.;
    match &app.export {
        ExportJob::Preparing { .. } => {
            sublabel(font, p, "正在准备谱面…", rect.x + 22., job_y + 20., 13)
        }
        ExportJob::Loading(_, _) => {
            sublabel(font, p, "正在载入渲染场景…", rect.x + 22., job_y + 20., 13)
        }
        ExportJob::Rendering(renderer, _, started) => {
            let (done, total) = renderer.progress();
            let ratio = if total == 0 {
                0.
            } else {
                done as f32 / total as f32
            };
            draw_rectangle(
                rect.x + 22.,
                job_y,
                (rect.w - 44.).max(80.),
                12.,
                p.surface3,
            );
            draw_rectangle(
                rect.x + 22.,
                job_y,
                (rect.w - 44.).max(80.) * ratio,
                12.,
                p.accent,
            );
            sublabel(
                font,
                p,
                &format!(
                    "{done}/{total} 帧 · 已用 {:.0} 秒",
                    started.elapsed().as_secs_f32()
                ),
                rect.x + 22.,
                job_y + 36.,
                13,
            );
            if button(
                font,
                p,
                Rect::new(rect.x + 22., job_y + 52., 130., 36.),
                "取消导出",
                false,
                false,
            ) {
                let state = std::mem::replace(&mut app.export, ExportJob::Idle);
                if let ExportJob::Rendering(mut renderer, _prepared, _) = state {
                    renderer.cancel();
                }
                app.status = "导出已取消".to_owned();
            }
        }
        ExportJob::Done => sublabel(font, p, "视频已生成", rect.x + 22., job_y + 20., 13),
        ExportJob::Error(error) => fit_text(
            font,
            error,
            rect.x + 22.,
            job_y + 20.,
            rect.w - 44.,
            12,
            p.danger,
        ),
        ExportJob::Idle => {}
    }
    if !matches!(
        &app.export,
        ExportJob::Preparing { .. } | ExportJob::Loading(_, _) | ExportJob::Rendering(_, _, _)
    ) && button(
        font,
        p,
        Rect::new(rect.x + 22., rect.y + rect.h - 66., 186., 42.),
        "选择位置并开始导出…",
        true,
        false,
    ) {
        start_export(app);
    }
}

fn draw_settings_page(app: &mut App, font: &Font, p: Palette, rect: Rect) {
    panel(rect, p);
    label(font, p, "外观与默认项", rect.x + 22., rect.y + 34., 20);
    sublabel(
        font,
        p,
        "设置保存在当前 Windows 用户的 PhiRender 配置中。",
        rect.x + 22.,
        rect.y + 60.,
        13,
    );
    let y = rect.y + 104.;
    label(font, p, "外观模式", rect.x + 22., y, 13);
    let modes = [
        (ThemeMode::System, "跟随系统"),
        (ThemeMode::Light, "浅色"),
        (ThemeMode::Dark, "深色"),
    ];
    for (i, (mode, title)) in modes.iter().enumerate() {
        if button(
            font,
            p,
            Rect::new(rect.x + 22. + i as f32 * 126., y + 12., 112., 36.),
            title,
            false,
            app.settings.theme == *mode,
        ) {
            app.settings.theme = *mode;
            app.save_settings();
        }
    }
    let vy = y + 92.;
    label(font, p, "预览音量", rect.x + 22., vy, 13);
    let slider = Rect::new(rect.x + 22., vy + 18., 260., 10.);
    draw_rectangle(slider.x, slider.y, slider.w, slider.h, p.surface3);
    draw_rectangle(
        slider.x,
        slider.y,
        slider.w * app.settings.volume.clamp(0., 1.),
        slider.h,
        p.accent,
    );
    let (mx, my) = mouse_position();
    if is_mouse_button_down(MouseButton::Left)
        && mx >= slider.x
        && mx <= slider.x + slider.w
        && my >= slider.y - 8.
        && my <= slider.y + slider.h + 8.
    {
        app.settings.volume = ((mx - slider.x) / slider.w).clamp(0., 1.);
        if let Some(preview) = app.preview.as_mut() {
            let _ = preview.preview_set_volume(app.settings.volume);
        }
        app.save_settings();
    }
    sublabel(
        font,
        p,
        &format!("{:.0}%", app.settings.volume * 100.),
        slider.x + slider.w + 16.,
        vy + 27.,
        12,
    );
    if checkbox(
        font,
        p,
        rect.x + 22.,
        vy + 78.,
        "默认使用归档模式",
        app.settings.archive_mode,
    ) {
        app.settings.archive_mode = !app.settings.archive_mode;
        app.save_settings();
    }
    sublabel(
        font,
        p,
        "实时准确率默认关闭；可在导出页单独打开。",
        rect.x + 22.,
        vy + 116.,
        12,
    );
}

fn draw_about_page(font: &Font, p: Palette, rect: Rect) {
    panel(rect, p);
    label(font, p, "PhiRender", rect.x + 22., rect.y + 42., 24);
    sublabel(
        font,
        p,
        "Phigros 谱面转换与 GPU 渲染工具",
        rect.x + 22.,
        rect.y + 73.,
        14,
    );
    draw_line(
        rect.x + 22.,
        rect.y + 98.,
        rect.x + rect.w - 22.,
        rect.y + 98.,
        1.,
        p.border,
    );
    label(font, p, "当前功能", rect.x + 22., rect.y + 132., 15);
    let lines = [
        "• 官方谱面、RPE 与 RPEN 的识别与转换",
        "• PEZ / ZIP 资源保留与本地皮肤加载",
        "• 基于 Phira Pro 渲染场景的 GPU 预览与离屏导出",
        "• 软件 H.264 与可选 NVENC 编码",
    ];
    for (i, text) in lines.iter().enumerate() {
        sublabel(
            font,
            p,
            text,
            rect.x + 22.,
            rect.y + 164. + i as f32 * 28.,
            13,
        );
    }
    sublabel(
        font,
        p,
        "渲染核心采用 GPL-3.0 项目组件；随包素材与资源包依各自许可使用。",
        rect.x + 22.,
        rect.y + 310.,
        12,
    );
}

fn current_palette(settings: &Settings, system_light: bool) -> Palette {
    palette(settings.theme, system_light)
}

fn draw_frame(app: &mut App, font: &Font, system_light: bool) {
    let p = current_palette(&app.settings, system_light);
    clear_background(p.bg);
    let layout = layout();
    draw_sidebar(app, font, p, layout.sidebar);
    draw_topbar(app, font, p);
    match app.page {
        Page::Work => draw_work_page(app, font, p, layout),
        Page::Convert => draw_convert_page(app, font, p, layout.content),
        Page::Skins => draw_skins_page(app, font, p, layout.content),
        Page::Export => draw_export_page(app, font, p, layout.content),
        Page::Settings => draw_settings_page(app, font, p, layout.content),
        Page::About => draw_about_page(font, p, layout.content),
    }
    if !app.status.is_empty() {
        draw_rectangle(0., screen_height() - 26., screen_width(), 26., p.surface);
        draw_line(
            0.,
            screen_height() - 26.,
            screen_width(),
            screen_height() - 26.,
            1.,
            p.border,
        );
        fit_text(
            font,
            &app.status,
            layout.sidebar + 18.,
            screen_height() - 8.,
            screen_width() - layout.sidebar - 36.,
            12,
            p.secondary,
        );
    }
    REQUEST_EXPORT.with(|request| {
        if request.replace(false) {
            app.page = Page::Export;
        }
    });
    APP_ACTION.with(|action| {
        if let Some(action) = action.borrow_mut().take() {
            match action {
                UiAction::Music(path) => {
                    app.music_path = Some(path);
                    start_preview(app);
                    app.save_settings();
                }
            }
        }
    });
}

fn resolve_project_root() -> PathBuf {
    let assets = Path::new("work").join("Phira-Pro").join("assets");
    let mut candidates = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.to_path_buf());
            if let Some(parent) = dir.parent() {
                candidates.push(parent.to_path_buf());
            }
        }
    }
    if let Ok(dir) = std::env::current_dir() {
        candidates.push(dir);
    }
    candidates.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")));
    candidates
        .into_iter()
        .find(|path| path.join(&assets).is_dir())
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")))
}

fn load_default_game_font(assets: &Path) -> Option<Vec<u8>> {
    let mut candidates = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join("saira-regular.ttf"));
        }
    }
    candidates.push(assets.join("saira-regular.ttf"));
    candidates.push(assets.join("phigros.ttf"));
    candidates
        .into_iter()
        .find_map(|path| std::fs::read(path).ok())
}

#[macroquad::main(window_conf)]
async fn main() {
    let runtime = match tokio::runtime::Runtime::new() {
        Ok(runtime) => runtime,
        Err(error) => {
            eprintln!("无法启动异步运行环境：{error}");
            return;
        }
    };
    let _runtime_guard = runtime.enter();
    let root = resolve_project_root();
    let assets = root.join("work").join("Phira-Pro").join("assets");
    set_pc_assets_folder(assets.to_string_lossy().as_ref());
    let font_bytes = match std::fs::read(assets.join("harmonyos.ttf")) {
        Ok(bytes) => bytes,
        Err(error) => {
            eprintln!("读取界面字体失败：{error}");
            return;
        }
    };
    let ui_font = match load_ttf_font_from_bytes(&font_bytes) {
        Ok(font) => font,
        Err(error) => {
            eprintln!("载入界面字体失败：{error}");
            return;
        }
    };
    let fallback = match FontArc::try_from_vec(font_bytes.clone()) {
        Ok(font) => font,
        Err(error) => {
            eprintln!("解析渲染字体失败：{error}");
            return;
        }
    };
    let mut painter = TextPainter::new(fallback.clone(), None);
    if let Some(bytes) = load_default_game_font(&assets) {
        if let Ok(font) = FontArc::try_from_vec(bytes) {
            PGR_FONT.with(|slot| {
                *slot.borrow_mut() = Some(TextPainter::new(font, Some(fallback.clone())))
            });
        }
    }
    if let Ok(bytes) = std::fs::read(assets.join("bold.ttf")) {
        if let Ok(font) = FontArc::try_from_vec(bytes) {
            BOLD_FONT
                .with(|slot| *slot.borrow_mut() = Some(TextPainter::new(font, Some(fallback))));
        }
    }
    let mut app = App::new();
    let system_light = system_is_light();
    loop {
        update_preview(&mut app, &mut painter);
        update_export(&mut app, &mut painter);
        update_conversion(&mut app);

        if !matches!(&app.export, ExportJob::Rendering(_, _, _)) {
            if let Some(preview) = app.preview.as_mut() {
                let time = preview.preview_time();
                if (time - app.last_preview_time).abs() > 0.002 || app.last_preview_time < 0. {
                    if let Err(error) = preview.render_preview_at(&mut painter, time) {
                        app.status = format!("预览渲染失败：{error:#}");
                    } else {
                        app.last_preview_time = time;
                    }
                }
            }
        }

        // GameScene renders into its offscreen target and leaves that camera
        // active. Switch back to the window framebuffer before drawing the
        // application UI, otherwise the UI is drawn into the chart texture
        // and the desktop window stays black after preview initialization.
        set_default_camera();
        draw_frame(&mut app, &ui_font, system_light);
        next_frame().await;
    }
}
