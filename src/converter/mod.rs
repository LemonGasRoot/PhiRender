use crate::{
    error::invalid,
    model::rpe::{beat, object, rpe_event},
    Error, Result,
};
use serde_json::{json, Map, Value};

#[derive(Clone, Debug, Default)]
pub struct ConversionReport {
    pub diagnostics: Vec<Diagnostic>,
    pub notes: usize,
    pub judge_lines: usize,
    pub noise_areas: usize,
}

/// Controls whether the complete official source document is embedded in the
/// converted RPEN chart for diagnostics and archival recovery.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum OutputMode {
    /// Emit RPEN runtime data only. This is the normal compact output.
    #[default]
    Standard,
    /// Also embed the original Phigros v3 document under
    /// `rpeNextLegacySource.document`.
    Archive,
}
#[derive(Clone, Debug)]
pub struct Diagnostic {
    pub severity: Severity,
    pub path: String,
    pub message: String,
}
#[derive(Clone, Copy, Debug)]
pub enum Severity {
    Warning,
    Info,
}

fn number(v: &Value, path: &str) -> Result<f64> {
    let n = v
        .as_f64()
        .ok_or_else(|| invalid(path, "expected a number"))?;
    if !n.is_finite() {
        return Err(invalid(path, "number must be finite"));
    }
    Ok(n)
}
fn field_num(v: &Value, key: &str, path: &str, default: Option<f64>) -> Result<f64> {
    match v.get(key) {
        Some(x) => number(x, &format!("{path}.{key}")),
        None => default.ok_or_else(|| invalid(format!("{path}.{key}"), "missing required field")),
    }
}
fn arr<'a>(v: &'a Value, key: &str, path: &str) -> Result<&'a [Value]> {
    match v.get(key) {
        None | Some(Value::Null) => Ok(&[]),
        Some(Value::Array(a)) => Ok(a),
        _ => Err(invalid(format!("{path}.{key}"), "expected an array")),
    }
}
fn to_beat(ticks: f64) -> [i64; 3] {
    ((ticks + 0.1).floor().max(-100.0) / 32.0).pipe(|x| beat(x, 19_200))
}
trait Pipe: Sized {
    fn pipe<R>(self, f: impl FnOnce(Self) -> R) -> R {
        f(self)
    }
}
impl<T> Pipe for T {}

fn rpe_note(note: &Value, above: u8, path: &str) -> Result<Value> {
    let kind_value = field_num(note, "type", path, None)?;
    if kind_value.fract() != 0.0 {
        return Err(invalid(
            format!("{path}.type"),
            "note type must be an integer",
        ));
    }
    let kind = kind_value as u32;
    let ty = match kind {
        1 => 1,
        2 => 4,
        3 => 2,
        4 => 3,
        _ => {
            return Err(invalid(
                format!("{path}.type"),
                format!("unknown official note type {kind}"),
            ))
        }
    };
    let time = field_num(note, "time", path, None)?;
    let hold = field_num(note, "holdTime", path, Some(0.0))?;
    let position = field_num(note, "positionX", path, None)?;
    let speed = if ty == 2 {
        1.0
    } else {
        field_num(note, "speed", path, Some(1.0))?
    };
    Ok(object([
        ("type", json!(ty)),
        ("startTime", json!(to_beat(time))),
        (
            "endTime",
            json!(to_beat(time + if ty == 2 { hold } else { 0.0 })),
        ),
        ("positionX", json!(position * 75.0)),
        ("above", json!(above)),
        ("isFake", json!(0)),
        ("speed", json!(speed)),
        ("size", json!(1.0)),
        ("yOffset", json!(0.0)),
        ("visibleTime", json!(999999.0)),
        ("alpha", json!(255)),
    ]))
}

fn rpe_line(line: &Value, index: usize, base_bpm: f64) -> Result<Value> {
    let path = format!("judgeLineList[{index}]");
    let bpm = field_num(line, "bpm", &path, None)?;
    if bpm <= 0.0 {
        return Err(invalid(format!("{path}.bpm"), "BPM must be positive"));
    }
    let mut layer = Map::new();
    for (source, target, mapper) in [
        ("speedEvents", "speedEvents", 0u8),
        ("judgeLineDisappearEvents", "alphaEvents", 1),
        ("judgeLineMoveEvents", "moveXEvents", 2),
        ("judgeLineMoveEvents", "moveYEvents", 3),
        ("judgeLineRotateEvents", "rotateEvents", 4),
    ] {
        let mut events = Vec::new();
        for (ei, e) in arr(line, source, &path)?.iter().enumerate() {
            let ep = format!("{path}.{source}[{ei}]");
            let (s, en) = match mapper {
                0 => {
                    let x = field_num(e, "value", &ep, None)? * 4.5;
                    (x, x)
                }
                1 => (
                    (field_num(e, "start", &ep, None)? * 255.0).trunc(),
                    (field_num(e, "end", &ep, None)? * 255.0).trunc(),
                ),
                2 => (
                    -675.0 + field_num(e, "start", &ep, None)? * 1350.0,
                    -675.0 + field_num(e, "end", &ep, None)? * 1350.0,
                ),
                3 => (
                    -450.0 + field_num(e, "start2", &ep, None)? * 900.0,
                    -450.0 + field_num(e, "end2", &ep, None)? * 900.0,
                ),
                _ => (
                    -field_num(e, "start", &ep, None)?,
                    -field_num(e, "end", &ep, None)?,
                ),
            };
            events.push(rpe_event(
                s,
                en,
                to_beat(field_num(e, "startTime", &ep, None)?),
                to_beat(field_num(e, "endTime", &ep, None)?),
            ));
        }
        layer.insert(target.into(), Value::Array(events));
    }
    let mut notes = Vec::new();
    for (field, above) in [("notesAbove", 1), ("notesBelow", 0)] {
        for (ni, n) in arr(line, field, &path)?.iter().enumerate() {
            notes.push(rpe_note(n, above, &format!("{path}.{field}[{ni}]"))?);
        }
    }
    let num_notes = notes.len();
    Ok(object([
        ("Group", json!(0)),
        ("Name", json!(format!("Line {}", index + 1))),
        ("Texture", json!("line.png")),
        ("bpmfactor", json!(base_bpm / bpm)),
        ("father", json!(-1)),
        ("rotateWithFather", json!(true)),
        ("isCover", json!(1)),
        ("zOrder", json!(0)),
        ("anchor", json!([0.5, 0.5])),
        ("isGif", json!(false)),
        ("eventLayers", json!([layer])),
        ("extended", json!({})),
        ("notes", json!(notes)),
        ("numOfNotes", json!(num_notes)),
    ]))
}

fn pos(v: &Value, path: &str) -> Result<Value> {
    let x = field_num(v, "x", path, None)? * 1350.0 - 675.0;
    let y = field_num(v, "y", path, None)? * 900.0 - 450.0;
    Ok(json!({"x":x,"y":y}))
}
#[derive(Clone)]
struct Frame {
    time: f64,
    value: f64,
    ease: u32,
    anchor: Option<Value>,
}
fn frames(
    area: &Value,
    kind: &str,
    axis: Option<char>,
    path: &str,
    anchored: bool,
) -> Result<Vec<Frame>> {
    let mut out = Vec::new();
    for (i, e) in arr(area, kind, path)?.iter().enumerate() {
        let p = format!("{path}.{kind}[{i}]");
        let ease_name = axis.map(|a| format!("easeType{}", a.to_ascii_uppercase()));
        let ease = e
            .get("easeType")
            .or_else(|| ease_name.as_ref().and_then(|x| e.get(x)))
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32;
        if ease > 14 {
            return Err(invalid(
                format!("{p}.easeType"),
                "supported official noise easing IDs are 0 through 14",
            ));
        }
        let value = match kind {
            "moveEvents" => {
                let position = pos(
                    e.get("endPosition")
                        .ok_or_else(|| invalid(format!("{p}.endPosition"), "missing field"))?,
                    &format!("{p}.endPosition"),
                )?;
                position[axis.unwrap().to_string()].as_f64().unwrap()
            }
            "scaleEvents" => number(
                e.get("scale")
                    .and_then(|x| x.get(axis.unwrap().to_string()))
                    .ok_or_else(|| invalid(format!("{p}.scale"), "missing axis scale"))?,
                &format!("{p}.scale"),
            )?,
            _ => -field_num(e, "rotation", &p, None)?,
        };
        let anchor = if anchored {
            Some(pos(
                e.get("anchor")
                    .ok_or_else(|| invalid(format!("{p}.anchor"), "missing anchor"))?,
                &format!("{p}.anchor"),
            )?)
        } else {
            None
        };
        out.push(Frame {
            time: field_num(e, "time", &p, None)?,
            value,
            ease,
            anchor,
        });
    }
    out.sort_by(|a, b| a.time.total_cmp(&b.time));
    Ok(out)
}
const EASE: [u32; 12] = [1, 5, 4, 7, 9, 8, 12, 11, 10, 13, 15, 14];
fn noise_event(
    start: f64,
    end: f64,
    t0: f64,
    t1: f64,
    anchor: &Option<Value>,
    ease: u32,
    meta: &[(&str, Value)],
    offset: f64,
    bpm: f64,
) -> Value {
    let mut e = rpe_event(
        start,
        end,
        beat((t0 - offset) * bpm / 60.0, 1_000_000_000),
        beat((t1 - offset) * bpm / 60.0, 1_000_000_000),
    );
    let o = e.as_object_mut().unwrap();
    o.insert("easingType".into(), json!(ease));
    o.insert("inst".into(), json!(0));
    if let Some(a) = anchor {
        o.insert("anchor".into(), a.clone());
    }
    for (k, v) in meta {
        o.insert((*k).into(), v.clone());
    }
    Value::Object(o.clone())
}
fn noise_track(fs: Vec<Frame>, anchored: bool, offset: f64, bpm: f64) -> Vec<Value> {
    let mut out = Vec::new();
    if anchored {
        for pair in fs.windows(2) {
            let (a, b) = (&pair[0], &pair[1]);
            if a.time == b.time {
                out.push(noise_event(
                    b.value,
                    b.value,
                    a.time,
                    b.time,
                    &a.anchor,
                    1,
                    &[
                        ("transformStart", json!(a.value)),
                        ("transformEnd", json!(b.value)),
                        ("transformStep", json!(true)),
                    ],
                    offset,
                    bpm,
                ));
            } else if a.ease == 13 || a.ease == 14 {
                let value = if a.ease == 13 { a.value } else { b.value };
                out.push(noise_event(
                    value,
                    value,
                    a.time,
                    b.time,
                    &a.anchor,
                    1,
                    &[
                        ("transformStart", json!(a.value)),
                        ("transformEnd", json!(b.value)),
                    ],
                    offset,
                    bpm,
                ));
            } else if a.ease == 12 && a.value != b.value {
                let mt = (a.time + b.time) / 2.0;
                let mv = (a.value + b.value) / 2.0;
                out.push(noise_event(
                    a.value,
                    mv,
                    a.time,
                    mt,
                    &a.anchor,
                    15,
                    &[],
                    offset,
                    bpm,
                ));
                out.push(noise_event(
                    mv,
                    b.value,
                    mt,
                    b.time,
                    &a.anchor,
                    14,
                    &[
                        ("transformStart", json!(mv)),
                        ("transformEnd", json!(b.value)),
                    ],
                    offset,
                    bpm,
                ));
            } else {
                out.push(noise_event(
                    a.value,
                    b.value,
                    a.time,
                    b.time,
                    &a.anchor,
                    EASE[a.ease as usize],
                    &[],
                    offset,
                    bpm,
                ));
            }
        }
        if let Some(f) = fs.last() {
            let meta = if fs.len() > 1 && fs[fs.len() - 2].time == f.time {
                vec![("transformStep", json!(true))]
            } else {
                vec![]
            };
            out.push(noise_event(
                f.value, f.value, f.time, f.time, &f.anchor, 1, &meta, offset, bpm,
            ));
        }
    } else {
        let mut groups: Vec<(f64, Frame, Frame)> = Vec::new();
        for f in fs {
            if let Some(last) = groups.last_mut().filter(|g| g.0 == f.time) {
                last.2 = f;
            } else {
                groups.push((f.time, f.clone(), f));
            }
        }
        for pair in groups.windows(2) {
            let (_, _, a) = &pair[0];
            let (_, b, _) = &pair[1];
            if a.ease == 13 || a.ease == 14 {
                let value = if a.ease == 13 { a.value } else { b.value };
                out.push(noise_event(
                    value,
                    value,
                    a.time,
                    b.time,
                    &None,
                    1,
                    &[],
                    offset,
                    bpm,
                ));
            } else if a.ease == 12 && a.value != b.value {
                let middle_time = (a.time + b.time) / 2.0;
                let middle_value = (a.value + b.value) / 2.0;
                out.push(noise_event(
                    a.value,
                    middle_value,
                    a.time,
                    middle_time,
                    &None,
                    15,
                    &[],
                    offset,
                    bpm,
                ));
                out.push(noise_event(
                    middle_value,
                    b.value,
                    middle_time,
                    b.time,
                    &None,
                    14,
                    &[],
                    offset,
                    bpm,
                ));
            } else {
                out.push(noise_event(
                    a.value,
                    b.value,
                    a.time,
                    b.time,
                    &None,
                    EASE[a.ease as usize],
                    &[],
                    offset,
                    bpm,
                ));
            }
        }
        if let Some((_, _, f)) = groups.last() {
            out.push(noise_event(
                f.value,
                f.value,
                f.time,
                f.time,
                &None,
                1,
                &[],
                offset,
                bpm,
            ));
        }
    }
    out
}

pub fn convert_official(original: &Value) -> Result<(Value, ConversionReport)> {
    convert_official_with_mode(original, OutputMode::Standard)
}

pub fn convert_official_with_mode(
    original: &Value,
    mode: OutputMode,
) -> Result<(Value, ConversionReport)> {
    if original["formatVersion"].as_u64() != Some(3) {
        return Err(Error::Unsupported(
            "only official Phigros formatVersion 3 is supported".into(),
        ));
    }
    let lines = original["judgeLineList"]
        .as_array()
        .ok_or_else(|| invalid("judgeLineList", "expected array"))?;
    if lines.is_empty() {
        return Err(invalid(
            "judgeLineList",
            "must contain at least one judge line",
        ));
    }
    let bpm = field_num(&lines[0], "bpm", "judgeLineList[0]", None)?;
    if bpm <= 0.0 {
        return Err(invalid("judgeLineList[0].bpm", "BPM must be positive"));
    }
    let offset = field_num(original, "offset", "", Some(0.0))?;
    let mut report = ConversionReport::default();
    let mut out_lines = Vec::new();
    for (i, l) in lines.iter().enumerate() {
        let n = arr(l, "notesAbove", &format!("judgeLineList[{i}]"))?.len()
            + arr(l, "notesBelow", &format!("judgeLineList[{i}]"))?.len();
        report.notes += n;
        out_lines.push(rpe_line(l, i, bpm)?);
    }
    report.judge_lines = out_lines.len();
    let mut areas = Vec::new();
    for (i, a) in arr(original, "blockAreaList", "")?.iter().enumerate() {
        let p = format!("blockAreaList[{i}]");
        let bl = pos(
            a.get("bottomLeftPercentage")
                .ok_or_else(|| invalid(format!("{p}.bottomLeftPercentage"), "missing field"))?,
            &format!("{p}.bottomLeftPercentage"),
        )?;
        let tr = pos(
            a.get("topRightPercentage")
                .ok_or_else(|| invalid(format!("{p}.topRightPercentage"), "missing field"))?,
            &format!("{p}.topRightPercentage"),
        )?;
        let appear = field_num(a, "appearTime", &p, None)?;
        let disappear = field_num(a, "disappearTime", &p, None)?;
        let enable = field_num(a, "enableTime", &p, None)?;
        let disable = field_num(a, "disableTime", &p, None)?;
        if enable > disable {
            return Err(invalid(&p, "enableTime is after disableTime"));
        }
        let track = |kind: &str, axis: Option<char>, anchored: bool| -> Result<Vec<Value>> {
            let f = frames(a, kind, axis, &p, anchored)?;
            Ok(noise_track(f, anchored, offset, bpm))
        };
        let mx = track("moveEvents", Some('x'), false)?;
        let my = track("moveEvents", Some('y'), false)?;
        let sx = track("scaleEvents", Some('x'), true)?;
        let sy = track("scaleEvents", Some('y'), true)?;
        let rot = track("rotateEvents", None, true)?;
        if a.get("isSubtract").is_some_and(|v| !v.is_boolean()) {
            return Err(invalid(format!("{p}.isSubtract"), "expected a boolean"));
        }
        let mut area = object([
            ("Name", a.get("Name").cloned().unwrap_or(json!(""))),
            ("bottomLeft", bl),
            ("topRight", tr),
            (
                "appearTime",
                json!(beat((appear - offset) * bpm / 60.0, 1_000_000_000)),
            ),
            (
                "disappearTime",
                json!(beat((disappear - offset) * bpm / 60.0, 1_000_000_000)),
            ),
            (
                "activeIntervals",
                json!([{"startTime":beat((enable-offset)*bpm/60.0,1_000_000_000),"endTime":beat((disable-offset)*bpm/60.0,1_000_000_000),"readyFlash":0.5}]),
            ),
            (
                "isInvert",
                json!(a["isSubtract"].as_bool().unwrap_or(false)),
            ),
            ("moveXEvents", json!(mx)),
            ("moveYEvents", json!(my)),
            ("scaleXEvents", json!(sx)),
            ("scaleYEvents", json!(sy)),
            ("rotateEvents", json!(rot)),
        ]);
        // An official keyframe track with no move keys leaves the rectangle centered.
        areas.push(area.take());
    }
    report.noise_areas = areas.len();
    if !areas.is_empty() {
        report.diagnostics.push(Diagnostic {
            severity: Severity::Info,
            path: "blockAreaList".into(),
            message: "Official v3 noise areas converted to RPEN geometry and animation tracks."
                .into(),
        });
    }
    if mode == OutputMode::Standard {
        report.diagnostics.push(Diagnostic {
            severity: Severity::Warning,
            path: "official-v3-unmapped-fields".into(),
            message: "Unmapped source fields are omitted from standard RPEN output; use archive mode to retain the complete original document.".into(),
        });
    } else {
        report.diagnostics.push(Diagnostic {
            severity: Severity::Warning,
            path: "rpeNextLegacySource.document".into(),
            message: "The full official JSON is embedded for unknown-field recovery. Only the documented v3 chart and noise fields are mapped to RPEN runtime semantics.".into(),
        });
    }
    let name = "未命名谱面";
    let mut chart = object([
        (
            "META",
            json!({"RPEVersion":if areas.is_empty(){170}else{220},"name":name,"composer":"","charter":"","illustration":"","level":"","song":"","background":"","offset":(offset*1000.0).round() as i64}),
        ),
        ("BPMList", json!([{"bpm":bpm,"startTime":[0,0,1]}])),
        ("judgeLineGroup", json!(["Default"])),
        ("judgeLineList", json!(out_lines)),
        ("blockAreaList", json!(areas)),
        ("noiseAreaOptions", json!({"ignoreTripleInversion":true})),
    ]);
    if mode == OutputMode::Archive {
        chart["rpeNextLegacySource"] = json!({"format":"phigros-v3","document":original});
    }
    Ok((chart, report))
}
