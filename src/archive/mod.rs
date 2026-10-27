use crate::{
    convert_json_with_mode, detect_format, parser::ChartFormat, Error, OutputMode, Result,
};
use serde_json::Value;
use std::{
    collections::{BTreeMap, HashSet},
    io::{Cursor, Read, Write},
};
use zip::{
    write::{SimpleFileOptions, ZipWriter},
    CompressionMethod, ZipArchive,
};

const MAX_ARCHIVE_FILES: usize = 20_000;
const MAX_ARCHIVE_BYTES: u64 = 1024 * 1024 * 1024;
const MAX_ENTRY_BYTES: u64 = 512 * 1024 * 1024;

fn normalized_path(name: &str) -> Result<String> {
    let path = name.replace('\\', "/");
    if path.is_empty() || path.starts_with('/') || path.contains('\0') {
        return Err(Error::Archive(format!("unsafe archive path: {name}")));
    }
    let mut parts = Vec::new();
    for part in path.split('/') {
        match part {
            "" | "." => continue,
            ".." => return Err(Error::Archive(format!("unsafe archive path: {name}"))),
            _ if parts.is_empty() && part.contains(':') => {
                return Err(Error::Archive(format!("unsafe archive path: {name}")))
            }
            _ => parts.push(part),
        }
    }
    if parts.is_empty() {
        return Err(Error::Archive(format!("unsafe archive path: {name}")));
    }
    Ok(parts.join("/"))
}

fn resolve_metadata_path(metadata_name: &str, reference: &str) -> Option<String> {
    let reference = reference.replace('\\', "/");
    if reference.starts_with('/') || reference.contains('\0') {
        return None;
    }
    let mut parts = metadata_name
        .replace('\\', "/")
        .rsplit_once('/')
        .map(|(directory, _)| {
            directory
                .split('/')
                .filter(|part| !part.is_empty() && *part != ".")
                .map(str::to_owned)
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    for part in reference.split('/') {
        match part {
            "" | "." => continue,
            ".." => {
                parts.pop()?;
            }
            _ if parts.is_empty() && part.contains(':') => return None,
            _ => parts.push(part.to_owned()),
        }
    }
    (!parts.is_empty()).then(|| parts.join("/"))
}

pub fn read_zip(bytes: &[u8]) -> Result<BTreeMap<String, Vec<u8>>> {
    let mut zip = ZipArchive::new(Cursor::new(bytes)).map_err(|e| Error::Archive(e.to_string()))?;
    if zip.len() > MAX_ARCHIVE_FILES {
        return Err(Error::Archive(format!(
            "archive contains {} entries (limit {MAX_ARCHIVE_FILES})",
            zip.len()
        )));
    }
    let mut files = BTreeMap::new();
    let mut paths = HashSet::new();
    let mut total = 0u64;
    for i in 0..zip.len() {
        let mut file = zip.by_index(i).map_err(|e| Error::Archive(e.to_string()))?;
        let name = normalized_path(file.name())?;
        if !paths.insert(name.to_lowercase()) {
            return Err(Error::Archive(format!(
                "duplicate normalized archive path: {name}"
            )));
        }
        if file.is_dir() {
            continue;
        }
        if file.size() > MAX_ENTRY_BYTES {
            return Err(Error::Archive(format!(
                "archive entry {name} exceeds the {MAX_ENTRY_BYTES}-byte limit"
            )));
        }
        total = total
            .checked_add(file.size())
            .ok_or_else(|| Error::Archive("archive uncompressed size overflow".into()))?;
        if total > MAX_ARCHIVE_BYTES {
            return Err(Error::Archive(format!(
                "archive uncompressed data exceeds the {MAX_ARCHIVE_BYTES}-byte limit"
            )));
        }
        let mut data = Vec::with_capacity(file.size() as usize);
        file.read_to_end(&mut data)?;
        files.insert(name, data);
    }
    Ok(files)
}

pub fn write_zip(files: &BTreeMap<String, Vec<u8>>) -> Result<Vec<u8>> {
    let mut writer = ZipWriter::new(Cursor::new(Vec::new()));
    let options = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
    for (name, data) in files {
        normalized_path(name)?;
        writer
            .start_file(name, options)
            .map_err(|e| Error::Archive(e.to_string()))?;
        writer.write_all(data)?;
    }
    writer
        .finish()
        .map(|c| c.into_inner())
        .map_err(|e| Error::Archive(e.to_string()))
}

#[derive(Clone, Debug)]
pub struct ArchiveReport {
    pub chart_path: String,
    pub output_chart_path: String,
    pub format: ChartFormat,
    pub notes: usize,
    pub judge_lines: usize,
    pub noise_areas: usize,
    pub assets_preserved: usize,
    pub warnings: Vec<String>,
}

fn unquote(value: &str) -> &str {
    let value = value.trim();
    if value.len() >= 2 {
        let first = value.as_bytes()[0];
        if (first == b'\'' || first == b'"') && value.as_bytes().last() == Some(&first) {
            return &value[1..value.len() - 1];
        }
    }
    value
}

fn metadata_chart_references(files: &BTreeMap<String, Vec<u8>>) -> Vec<String> {
    let mut references = Vec::new();
    for (name, bytes) in files {
        let basename = name.rsplit('/').next().unwrap_or(name);
        let is_info_txt = basename.eq_ignore_ascii_case("info.txt");
        let is_info_yml =
            basename.eq_ignore_ascii_case("info.yml") || basename.eq_ignore_ascii_case("info.yaml");
        if !is_info_txt && !is_info_yml {
            continue;
        }
        let Ok(contents) = std::str::from_utf8(bytes) else {
            continue;
        };
        for line in contents.lines() {
            let Some((key, value)) = line.split_once(':') else {
                continue;
            };
            if (is_info_txt && key.trim().eq_ignore_ascii_case("Chart"))
                || (is_info_yml && key.trim().eq_ignore_ascii_case("chart"))
            {
                let value = unquote(value.trim());
                if value.is_empty() {
                    continue;
                }
                if let Some(path) = resolve_metadata_path(name, value) {
                    references.push(path);
                }
            }
        }
    }
    references.sort();
    references.dedup();
    references
}

fn chart_metadata(files: &BTreeMap<String, Vec<u8>>, chart_path: &str) -> Value {
    // Phigros v3 JSON has no chart-level META object. PEZ metadata is the
    // authoritative source for those RPEN fields; Name is the human-facing
    // title, while Path is only a fallback identifier.
    let mut fields = BTreeMap::<String, String>::new();
    for (name, bytes) in files {
        let basename = name.rsplit('/').next().unwrap_or(name);
        let is_txt = basename.eq_ignore_ascii_case("info.txt");
        let is_yml =
            basename.eq_ignore_ascii_case("info.yml") || basename.eq_ignore_ascii_case("info.yaml");
        if !is_txt && !is_yml {
            continue;
        }
        let Ok(contents) = std::str::from_utf8(bytes) else {
            continue;
        };
        for line in contents.lines() {
            let Some((key, value)) = line.split_once(':') else {
                continue;
            };
            let key = key.trim().to_ascii_lowercase();
            let value = unquote(value.trim()).trim();
            if value.is_empty() || value.eq_ignore_ascii_case("null") {
                continue;
            }
            // Prefer the legacy info.txt keys when both formats are present;
            // the YAML values often contain optional downloader-specific data.
            let mapped = if is_txt {
                match key.as_str() {
                    "name" => "name",
                    "path" => "pathid",
                    "song" => "song",
                    "picture" => "background",
                    "level" => "level",
                    "composer" => "composer",
                    "charter" => "charter",
                    "illustrator" | "illustration" => "illustration",
                    _ => continue,
                }
            } else {
                match key.as_str() {
                    "name" => "name",
                    "music" => "song",
                    "illustration" => "background",
                    "level" => "level",
                    "composer" => "composer",
                    "charter" => "charter",
                    "illustrator" => "illustration",
                    _ => continue,
                }
            };
            if is_txt || !fields.contains_key(mapped) {
                fields.insert(mapped.to_owned(), value.to_owned());
            }
        }
    }
    let path_id = fields.remove("pathid");
    fields.entry("name".into()).or_insert_with(|| {
        if let Some(path_id) = path_id {
            return path_id;
        }
        chart_path
            .rsplit('/')
            .next()
            .unwrap_or(chart_path)
            .rsplit_once('.')
            .map(|(stem, _)| stem)
            .unwrap_or(chart_path)
            .to_owned()
    });
    serde_json::to_value(fields).unwrap_or(Value::Null)
}

fn apply_pez_metadata(chart: &mut Value, files: &BTreeMap<String, Vec<u8>>, chart_path: &str) {
    let metadata = chart_metadata(files, chart_path);
    let Some(meta) = chart.get_mut("META").and_then(Value::as_object_mut) else {
        return;
    };
    if let Some(fields) = metadata.as_object() {
        for (key, value) in fields {
            meta.insert(key.clone(), value.clone());
        }
    }
}

fn choose_candidate(
    candidates: &[(String, ChartFormat, Value)],
    files: &BTreeMap<String, Vec<u8>>,
) -> Result<usize> {
    if candidates.is_empty() {
        let json_paths = files
            .keys()
            .filter(|name| name.to_ascii_lowercase().ends_with(".json"))
            .cloned()
            .collect::<Vec<_>>();
        return Err(Error::Unsupported(format!(
            "ZIP/PEZ contains no recognized chart JSON; JSON entries found: {}",
            if json_paths.is_empty() {
                "(none)".to_owned()
            } else {
                json_paths.join(", ")
            }
        )));
    }
    let paths = metadata_chart_references(files);
    let matches = candidates
        .iter()
        .enumerate()
        .filter(|(_, (name, _, _))| paths.iter().any(|path| path.eq_ignore_ascii_case(name)))
        .map(|(index, _)| index)
        .collect::<Vec<_>>();
    match matches.as_slice() {
        [index] => Ok(*index),
        [] if candidates.len() == 1 && paths.is_empty() => Ok(0),
        [] if candidates.len() == 1 => Err(Error::Unsupported(format!(
            "chart metadata does not match the only recognized JSON candidate {}",
            candidates[0].0
        ))),
        [] => Err(Error::Unsupported(format!(
            "ZIP/PEZ contains multiple chart JSON candidates and metadata does not select one: {}",
            candidates
                .iter()
                .map(|(name, _, _)| name.as_str())
                .collect::<Vec<_>>()
                .join(", ")
        ))),
        _ => Err(Error::Unsupported(format!(
            "chart metadata is ambiguous; it selects multiple JSON candidates: {}",
            matches
                .iter()
                .map(|&i| candidates[i].0.as_str())
                .collect::<Vec<_>>()
                .join(", ")
        ))),
    }
}

fn chart_candidates(files: &BTreeMap<String, Vec<u8>>) -> Vec<(String, ChartFormat, Value)> {
    let mut candidates = Vec::<(String, ChartFormat, Value)>::new();
    for (name, data) in files {
        if !name.to_ascii_lowercase().ends_with(".json") {
            continue;
        }
        if let Ok(text) = std::str::from_utf8(data) {
            if let Ok(value) = serde_json::from_str::<Value>(text) {
                if let Ok(format) = detect_format(&value) {
                    candidates.push((name.clone(), format, value));
                }
            }
        }
    }
    candidates
}

/// Identify the chart stored in a PEZ/ZIP archive without converting or
/// rewriting it. This is useful for render-only tools that must reject
/// official charts instead of silently converting them.
pub fn inspect_archive(bytes: &[u8]) -> Result<ArchiveReport> {
    let files = read_zip(bytes)?;
    let candidates = chart_candidates(&files);
    let selected = choose_candidate(&candidates, &files)?;
    let (chart_path, format, chart) = &candidates[selected];
    Ok(ArchiveReport {
        chart_path: chart_path.clone(),
        output_chart_path: chart_path.clone(),
        format: *format,
        notes: note_count(chart, *format),
        judge_lines: chart["judgeLineList"].as_array().map_or(0, Vec::len),
        noise_areas: chart["blockAreaList"].as_array().map_or(0, Vec::len),
        assets_preserved: files.len().saturating_sub(1),
        warnings: Vec::new(),
    })
}

fn note_count(chart: &Value, format: ChartFormat) -> usize {
    chart["judgeLineList"].as_array().map_or(0, |lines| {
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
    })
}

fn archive_title_path(chart_path: &str, title: &str, files: &BTreeMap<String, Vec<u8>>) -> String {
    let directory = chart_path.rsplit_once('/').map(|(dir, _)| dir);
    let old_name = chart_path.rsplit('/').next().unwrap_or(chart_path);
    let fallback = old_name.strip_suffix(".json").unwrap_or(old_name);
    let mut stem = title
        .chars()
        .map(|ch| {
            if ch.is_control() || "<>:\"/\\|?*".contains(ch) {
                '_'
            } else {
                ch
            }
        })
        .collect::<String>()
        .trim_matches([' ', '.'])
        .to_owned();
    if stem.is_empty() {
        stem = fallback.to_owned();
    }
    let path_for = |stem: &str| match directory {
        Some(dir) => format!("{dir}/{stem}.json"),
        None => format!("{stem}.json"),
    };
    let candidate = path_for(&stem);
    if candidate.eq_ignore_ascii_case(chart_path)
        || !files
            .keys()
            .any(|path| path.eq_ignore_ascii_case(&candidate))
    {
        return candidate;
    }
    for suffix in 2..=10_000 {
        let candidate = path_for(&format!("{stem} ({suffix})"));
        if !files
            .keys()
            .any(|path| path.eq_ignore_ascii_case(&candidate))
        {
            return candidate;
        }
    }
    path_for(fallback)
}

fn update_chart_references(
    files: &BTreeMap<String, Vec<u8>>,
    chart_path: &str,
    output_chart_path: &str,
) -> BTreeMap<String, Vec<u8>> {
    let mut updated = BTreeMap::new();
    let new_basename = output_chart_path
        .rsplit('/')
        .next()
        .unwrap_or(output_chart_path);
    for (name, bytes) in files {
        let basename = name.rsplit('/').next().unwrap_or(name);
        let is_txt = basename.eq_ignore_ascii_case("info.txt");
        let is_yml =
            basename.eq_ignore_ascii_case("info.yml") || basename.eq_ignore_ascii_case("info.yaml");
        if !is_txt && !is_yml {
            continue;
        }
        let Ok(content) = std::str::from_utf8(bytes) else {
            continue;
        };
        let mut changed = false;
        let mut result = String::with_capacity(content.len());
        for line in content.split_inclusive('\n') {
            let line_without_newline = line.strip_suffix('\n').unwrap_or(line);
            let ending = if line.ends_with('\n') { "\n" } else { "" };
            let Some((key, raw_value)) = line_without_newline.split_once(':') else {
                result.push_str(line);
                continue;
            };
            let is_chart_key = (is_txt && key.trim().eq_ignore_ascii_case("Chart"))
                || (is_yml && key.trim().eq_ignore_ascii_case("chart"));
            let value = raw_value.trim();
            let reference = unquote(value);
            let resolves_to_chart = is_chart_key
                && resolve_metadata_path(name, reference)
                    .is_some_and(|path| path.eq_ignore_ascii_case(chart_path));
            if !resolves_to_chart {
                result.push_str(line);
                continue;
            }
            let prefix_len = raw_value.len() - raw_value.trim_start().len();
            let prefix = &raw_value[..prefix_len];
            let suffix_space_len = raw_value.len() - raw_value.trim_end().len();
            let suffix_space = &raw_value[raw_value.len() - suffix_space_len..];
            let replacement = if value.starts_with('\'') || value.starts_with('"') {
                format!(
                    "{}{}{}",
                    &value[..1],
                    new_basename,
                    &value[value.len() - 1..]
                )
            } else {
                new_basename.to_owned()
            };
            result.push_str(key);
            result.push(':');
            result.push_str(prefix);
            result.push_str(&replacement);
            result.push_str(suffix_space);
            result.push_str(ending);
            changed = true;
        }
        if changed {
            updated.insert(name.clone(), result.into_bytes());
        }
    }
    updated
}

fn repack_replacing_chart(
    input: &[u8],
    chart_path: &str,
    output_chart_path: &str,
    expected_chart: &[u8],
    new_chart: &[u8],
    metadata_updates: &BTreeMap<String, Vec<u8>>,
) -> Result<(Vec<u8>, Vec<String>)> {
    let mut source =
        ZipArchive::new(Cursor::new(input)).map_err(|e| Error::Archive(e.to_string()))?;
    let mut writer = ZipWriter::new(Cursor::new(Vec::new()));
    writer.set_raw_comment(source.comment().to_vec().into_boxed_slice());
    let mut replaced = false;
    let mut warnings = Vec::new();
    for index in 0..source.len() {
        let file = source
            .by_index(index)
            .map_err(|e| Error::Archive(e.to_string()))?;
        let raw_name = file.name().to_owned();
        let name = normalized_path(&raw_name)?;
        if name != chart_path {
            if let Some(replacement) = metadata_updates.get(&name) {
                let compression = file.compression();
                let modified = file.last_modified().unwrap_or_default();
                let permissions = file.unix_mode();
                let comment_present = !file.comment().is_empty();
                let mut options = SimpleFileOptions::default()
                    .compression_method(compression)
                    .last_modified_time(modified);
                if let Some(mode) = permissions {
                    options = options.unix_permissions(mode);
                }
                writer
                    .start_file(&raw_name, options)
                    .map_err(|e| Error::Archive(e.to_string()))?;
                writer.write_all(replacement)?;
                if comment_present {
                    warnings.push(format!("rewritten metadata entry {name} had a per-file ZIP comment which zip 2.6 cannot set on new entries"));
                }
                continue;
            }
            writer
                .raw_copy_file(file)
                .map_err(|e| Error::Archive(e.to_string()))?;
            continue;
        }
        if replaced {
            return Err(Error::Archive(format!(
                "multiple ZIP entries normalize to chart path {chart_path}"
            )));
        }
        replaced = true;
        let compression = file.compression();
        let modified = file.last_modified().unwrap_or_default();
        let permissions = file.unix_mode();
        let comment_present = !file.comment().is_empty();
        let mut original = Vec::new();
        let mut file = file;
        file.read_to_end(&mut original)?;
        if original != expected_chart {
            return Err(Error::Archive(format!(
                "chart entry changed while converting: {chart_path}"
            )));
        }
        let mut options = SimpleFileOptions::default()
            .compression_method(compression)
            .last_modified_time(modified);
        if let Some(mode) = permissions {
            options = options.unix_permissions(mode);
        }
        writer
            .start_file(output_chart_path, options)
            .map_err(|e| Error::Archive(e.to_string()))?;
        writer.write_all(new_chart)?;
        if comment_present {
            warnings.push(format!(
                "rewritten chart entry {chart_path} had a per-file ZIP comment which zip 2.6 cannot set on new entries"
            ));
        }
    }
    if !replaced {
        return Err(Error::Archive(format!(
            "selected chart entry disappeared: {chart_path}"
        )));
    }
    let output = writer
        .finish()
        .map(|cursor| cursor.into_inner())
        .map_err(|e| Error::Archive(e.to_string()))?;
    Ok((output, warnings))
}

pub fn convert_archive(bytes: &[u8]) -> Result<(Vec<u8>, ArchiveReport)> {
    convert_archive_with_mode(bytes, OutputMode::Standard)
}

pub fn convert_archive_with_mode(
    bytes: &[u8],
    mode: OutputMode,
) -> Result<(Vec<u8>, ArchiveReport)> {
    let files = read_zip(bytes)?;
    let candidates = chart_candidates(&files);
    let selected = choose_candidate(&candidates, &files)?;
    let (chart_path, format, source_chart) = &candidates[selected];
    let source_text = std::str::from_utf8(&files[chart_path])
        .map_err(|e| Error::Unsupported(format!("chart JSON is not UTF-8: {e}")))?;
    let (output_text, conversion_report) = convert_json_with_mode(source_text, mode)?;
    let mut output_chart: Value = serde_json::from_str(&output_text)?;
    if *format == ChartFormat::OfficialV3 {
        apply_pez_metadata(&mut output_chart, &files, chart_path);
    }
    let output_bytes = if *format == ChartFormat::OfficialV3 {
        serde_json::to_vec(&output_chart)?
    } else {
        output_text.into_bytes()
    };
    let output_format = detect_format(&output_chart)?;
    if *format == ChartFormat::OfficialV3 && output_format != ChartFormat::Rpen {
        return Err(Error::Unsupported(format!(
            "converted chart did not validate as RPEN (detected {output_format:?})"
        )));
    }
    // RPEN's official-source import intentionally derives META.name from the
    // chart JSON filename when the legacy source marker is present. Use the
    // human title as the archived chart filename so archive mode displays the
    // same title as standard mode after RPEN import.
    let output_chart_path = if mode == OutputMode::Archive && *format == ChartFormat::OfficialV3 {
        archive_title_path(
            chart_path,
            output_chart["META"]["name"].as_str().unwrap_or_default(),
            &files,
        )
    } else {
        chart_path.clone()
    };
    let metadata_updates = if output_chart_path != *chart_path {
        update_chart_references(&files, chart_path, &output_chart_path)
    } else {
        BTreeMap::new()
    };
    let (packed, mut warnings) = repack_replacing_chart(
        bytes,
        chart_path,
        &output_chart_path,
        &files[chart_path],
        &output_bytes,
        &metadata_updates,
    )?;

    // Re-read every entry. This verifies the output ZIP's CRCs and confirms that
    // all non-chart assets are byte-identical to the input archive.
    let roundtrip = read_zip(&packed)?;
    if roundtrip.len() != files.len() {
        return Err(Error::Archive(format!(
            "archive entry count changed: {} -> {}",
            files.len(),
            roundtrip.len()
        )));
    }
    let mut expected_files = files.clone();
    expected_files.remove(chart_path);
    expected_files.insert(output_chart_path.clone(), output_bytes.clone());
    expected_files.extend(metadata_updates);
    for (name, expected) in &expected_files {
        if roundtrip.get(name).map(Vec::as_slice) != Some(expected.as_slice()) {
            return Err(Error::Archive(format!(
                "archive validation failed for entry {name}"
            )));
        }
    }
    let validated_chart: Value = serde_json::from_slice(&roundtrip[&output_chart_path])?;
    if detect_format(&validated_chart)? != output_format {
        return Err(Error::Archive(
            "repacked chart JSON format changed during validation".into(),
        ));
    }
    warnings.extend(
        conversion_report
            .diagnostics
            .iter()
            .map(|diagnostic| {
                format!(
                    "{:?} {}: {}",
                    diagnostic.severity, diagnostic.path, diagnostic.message
                )
            })
            .collect::<Vec<_>>(),
    );
    if *format != ChartFormat::OfficialV3 {
        warnings.push("Input chart was already RPE/RPEN and was not converted.".into());
    }
    let report = ArchiveReport {
        chart_path: chart_path.clone(),
        output_chart_path,
        format: *format,
        notes: if *format == ChartFormat::OfficialV3 {
            conversion_report.notes
        } else {
            note_count(source_chart, *format)
        },
        judge_lines: source_chart["judgeLineList"].as_array().map_or(0, Vec::len),
        noise_areas: source_chart["blockAreaList"].as_array().map_or(0, Vec::len),
        assets_preserved: files.len().saturating_sub(1),
        warnings,
    };
    Ok((packed, report))
}
