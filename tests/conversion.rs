use phigros_rpen_converter::{
    archive, convert_json, convert_official, convert_official_with_mode, detect_format,
    parser::ChartFormat, OutputMode,
};
use serde_json::{json, Value};
use std::{collections::BTreeMap, fs};

fn approximately_equal(a: &Value, b: &Value, tolerance: f64) -> bool {
    match (a, b) {
        (Value::Number(x), Value::Number(y)) => match (x.as_f64(), y.as_f64()) {
            (Some(x), Some(y)) => (x - y).abs() <= tolerance,
            _ => false,
        },
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len()
                && x.iter()
                    .zip(y)
                    .all(|(x, y)| approximately_equal(x, y, tolerance))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter().all(|(k, v)| {
                    y.get(k)
                        .is_some_and(|other| approximately_equal(v, other, tolerance))
                })
        }
        _ => a == b,
    }
}

fn info_chart(files: &BTreeMap<String, Vec<u8>>) -> String {
    let info = files
        .iter()
        .find(|(name, _)| name.rsplit('/').next().unwrap_or(name) == "info.txt")
        .map(|(_, data)| String::from_utf8_lossy(data).into_owned())
        .unwrap();
    info.lines()
        .find_map(|line| {
            let (key, value) = line.split_once(':')?;
            key.trim()
                .eq_ignore_ascii_case("chart")
                .then(|| value.trim().to_owned())
        })
        .unwrap()
}

fn assert_reference_semantics(converted: &Value, reference: &Value) {
    let num = |value: &Value| value.as_f64().unwrap();
    let beat = |value: &Value| {
        let parts = value.as_array().unwrap();
        num(&parts[0]) + num(&parts[1]) / num(&parts[2])
    };
    let notes = |chart: &Value| {
        let mut result = chart["judgeLineList"]
            .as_array()
            .unwrap()
            .iter()
            .enumerate()
            .flat_map(|(line_index, line)| {
                line["notes"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(move |note| (line_index, note))
            })
            .map(|(line_index, note)| {
                let q = |value: f64, scale: f64| (value * scale).round() as i64;
                [
                    line_index as i64,
                    q(num(&note["type"]), 1.0),
                    q(num(&note["above"]), 1.0),
                    q(beat(&note["startTime"]), 1_000_000.0),
                    q(beat(&note["endTime"]), 1_000_000.0),
                    q(num(&note["positionX"]), 100.0),
                    q(num(&note["speed"]), 1_000_000.0),
                ]
            })
            .collect::<Vec<_>>();
        result.sort();
        result
    };
    assert_eq!(notes(converted), notes(reference), "note semantics");
    assert!(approximately_equal(
        &converted["BPMList"],
        &reference["BPMList"],
        1e-9
    ));
    assert_eq!(
        converted["judgeLineList"].as_array().unwrap().len(),
        reference["judgeLineList"].as_array().unwrap().len()
    );
    for (actual, expected) in converted["judgeLineList"]
        .as_array()
        .unwrap()
        .iter()
        .zip(reference["judgeLineList"].as_array().unwrap())
    {
        assert!(approximately_equal(
            &actual["bpmfactor"],
            &expected["bpmfactor"],
            1e-9
        ));
        for key in [
            "speedEvents",
            "alphaEvents",
            "moveXEvents",
            "moveYEvents",
            "rotateEvents",
        ] {
            assert!(
                approximately_equal(
                    &actual["eventLayers"][0][key],
                    &expected["eventLayers"][0][key],
                    1e-6
                ),
                "judge-line event channel {key}"
            );
        }
    }
    assert_eq!(
        converted["blockAreaList"].as_array().unwrap().len(),
        reference["blockAreaList"].as_array().unwrap().len()
    );
    for (actual, expected) in converted["blockAreaList"]
        .as_array()
        .unwrap()
        .iter()
        .zip(reference["blockAreaList"].as_array().unwrap())
    {
        for key in [
            "Name",
            "bottomLeft",
            "topRight",
            "appearTime",
            "disappearTime",
            "activeIntervals",
            "isInvert",
            "moveXEvents",
            "moveYEvents",
            "scaleXEvents",
            "scaleYEvents",
            "rotateEvents",
        ] {
            assert!(
                approximately_equal(&actual[key], &expected[key], 1e-6),
                "noise area field {key}"
            );
        }
    }
}

fn zip_member_signatures(
    bytes: &[u8],
) -> BTreeMap<String, (zip::CompressionMethod, u64, u64, u32)> {
    let mut zip = zip::ZipArchive::new(std::io::Cursor::new(bytes)).unwrap();
    (0..zip.len())
        .map(|index| {
            let file = zip.by_index(index).unwrap();
            (
                file.name().replace('\\', "/"),
                (
                    file.compression(),
                    file.size(),
                    file.compressed_size(),
                    file.crc32(),
                ),
            )
        })
        .collect()
}

fn official() -> Value {
    json!({"formatVersion":3,"offset":0.0,"judgeLineList":[{"bpm":120.0,"judgeLineMoveEvents":[],"judgeLineRotateEvents":[],"judgeLineDisappearEvents":[],"speedEvents":[],"notesAbove":[{"type":1,"time":32,"positionX":1.0,"speed":1.0,"holdTime":0.0}],"notesBelow":[{"type":3,"time":64,"positionX":-1.0,"speed":1.0,"holdTime":32.0}]}],"blockAreaList":[{"bottomLeftPercentage":{"x":0.2,"y":0.3},"topRightPercentage":{"x":0.8,"y":0.7},"appearTime":0.0,"disappearTime":2.0,"enableTime":0.5,"disableTime":1.5,"isSubtract":false,"moveEvents":[],"scaleEvents":[],"rotateEvents":[],"extension":{"kept":true}}],"unknownTopLevel":{"sentinel":7}})
}

#[test]
fn official_mapping_and_source_preservation() {
    let source = official();
    let (out, report) = convert_official_with_mode(&source, OutputMode::Archive).unwrap();
    assert_eq!(report.notes, 2);
    assert_eq!(report.judge_lines, 1);
    assert_eq!(report.noise_areas, 1);
    assert_eq!(out["META"]["RPEVersion"], 220);
    assert_eq!(out["META"]["offset"].as_i64(), Some(0));
    assert_eq!(out["BPMList"][0]["bpm"], 120.0);
    assert_eq!(
        out["judgeLineList"][0]["notes"].as_array().unwrap().len(),
        2
    );
    assert_eq!(out["judgeLineList"][0]["notes"][0]["type"], 1);
    assert_eq!(out["judgeLineList"][0]["notes"][1]["type"], 2);
    assert_eq!(
        out["judgeLineList"][0]["notes"][1]["endTime"],
        json!([3, 0, 1])
    );
    assert_eq!(
        out["blockAreaList"][0]["topRight"],
        json!({"x":405.0,"y":180.0})
    );
    assert_eq!(
        out["rpeNextLegacySource"]["document"]["unknownTopLevel"]["sentinel"],
        7
    );
    assert_eq!(
        out["rpeNextLegacySource"]["document"]["blockAreaList"][0]["extension"]["kept"],
        true
    );
}

#[test]
fn standard_and_archive_modes_have_identical_rpen_semantics() {
    let source = official();
    let (standard, _) = convert_official(&source).unwrap();
    let (mut archived, _) = convert_official_with_mode(&source, OutputMode::Archive).unwrap();
    assert!(standard.get("rpeNextLegacySource").is_none());
    assert!(archived.get("rpeNextLegacySource").is_some());
    archived
        .as_object_mut()
        .unwrap()
        .remove("rpeNextLegacySource");
    assert_eq!(standard, archived);
}

#[test]
fn format_detection_and_passthrough() {
    let official = serde_json::to_string(&official()).unwrap();
    assert_eq!(
        detect_format(&serde_json::from_str(&official).unwrap()).unwrap(),
        ChartFormat::OfficialV3
    );
    let (converted, _) = convert_json(&official).unwrap();
    let parsed = phigros_rpen_converter::parse_document(&converted).unwrap();
    assert_eq!(parsed.format, ChartFormat::Rpen);
    let native = r#"{"META":{"RPEVersion":170},"BPMList":[{"bpm":120,"startTime":[0,0,1]}],"judgeLineList":[],"judgeLineGroup":["Default"]}"#;
    assert_eq!(convert_json(native).unwrap().0, native);
}

#[test]
fn zip_conversion_keeps_non_chart_assets() {
    let mut input = BTreeMap::new();
    input.insert(
        "Song/chart.json".into(),
        serde_json::to_vec(&official()).unwrap(),
    );
    input.insert(
        "Song/info.txt".into(),
        b"#\nName: Sample\nChart: chart.json\nSong: music.ogg\nPicture: bg.png\n".to_vec(),
    );
    input.insert("Song/music.ogg".into(), vec![1, 2, 3, 4]);
    input.insert("Song/bg.png".into(), vec![5, 6]);
    let encoded = archive::write_zip(&input).unwrap();
    let (out, report) = archive::convert_archive(&encoded).unwrap();
    let files = archive::read_zip(&out).unwrap();
    assert_eq!(report.output_chart_path, "Song/chart.json");
    assert_eq!(files["Song/music.ogg"], [1, 2, 3, 4]);
    assert_eq!(files["Song/bg.png"], [5, 6]);
    let chart: Value = serde_json::from_slice(&files["Song/chart.json"]).unwrap();
    assert_eq!(chart["blockAreaList"].as_array().unwrap().len(), 1);
    assert_eq!(
        files["Song/info.txt"], input["Song/info.txt"],
        "metadata should remain byte-identical when the chart path is unchanged"
    );
    let (archived_zip, archive_report) =
        archive::convert_archive_with_mode(&encoded, OutputMode::Archive).unwrap();
    let archived_files = archive::read_zip(&archived_zip).unwrap();
    assert_eq!(archive_report.output_chart_path, "Song/Sample.json");
    assert_eq!(info_chart(&archived_files), "Sample.json");
    let archived_chart: Value =
        serde_json::from_slice(&archived_files["Song/Sample.json"]).unwrap();
    assert!(archived_chart.get("rpeNextLegacySource").is_some());
    let mut archived_runtime = archived_chart;
    archived_runtime
        .as_object_mut()
        .unwrap()
        .remove("rpeNextLegacySource");
    assert_eq!(chart, archived_runtime);
}

#[test]
fn supplied_official_and_rpen_reference_semantics() {
    let official_path = std::env::var("PHIGROS_SAMPLE_OFFICIAL")
        .unwrap_or_else(|_| "D:/Users/lemon/Downloads/AT.json".into());
    let reference_path = std::env::var("PHIGROS_SAMPLE_RPEN")
        .unwrap_or_else(|_| "D:/Users/lemon/Downloads/RPENAT.json".into());
    if !std::path::Path::new(&official_path).exists()
        || !std::path::Path::new(&reference_path).exists()
    {
        eprintln!("sample comparison skipped; set PHIGROS_SAMPLE_OFFICIAL and PHIGROS_SAMPLE_RPEN");
        return;
    }
    let source: Value = serde_json::from_slice(&fs::read(official_path).unwrap()).unwrap();
    let reference: Value = serde_json::from_slice(&fs::read(reference_path).unwrap()).unwrap();
    let (actual, report) = convert_official(&source).unwrap();
    let source_note_count: usize = source["judgeLineList"]
        .as_array()
        .unwrap()
        .iter()
        .map(|line| {
            line["notesAbove"].as_array().map_or(0, Vec::len)
                + line["notesBelow"].as_array().map_or(0, Vec::len)
        })
        .sum();
    assert_eq!(report.notes, source_note_count);
    assert_eq!(
        report.judge_lines,
        source["judgeLineList"].as_array().unwrap().len()
    );
    assert_eq!(
        report.noise_areas,
        source["blockAreaList"].as_array().unwrap().len()
    );
    assert_eq!(
        actual["judgeLineList"].as_array().unwrap().len(),
        reference["judgeLineList"].as_array().unwrap().len()
    );
    assert_eq!(
        report.notes,
        reference["judgeLineList"]
            .as_array()
            .unwrap()
            .iter()
            .map(|line| line["notes"].as_array().unwrap().len())
            .sum::<usize>()
    );
    let notes = |chart: &Value| {
        let num = |v: &Value| v.as_f64().unwrap();
        let beat_value = |v: &Value| {
            let b = v.as_array().unwrap();
            num(&b[0]) + num(&b[1]) / num(&b[2])
        };
        let mut notes = chart["judgeLineList"]
            .as_array()
            .unwrap()
            .iter()
            .enumerate()
            .flat_map(|(line_index, line)| {
                line["notes"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(move |note| (line_index, note))
            })
            .map(|n| {
                let (line_index, note) = n;
                let q = |x: f64, scale: f64| (x * scale).round() as i64;
                [
                    line_index as i64,
                    q(num(&note["type"]), 1.0),
                    q(num(&note["above"]), 1.0),
                    q(beat_value(&note["startTime"]), 1_000_000.0),
                    q(beat_value(&note["endTime"]), 1_000_000.0),
                    q(num(&note["positionX"]), 100.0),
                    q(num(&note["speed"]), 1_000_000.0),
                ]
            })
            .collect::<Vec<_>>();
        notes.sort();
        notes
    };
    let an = notes(&actual);
    let rn = notes(&reference);
    let mut diff = std::collections::BTreeMap::<[i64; 7], isize>::new();
    for n in an {
        *diff.entry(n).or_default() += 1;
    }
    for n in rn {
        *diff.entry(n).or_default() -= 1;
    }
    let differences = diff
        .into_iter()
        .filter(|(_, n)| *n != 0)
        .take(10)
        .collect::<Vec<_>>();
    assert!(differences.is_empty(), "note differences: {differences:#?}");
    assert!(approximately_equal(
        &actual["BPMList"],
        &reference["BPMList"],
        1e-9
    ));
    for (a, b) in actual["judgeLineList"]
        .as_array()
        .unwrap()
        .iter()
        .zip(reference["judgeLineList"].as_array().unwrap())
    {
        assert!(approximately_equal(&a["bpmfactor"], &b["bpmfactor"], 1e-9));
        for key in [
            "speedEvents",
            "alphaEvents",
            "moveXEvents",
            "moveYEvents",
            "rotateEvents",
        ] {
            let x = &a["eventLayers"][0][key];
            let y = &b["eventLayers"][0][key];
            if !approximately_equal(x, y, 1e-6) {
                if let (Some(xs), Some(ys)) = (x.as_array(), y.as_array()) {
                    for (i, (xe, ye)) in xs.iter().zip(ys).enumerate() {
                        if !approximately_equal(xe, ye, 1e-6) {
                            panic!("judge-line event channel {key} differs at line {}, event {i}: ours={xe}, reference={ye}",a["Name"]);
                        }
                    }
                }
                panic!("judge-line event channel {key} count differs");
            }
        }
    }
    assert_eq!(
        actual["blockAreaList"].as_array().unwrap().len(),
        reference["blockAreaList"].as_array().unwrap().len()
    );
    for (a, b) in actual["blockAreaList"]
        .as_array()
        .unwrap()
        .iter()
        .zip(reference["blockAreaList"].as_array().unwrap())
    {
        for key in [
            "Name",
            "bottomLeft",
            "topRight",
            "appearTime",
            "disappearTime",
            "activeIntervals",
            "isInvert",
            "moveXEvents",
            "moveYEvents",
            "scaleXEvents",
            "scaleYEvents",
            "rotateEvents",
        ] {
            assert!(
                approximately_equal(&a[key], &b[key], 1e-6),
                "noise-area field {key}"
            );
        }
    }
    assert!(actual.get("rpeNextLegacySource").is_none());
    let (archived, _) = convert_official_with_mode(&source, OutputMode::Archive).unwrap();
    assert_eq!(
        archived["rpeNextLegacySource"]["document"]["blockAreaList"],
        source["blockAreaList"]
    );
    let mut archived_runtime = archived;
    archived_runtime
        .as_object_mut()
        .unwrap()
        .remove("rpeNextLegacySource");
    assert_eq!(actual, archived_runtime);
}

#[test]
fn archive_candidate_selection_uses_info_chart_and_rejects_ambiguity() {
    let mut selected = BTreeMap::new();
    selected.insert(
        "info.txt".into(),
        b"Name: Human title\nPath: 1234567890\nChart: nested/source.json\nSong: music.ogg\nPicture: bg.png\nLevel: IN Lv.17\nComposer: composer\nCharter: charter\nIllustration: artist\n".to_vec(),
    );
    selected.insert(
        "nested/source.json".into(),
        serde_json::to_vec(&official()).unwrap(),
    );
    selected.insert(
        "nested/other.json".into(),
        br#"{"META":{"RPEVersion":220},"BPMList":[],"judgeLineList":[]}"#.to_vec(),
    );
    let (output, report) =
        archive::convert_archive(&archive::write_zip(&selected).unwrap()).unwrap();
    assert_eq!(report.chart_path, "nested/source.json");
    let roundtrip = archive::read_zip(&output).unwrap();
    let converted: Value = serde_json::from_slice(&roundtrip["nested/source.json"]).unwrap();
    assert_eq!(converted["META"]["name"], "Human title");
    assert_eq!(converted["META"]["song"], "music.ogg");
    assert_eq!(converted["META"]["background"], "bg.png");
    assert_eq!(converted["META"]["level"], "IN Lv.17");
    assert_eq!(converted["META"]["composer"], "composer");
    assert_eq!(converted["META"]["charter"], "charter");
    assert_eq!(converted["META"]["illustration"], "artist");
    assert_eq!(
        roundtrip["nested/other.json"],
        selected["nested/other.json"]
    );

    selected.remove("info.txt");
    let ambiguous = archive::write_zip(&selected).unwrap();
    assert!(archive::convert_archive(&ambiguous)
        .unwrap_err()
        .to_string()
        .contains("multiple chart JSON candidates"));
    selected.insert(
        "nested/info.yml".into(),
        b"chart: \"source.json\"\n".to_vec(),
    );
    let yml_selected = archive::write_zip(&selected).unwrap();
    assert_eq!(
        archive::convert_archive(&yml_selected)
            .unwrap()
            .1
            .chart_path,
        "nested/source.json"
    );
}

#[test]
fn damaged_or_chartless_archives_fail_without_partial_output() {
    assert!(archive::convert_archive(b"not a zip archive").is_err());
    let mut files = BTreeMap::new();
    files.insert("info.txt".into(), b"Chart: missing.json\n".to_vec());
    files.insert("music.ogg".into(), vec![1, 2, 3]);
    assert!(archive::convert_archive(&archive::write_zip(&files).unwrap()).is_err());
}

#[test]
fn supplied_real_pez_archives_convert_and_preserve_members() {
    let official_path = std::env::var("PHIGROS_SAMPLE_OFFICIAL_PEZ")
        .unwrap_or_else(|_| "D:/Users/lemon/Downloads/Desultory_Signals_IN.pez".into());
    let reference_path = std::env::var("PHIGROS_SAMPLE_RPEN_PEZ")
        .unwrap_or_else(|_| "D:/Users/lemon/Downloads/1573320261807187-RPEN重导.pez".into());
    if !std::path::Path::new(&official_path).exists()
        || !std::path::Path::new(&reference_path).exists()
    {
        eprintln!("real PEZ regression skipped; set PHIGROS_SAMPLE_OFFICIAL_PEZ and PHIGROS_SAMPLE_RPEN_PEZ");
        return;
    }
    let official_bytes = fs::read(&official_path).unwrap();
    let rpen_bytes = fs::read(&reference_path).unwrap();
    let source_files = archive::read_zip(&official_bytes).unwrap();
    let reference_files = archive::read_zip(&rpen_bytes).unwrap();
    let source_chart_path = info_chart(&source_files);
    let reference_chart_path = info_chart(&reference_files);
    let source_chart: Value = serde_json::from_slice(&source_files[&source_chart_path]).unwrap();
    let reference_chart: Value =
        serde_json::from_slice(&reference_files[&reference_chart_path]).unwrap();
    assert_eq!(
        detect_format(&source_chart).unwrap(),
        ChartFormat::OfficialV3
    );
    assert_eq!(detect_format(&reference_chart).unwrap(), ChartFormat::Rpen);

    let (standard_pez, report) = archive::convert_archive(&official_bytes).unwrap();
    assert_eq!(report.format, ChartFormat::OfficialV3);
    assert_eq!(report.chart_path, source_chart_path);
    let standard_files = archive::read_zip(&standard_pez).unwrap();
    assert_eq!(
        standard_files.keys().collect::<Vec<_>>(),
        source_files.keys().collect::<Vec<_>>()
    );
    for (name, bytes) in &source_files {
        if name != &source_chart_path {
            assert_eq!(&standard_files[name], bytes, "asset changed: {name}");
        }
    }
    let mut input_signatures = zip_member_signatures(&official_bytes);
    let mut output_signatures = zip_member_signatures(&standard_pez);
    let input_chart_signature = input_signatures.remove(&source_chart_path).unwrap();
    let output_chart_signature = output_signatures.remove(&source_chart_path).unwrap();
    assert_eq!(
        input_chart_signature.0, output_chart_signature.0,
        "rewritten chart compression method changed"
    );
    assert_eq!(
        output_signatures, input_signatures,
        "non-chart ZIP metadata changed"
    );
    let converted: Value = serde_json::from_slice(&standard_files[&source_chart_path]).unwrap();
    assert_eq!(detect_format(&converted).unwrap(), ChartFormat::Rpen);
    assert!(converted.get("rpeNextLegacySource").is_none());
    assert_eq!(converted["META"]["name"], "Desultory Signals");
    assert_eq!(converted["META"]["song"], "1573320261807187.ogg");
    assert_eq!(converted["META"]["background"], "1573320261807187.png");
    assert_eq!(converted["META"]["level"], "IN Lv.17");
    assert_eq!(converted["META"]["composer"], "technoplanet");
    assert_eq!(
        converted["META"]["charter"],
        "Retaliating Resolve \"反击的决意\""
    );
    assert_eq!(converted["META"]["illustration"], "Phigros");
    assert_reference_semantics(&converted, &reference_chart);

    let (archive_pez, archive_report) =
        archive::convert_archive_with_mode(&official_bytes, OutputMode::Archive).unwrap();
    assert_eq!(archive_report.chart_path, source_chart_path);
    assert_eq!(archive_report.output_chart_path, "Desultory Signals.json");
    let archive_files = archive::read_zip(&archive_pez).unwrap();
    assert_eq!(
        info_chart(&archive_files),
        "Desultory Signals.json",
        "archive chart references must follow the title-based filename"
    );
    let archived_chart: Value =
        serde_json::from_slice(&archive_files[&archive_report.output_chart_path]).unwrap();
    assert!(archived_chart.get("rpeNextLegacySource").is_some());
    let mut archived_runtime = archived_chart;
    archived_runtime
        .as_object_mut()
        .unwrap()
        .remove("rpeNextLegacySource");
    assert_eq!(converted, archived_runtime);

    let (rpen_roundtrip, rpen_report) = archive::convert_archive(&rpen_bytes).unwrap();
    assert_eq!(rpen_report.format, ChartFormat::Rpen);
    assert_eq!(archive::read_zip(&rpen_roundtrip).unwrap(), reference_files);
}
