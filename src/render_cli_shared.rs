use anyhow::{anyhow, bail, Context, Result};
use clap::{CommandFactory, FromArgMatches, Parser, ValueEnum};
use macroquad::prelude::*;
use phigros_rpen_converter::{
    archive, convert_json_with_mode, parse_document, ChartFormat, OutputMode,
};
use prpr::{
    chart_video::{ChartVideoRenderer, VideoEncoder, VideoOptions},
    core::{BOLD_FONT, PGR_FONT},
    ext::poll_future,
    ui::{FontArc, TextPainter},
};
use serde_json::Value;
use std::{
    collections::BTreeMap,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RunMode {
    Integrated,
    PureRpen,
}

#[derive(Clone, Copy, Debug, Default, ValueEnum)]
enum CliOutputMode {
    #[default]
    Standard,
    Archive,
}

impl From<CliOutputMode> for OutputMode {
    fn from(value: CliOutputMode) -> Self {
        match value {
            CliOutputMode::Standard => Self::Standard,
            CliOutputMode::Archive => Self::Archive,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, ValueEnum)]
enum CliEncoder {
    #[default]
    Software,
    Nvenc,
}

#[derive(Debug, Parser)]
struct RenderArgs {
    /// Input chart JSON, PEZ, or ZIP.
    input: PathBuf,
    /// Output MP4 path.
    output: PathBuf,
    #[arg(long, default_value_t = 1920)]
    width: u32,
    #[arg(long, default_value_t = 1080)]
    height: u32,
    #[arg(long, default_value_t = 60)]
    fps: u32,
    /// Start time in seconds.
    #[arg(long, default_value_t = 0.0)]
    start: f64,
    /// End time in seconds. Defaults to the chart/music length.
    #[arg(long)]
    end: Option<f64>,
    /// Keep the HUD visible (visible by default).
    #[arg(long)]
    no_hud: bool,
    /// Show the real-time accuracy percentage (hidden by default).
    #[arg(long)]
    show_accuracy: bool,
    /// Do not mix note hit sounds into the video.
    #[arg(long)]
    no_hitsound: bool,
    /// Export the video without the chart's music and hit sounds.
    #[arg(long)]
    no_audio: bool,
    #[arg(long, default_value_t = 1)]
    samples: u32,
    #[arg(long, value_enum, default_value_t = CliEncoder::Software)]
    encoder: CliEncoder,
    /// Optional Phira resource-pack directory or ZIP.
    #[arg(long)]
    skin: Option<PathBuf>,
    /// Optional ffmpeg executable path.
    #[arg(long)]
    ffmpeg: Option<PathBuf>,
    /// Conversion output mode for official charts in the integrated CLI.
    #[arg(long, value_enum, default_value_t = CliOutputMode::Standard, hide = true)]
    mode: CliOutputMode,
    /// Name used when a raw JSON file needs a temporary chart package.
    #[arg(long)]
    name: Option<String>,
    /// Level text used for a temporary package made from raw JSON.
    #[arg(long)]
    level: Option<String>,
    /// Attach music to a raw JSON input.
    #[arg(long)]
    music: Option<PathBuf>,
    /// Attach an illustration to a raw JSON input.
    #[arg(long)]
    picture: Option<PathBuf>,
}

struct TemporaryChart {
    path: PathBuf,
    remove_on_drop: bool,
}

impl Drop for TemporaryChart {
    fn drop(&mut self) {
        if self.remove_on_drop {
            let _ = std::fs::remove_file(&self.path);
        }
    }
}

struct PreparedChart {
    file: TemporaryChart,
    source_format: ChartFormat,
    notes: usize,
    judge_lines: usize,
    noise_areas: usize,
    warnings: Vec<String>,
}

pub fn conf() -> Conf {
    Conf {
        window_title: "Phigros command-line renderer".into(),
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

pub async fn run(mode: RunMode) {
    if let Err(error) = run_inner(mode).await {
        eprintln!("Error: {error:#}");
        std::process::exit(1);
    }
}

async fn run_inner(mode: RunMode) -> Result<()> {
    let args = parse_args(mode);
    validate_args(&args)?;
    let root = project_root();
    configure_ffmpeg(&root, args.ffmpeg.as_deref())?;
    initialize_renderer_assets(&root)?;

    let runtime = tokio::runtime::Runtime::new().context("Could not start async runtime")?;
    let _runtime_guard = runtime.enter();
    let font_path = root.join("work/Phira-Pro/assets/harmonyos.ttf");
    let font = FontArc::try_from_vec(
        std::fs::read(&font_path)
            .with_context(|| format!("Could not read renderer font {}", font_path.display()))?,
    )
    .map_err(|error| anyhow!("Could not load renderer font: {error}"))?;
    let mut painter = TextPainter::new(font.clone(), None);
    let pgr_font_path = root.join("work/Phira-Pro/assets/phigros.ttf");
    let pgr_font =
        FontArc::try_from_vec(std::fs::read(&pgr_font_path).with_context(|| {
            format!("Could not read renderer font {}", pgr_font_path.display())
        })?)
        .map_err(|error| anyhow!("Could not load Phigros font: {error}"))?;
    PGR_FONT.with(|slot| {
        *slot.borrow_mut() = Some(TextPainter::new(pgr_font, Some(font.clone())));
    });
    let bold_font_path = root.join("work/Phira-Pro/assets/bold.ttf");
    let bold_font =
        FontArc::try_from_vec(std::fs::read(&bold_font_path).with_context(|| {
            format!("Could not read renderer font {}", bold_font_path.display())
        })?)
        .map_err(|error| anyhow!("Could not load bold font: {error}"))?;
    BOLD_FONT.with(|slot| {
        *slot.borrow_mut() = Some(TextPainter::new(bold_font, Some(font.clone())));
    });
    next_frame().await;

    let prepared = prepare_chart(&args, mode)?;
    print_chart_summary(&prepared, mode);
    let options = VideoOptions {
        width: args.width,
        height: args.height,
        fps: args.fps,
        start: args.start,
        end: args.end,
        hitsound: !args.no_hitsound,
        show_hud: !args.no_hud,
        show_accuracy: args.show_accuracy,
        show_offset_indicator: false,
        include_audio: !args.no_audio,
        sample_count: args.samples,
        encoder: match args.encoder {
            CliEncoder::Software => VideoEncoder::H264Software,
            CliEncoder::Nvenc => VideoEncoder::H264Nvenc,
        },
        res_pack_path: args.skin.clone(),
    };
    options.validate().context("Invalid render settings")?;
    let mut load = Box::pin(ChartVideoRenderer::new(
        prepared.file.path.clone(),
        options,
        args.output.clone(),
    ));
    let mut renderer = loop {
        if let Some(result) = poll_future(load.as_mut()) {
            break result.context("Could not load chart for rendering")?;
        }
        next_frame().await;
    };

    let total = renderer.progress().1;
    println!(
        "Rendering {}: {} frames, {}x{} @ {} FPS",
        renderer.title(),
        total,
        args.width,
        args.height,
        args.fps
    );
    while !renderer.finished() {
        renderer
            .render_next_frame(&mut painter)
            .context("Frame rendering failed")?;
        let (frame, total) = renderer.progress();
        if frame % 120 == 0 || frame == total {
            println!("Rendered {frame}/{total} frames");
        }
    }
    let output = renderer.finish().context("Video encoding failed")?;
    println!("Video saved: {}", output.display());
    Ok(())
}

fn parse_args(mode: RunMode) -> RenderArgs {
    let (name, about) = match mode {
        RunMode::Integrated => (
            "phigros-render-cli",
            "Convert official Phigros charts when needed, then render a video.",
        ),
        RunMode::PureRpen => (
            "phigros-render-pure",
            "Render an existing RPEN chart without converting it.",
        ),
    };
    let command = RenderArgs::command()
        .name(name)
        .version(env!("CARGO_PKG_VERSION"))
        .about(about);
    let matches = command.get_matches();
    RenderArgs::from_arg_matches(&matches).unwrap_or_else(|error| error.exit())
}

fn validate_args(args: &RenderArgs) -> Result<()> {
    if !args.input.exists() {
        bail!("Input chart does not exist: {}", args.input.display());
    }
    if let Some(skin) = &args.skin {
        if !skin.exists() {
            bail!("Skin resource does not exist: {}", skin.display());
        }
    }
    if let Some(ffmpeg) = &args.ffmpeg {
        if !ffmpeg.is_file() {
            bail!("ffmpeg does not exist: {}", ffmpeg.display());
        }
    }
    if args.music.as_ref().is_some_and(|path| !path.is_file()) {
        bail!("Music file does not exist");
    }
    if args.picture.as_ref().is_some_and(|path| !path.is_file()) {
        bail!("Illustration file does not exist");
    }
    if args.samples == 0 || args.samples > 8 {
        bail!("--samples must be between 1 and 8");
    }
    if args.end.is_some_and(|end| end <= args.start) {
        bail!("--end must be greater than --start");
    }
    if args.music.is_some() || args.picture.is_some() {
        if is_archive(&args.input) {
            bail!("--music and --picture can only be used with a raw JSON chart");
        }
    }
    Ok(())
}

fn prepare_chart(args: &RenderArgs, mode: RunMode) -> Result<PreparedChart> {
    if is_archive(&args.input) {
        let bytes = std::fs::read(&args.input)
            .with_context(|| format!("Could not read {}", args.input.display()))?;
        let (file, report, warnings) = match mode {
            RunMode::Integrated => {
                let (packed, report) =
                    archive::convert_archive_with_mode(&bytes, args.mode.into())?;
                let file = write_temp_chart(&packed)?;
                let warnings = report.warnings.clone();
                (file, report, warnings)
            }
            RunMode::PureRpen => {
                let report = archive::inspect_archive(&bytes)?;
                if report.format != ChartFormat::Rpen {
                    bail!(
                        "Pure renderer only accepts RPEN charts; this archive contains {:?}. Use phigros-render-cli to auto-convert official charts.",
                        report.format
                    );
                }
                (
                    TemporaryChart {
                        path: args.input.clone(),
                        remove_on_drop: false,
                    },
                    report,
                    Vec::new(),
                )
            }
        };
        return Ok(PreparedChart {
            file,
            source_format: report.format,
            notes: report.notes,
            judge_lines: report.judge_lines,
            noise_areas: report.noise_areas,
            warnings,
        });
    }
    if !is_json(&args.input) {
        bail!("Input must be a .json, .pez, or .zip chart");
    }

    let source = std::fs::read_to_string(&args.input)
        .with_context(|| format!("Could not read chart JSON {}", args.input.display()))?;
    let parsed = parse_document(&source).context("Could not recognize chart JSON format")?;
    let source_format = parsed.format;
    if mode == RunMode::PureRpen && source_format != ChartFormat::Rpen {
        bail!(
            "Pure renderer only accepts RPEN JSON; this file is {:?}. Use phigros-render-cli to auto-convert official charts.",
            source_format
        );
    }

    let (chart_json, warnings) = if mode == RunMode::Integrated {
        let (converted, report) = convert_json_with_mode(&source, args.mode.into())?;
        let warnings = report
            .diagnostics
            .iter()
            .map(|diagnostic| {
                format!(
                    "{:?} {}: {}",
                    diagnostic.severity, diagnostic.path, diagnostic.message
                )
            })
            .collect();
        (converted, warnings)
    } else {
        (source, Vec::new())
    };
    let chart_value: Value = serde_json::from_str(&chart_json)?;
    let output_format = phigros_rpen_converter::detect_format(&chart_value)?;
    let (notes, judge_lines, noise_areas) = chart_counts(&chart_value, output_format);
    let packed = make_json_package(args, &chart_json)?;
    Ok(PreparedChart {
        file: write_temp_chart(&packed)?,
        source_format,
        notes,
        judge_lines,
        noise_areas,
        warnings,
    })
}

fn make_json_package(args: &RenderArgs, chart_json: &str) -> Result<Vec<u8>> {
    let stem = args
        .input
        .file_stem()
        .and_then(|value| value.to_str())
        .filter(|value| !value.is_empty())
        .unwrap_or("chart");
    let title = clean_info_field(args.name.as_deref().unwrap_or(stem));
    let level = clean_info_field(args.level.as_deref().unwrap_or(""));
    let mut files = BTreeMap::new();
    files.insert("chart.json".to_owned(), chart_json.as_bytes().to_vec());

    let music_name = args
        .music
        .as_deref()
        .map(|path| resource_name(path, "music"));
    if let (Some(path), Some(name)) = (&args.music, &music_name) {
        files.insert(
            name.clone(),
            std::fs::read(path)
                .with_context(|| format!("Could not read music {}", path.display()))?,
        );
    }
    let picture_name = args
        .picture
        .as_deref()
        .map(|path| resource_name(path, "picture"));
    if let (Some(path), Some(name)) = (&args.picture, &picture_name) {
        files.insert(
            name.clone(),
            std::fs::read(path)
                .with_context(|| format!("Could not read illustration {}", path.display()))?,
        );
    }
    let info = format!(
        "#\nName: {title}\nPath: 00000000\nSong: {}\nPicture: {}\nChart: chart.json\nLevel: {level}\nComposer: \nCharter: \nIllustrator: \n",
        music_name.as_deref().unwrap_or(""),
        picture_name.as_deref().unwrap_or("")
    );
    files.insert("info.txt".to_owned(), info.into_bytes());
    Ok(archive::write_zip(&files)?)
}

fn resource_name(path: &Path, stem: &str) -> String {
    let extension = path
        .extension()
        .and_then(|value| value.to_str())
        .filter(|value| !value.is_empty())
        .unwrap_or("dat");
    format!("{stem}.{extension}")
}

fn clean_info_field(value: &str) -> String {
    value.replace(['\r', '\n'], " ")
}

fn write_temp_chart(bytes: &[u8]) -> Result<TemporaryChart> {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .context("System clock is before Unix epoch")?
        .as_nanos();
    let path = std::env::temp_dir().join(format!(
        "phigros-cli-render-{}-{stamp}.pez",
        std::process::id()
    ));
    std::fs::write(&path, bytes)
        .with_context(|| format!("Could not prepare temporary chart {}", path.display()))?;
    Ok(TemporaryChart {
        path,
        remove_on_drop: true,
    })
}

fn chart_counts(chart: &Value, format: ChartFormat) -> (usize, usize, usize) {
    let lines = chart["judgeLineList"].as_array();
    let judge_lines = lines.map_or(0, Vec::len);
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
    let noise_areas = chart["blockAreaList"].as_array().map_or(0, Vec::len);
    (notes, judge_lines, noise_areas)
}

fn print_chart_summary(chart: &PreparedChart, mode: RunMode) {
    let input_format = format_name(chart.source_format);
    let disposition = match (mode, chart.source_format) {
        (RunMode::Integrated, ChartFormat::OfficialV3) => "converted to RPEN",
        (RunMode::Integrated, _) => "kept as-is",
        (RunMode::PureRpen, _) => "RPEN input; no conversion",
    };
    println!("Input: {input_format} ({disposition})");
    println!(
        "Chart: {} notes, {} judge lines, {} noise areas",
        chart.notes, chart.judge_lines, chart.noise_areas
    );
    for warning in &chart.warnings {
        eprintln!("Warning: {warning}");
    }
}

fn format_name(format: ChartFormat) -> &'static str {
    match format {
        ChartFormat::OfficialV3 => "Phigros v3",
        ChartFormat::Rpe => "RPE",
        ChartFormat::Rpen => "RPEN",
    }
}

fn is_archive(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| {
            extension.eq_ignore_ascii_case("pez") || extension.eq_ignore_ascii_case("zip")
        })
}

fn is_json(path: &Path) -> bool {
    path.extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| extension.eq_ignore_ascii_case("json"))
}

fn project_root() -> PathBuf {
    let assets_relative = Path::new("work").join("Phira-Pro").join("assets");
    let executable_root = std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(Path::to_path_buf))
        .and_then(|path| path.parent().map(Path::to_path_buf));
    if let Some(root) = executable_root.filter(|root| root.join(&assets_relative).is_dir()) {
        return root;
    }
    let current_dir = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    if current_dir.join(&assets_relative).is_dir() {
        return current_dir;
    }
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

fn initialize_renderer_assets(root: &Path) -> Result<()> {
    let assets = root.join("work/Phira-Pro/assets");
    for required in ["harmonyos.ttf", "phigros.ttf", "bold.ttf"] {
        if !assets.join(required).is_file() {
            bail!(
                "Renderer resource is missing: {}. Keep the work/Phira-Pro/assets folder beside the outputs folder.",
                assets.join(required).display()
            );
        }
    }
    macroquad::file::set_pc_assets_folder(assets.to_string_lossy().as_ref());
    Ok(())
}

fn configure_ffmpeg(root: &Path, explicit: Option<&Path>) -> Result<()> {
    if let Some(path) = explicit {
        std::env::set_var("PHIRA_FFMPEG", path);
        return Ok(());
    }
    if std::env::var_os("PHIRA_FFMPEG").is_some() {
        return Ok(());
    }
    for relative in [
        "work/ffmpeg-package/extracted/ffmpeg-9.0.1-essentials_build/bin/ffmpeg.exe",
        "work/ffmpeg-8.0.1-extracted/ffmpeg-8.0.1-essentials_build/bin/ffmpeg.exe",
    ] {
        let candidate = root.join(relative);
        if candidate.is_file() {
            std::env::set_var("PHIRA_FFMPEG", candidate);
            break;
        }
    }
    Ok(())
}
