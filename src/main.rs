use clap::{Parser, ValueEnum};
use phigros_rpen_converter::{
    archive, convert_json_with_mode, parse_document, ChartFormat, Error, OutputMode,
};
use std::{fs, path::PathBuf};

#[derive(Parser)]
#[command(
    name = "phigros-rpen",
    version,
    about = "Convert official Phigros v3 charts to RPEN JSON/PEZ"
)]
struct Args {
    input: PathBuf,
    output: PathBuf,
    #[arg(long, value_enum, default_value_t = CliOutputMode::Standard)]
    mode: CliOutputMode,
}

#[derive(Clone, Copy, Debug, Default, ValueEnum)]
enum CliOutputMode {
    #[default]
    Standard,
    Archive,
}

impl From<CliOutputMode> for OutputMode {
    fn from(mode: CliOutputMode) -> Self {
        match mode {
            CliOutputMode::Standard => Self::Standard,
            CliOutputMode::Archive => Self::Archive,
        }
    }
}
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse();
    let mode: OutputMode = args.mode.into();
    let input = fs::read(&args.input)?;
    let zip_input = args
        .input
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case("pez") || e.eq_ignore_ascii_case("zip"));
    let zip_output = args
        .output
        .extension()
        .and_then(|e| e.to_str())
        .is_some_and(|e| e.eq_ignore_ascii_case("pez") || e.eq_ignore_ascii_case("zip"));
    if zip_input {
        if !zip_output {
            return Err(
                "ZIP/PEZ input requires .pez or .zip output to preserve bundled assets".into(),
            );
        }
        let (bytes, report) = archive::convert_archive_with_mode(&input, mode)?;
        fs::write(&args.output, bytes)?;
        println!(
            "输入格式：{:?}；输出格式：{}；模式：{:?}；Note：{}；判定线：{}；噪域：{}；保留资源：{} 个",
            report.format,
            if report.format == ChartFormat::OfficialV3 {
                "RPEN"
            } else {
                match report.format {
                    ChartFormat::Rpe => "RPE",
                    ChartFormat::Rpen => "RPEN",
                    ChartFormat::OfficialV3 => unreachable!(),
                }
            },
            args.mode,
            report.notes,
            report.judge_lines,
            report.noise_areas,
            report.assets_preserved
        );
        for warning in report.warnings {
            eprintln!("提示：[{}]", warning);
        }
        println!(
            "输出文件：{}（谱面路径：{}）",
            args.output.display(),
            report.output_chart_path
        );
    } else {
        let text = String::from_utf8(input)?;
        let parsed = parse_document(&text)?;
        let input_format = parsed.format;
        let (detected_notes, detected_lines, detected_noise) =
            chart_counts(&parsed.document, input_format);
        if zip_output {
            let mut files = std::collections::BTreeMap::new();
            let chart_stem = args
                .input
                .file_stem()
                .and_then(|s| s.to_str())
                .unwrap_or("chart")
                .to_owned();
            let (json, report) = convert_json_with_mode(&text, mode)?;
            let chart_name = format!("{chart_stem}.json");
            files.insert(chart_name.clone(), json.into_bytes());
            files.insert("info.txt".into(), format!("#\nName: {chart_stem}\nPath: 00000000\nSong: \nPicture: \nChart: {chart_name}\nLevel: \nComposer: \nCharter: \nIllustrator: \n").into_bytes());
            fs::write(&args.output, archive::write_zip(&files)?)?;
            println!(
                "输入格式：{:?}；输出格式：{}；模式：{:?}；Note：{}；判定线：{}；噪域：{}",
                input_format,
                if input_format == ChartFormat::OfficialV3 {
                    "RPEN"
                } else {
                    format_name(input_format)
                },
                args.mode,
                if input_format == ChartFormat::OfficialV3 {
                    report.notes
                } else {
                    detected_notes
                },
                if input_format == ChartFormat::OfficialV3 {
                    report.judge_lines
                } else {
                    detected_lines
                },
                if input_format == ChartFormat::OfficialV3 {
                    report.noise_areas
                } else {
                    detected_noise
                }
            );
            println!("输出文件：{}", args.output.display());
            print_diagnostics(report.diagnostics);
        } else {
            let (out, report) = convert_json_with_mode(&text, mode)?;
            fs::write(&args.output, out)?;
            println!(
                "输入格式：{:?}；输出格式：{}；模式：{:?}；Note：{}；判定线：{}；噪域：{}",
                input_format,
                if input_format == ChartFormat::OfficialV3 {
                    "RPEN"
                } else {
                    format_name(input_format)
                },
                args.mode,
                if input_format == ChartFormat::OfficialV3 {
                    report.notes
                } else {
                    detected_notes
                },
                if input_format == ChartFormat::OfficialV3 {
                    report.judge_lines
                } else {
                    detected_lines
                },
                if input_format == ChartFormat::OfficialV3 {
                    report.noise_areas
                } else {
                    detected_noise
                }
            );
            println!("输出文件：{}", args.output.display());
            print_diagnostics(report.diagnostics);
        }
    }
    Ok(())
}

fn format_name(format: ChartFormat) -> &'static str {
    match format {
        ChartFormat::OfficialV3 => "Phigros v3",
        ChartFormat::Rpe => "RPE",
        ChartFormat::Rpen => "RPEN",
    }
}

fn chart_counts(chart: &serde_json::Value, format: ChartFormat) -> (usize, usize, usize) {
    let lines = chart["judgeLineList"].as_array();
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
    let noise = chart["blockAreaList"].as_array().map_or(0, Vec::len);
    (notes, line_count, noise)
}

fn print_diagnostics(diagnostics: Vec<phigros_rpen_converter::Diagnostic>) {
    for diagnostic in diagnostics {
        eprintln!(
            "提示 [{}] {}: {}",
            match diagnostic.severity {
                phigros_rpen_converter::converter::Severity::Warning => "警告",
                phigros_rpen_converter::converter::Severity::Info => "信息",
            },
            diagnostic.path,
            diagnostic.message
        );
    }
}

fn main() {
    if let Err(e) = run() {
        eprintln!("错误: {e}");
        std::process::exit(
            if matches!(e.downcast_ref::<Error>(), Some(Error::Unsupported(_))) {
                2
            } else {
                1
            },
        );
    }
}
