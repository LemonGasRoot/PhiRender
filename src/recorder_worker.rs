//! Native recorder adapter; preserves the desktop worker job/event protocol.
use super::{event, prepare_skin, Job};
use anyhow::{bail, Context, Result};
use phigros_rpen_converter::archive;
use serde_json::{json, Value};
use std::{
    collections::HashMap,
    io::{BufRead, BufReader, Write},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
struct Stage(PathBuf);
impl Stage {
    fn new() -> Result<Self> {
        let p = std::env::temp_dir().join(format!(
            "phirender-recorder-{}-{}",
            std::process::id(),
            SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos()
        ));
        std::fs::create_dir(&p)?;
        Ok(Self(p))
    }
}
impl Drop for Stage {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}
fn hidden(c: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(0x08000000);
    }
}
fn kill_tree(c: &mut std::process::Child) {
    #[cfg(windows)]
    {
        let mut t = Command::new("taskkill");
        t.args(["/PID", &c.id().to_string(), "/T", "/F"]);
        hidden(&mut t);
        let _ = t.stdout(Stdio::null()).stderr(Stdio::null()).status();
    }
    let _ = c.kill();
    let _ = c.wait();
}
fn check_cancel(c: &AtomicBool) -> Result<()> {
    if c.load(Ordering::Relaxed) {
        bail!("任务已取消")
    }
    Ok(())
}
fn run_quiet(cmd: &mut Command, stage: &Path, cancel: &AtomicBool) -> Result<()> {
    check_cancel(cancel)?;
    let log = stage.join("command-error.log");
    hidden(cmd);
    let mut child = cmd
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(std::fs::File::create(&log)?)
        .spawn()?;
    loop {
        if cancel.load(Ordering::Relaxed) {
            kill_tree(&mut child);
            bail!("任务已取消")
        };
        if let Some(s) = child.try_wait()? {
            if !s.success() {
                bail!(
                    "处理失败：{}",
                    std::fs::read_to_string(&log).unwrap_or_default()
                )
            }
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}
fn copy_dir(a: &Path, b: &Path) -> Result<()> {
    std::fs::create_dir_all(b)?;
    for e in std::fs::read_dir(a)? {
        let e = e?;
        if e.file_type()?.is_dir() {
            copy_dir(&e.path(), &b.join(e.file_name()))?
        } else {
            std::fs::copy(e.path(), b.join(e.file_name()))?;
        }
    }
    Ok(())
}
fn ffmpeg(root: &Path) -> Result<PathBuf> {
    if let Some(p) = std::env::var_os("PHIRA_FFMPEG") {
        if Path::new(&p).is_file() {
            return Ok(p.into());
        }
    }
    for s in [
        "work/ffmpeg-8.0.1-extracted/ffmpeg-8.0.1-essentials_build/bin/ffmpeg.exe",
        "work/ffmpeg-package/extracted/ffmpeg-9.0.1-essentials_build/bin/ffmpeg.exe",
    ] {
        let p = root.join(s);
        if p.is_file() {
            return Ok(p);
        }
    }
    let p = std::env::current_exe()?.with_file_name("ffmpeg.exe");
    if p.is_file() {
        return Ok(p);
    }
    bail!("找不到FFmpeg")
}
fn resource(root: &Path, chart: &str, name: &str) -> Result<PathBuf> {
    if name.is_empty() {
        bail!("资源路径为空")
    };
    let dir = Path::new(chart).parent().unwrap_or(Path::new(""));
    for p in [root.join(dir).join(name), root.join(name)] {
        if p.is_file() {
            let p = p.canonicalize()?;
            if p.starts_with(root.canonicalize()?) {
                return Ok(p);
            }
        }
    }
    bail!("找不到资源：{name}")
}
fn info_value(files: &std::collections::BTreeMap<String, Vec<u8>>, key: &str) -> Option<String> {
    files
        .iter()
        .filter(|(n, _)| {
            n.rsplit('/')
                .next()
                .is_some_and(|x| x.eq_ignore_ascii_case("info.txt"))
        })
        .find_map(|(_, b)| {
            String::from_utf8_lossy(b).lines().find_map(|l| {
                let (k, v) = l.split_once(':')?;
                k.trim()
                    .eq_ignore_ascii_case(key)
                    .then(|| v.trim().to_owned())
            })
        })
}
fn beat(v: &Value) -> Result<f64> {
    if let Some(x) = v.as_f64() {
        return Ok(x);
    }
    let a = v.as_array().context("无效拍数")?;
    if a.len() != 3 {
        bail!("无效拍数")
    };
    let d = a[2].as_f64().context("无效拍数分母")?;
    if d == 0. {
        bail!("拍数分母为0")
    };
    Ok(a[0].as_f64().unwrap_or(0.) + a[1].as_f64().unwrap_or(0.) / d)
}
fn seconds(b: f64, map: &[(f64, f64)]) -> f64 {
    let mut result = map[0].0 * 60. / map[0].1;
    let mut i = 0;
    while i + 1 < map.len() && map[i + 1].0 <= b {
        result += (map[i + 1].0 - map[i].0) * 60. / map[i].1;
        i += 1;
    }
    result + (b - map[i].0) * 60. / map[i].1
}
fn decode(
    ff: &Path,
    input: &Path,
    out: &Path,
    stage: &Path,
    cancel: &AtomicBool,
) -> Result<Vec<f32>> {
    let mut c = Command::new(ff);
    c.args(["-y", "-v", "error", "-i"])
        .arg(input)
        .args(["-vn", "-f", "f32le", "-ar", "44100", "-ac", "2"])
        .arg(out);
    run_quiet(&mut c, stage, cancel)?;
    Ok(std::fs::read(out)?
        .chunks_exact(4)
        .map(|b| f32::from_le_bytes(b.try_into().unwrap()))
        .collect())
}
fn write_wave(path: &Path, v: &[f32]) -> Result<()> {
    let len = u32::try_from(v.len() * 4)?;
    let mut f = std::io::BufWriter::new(std::fs::File::create(path)?);
    f.write_all(b"RIFF")?;
    f.write_all(&(36 + len).to_le_bytes())?;
    f.write_all(b"WAVEfmt ")?;
    for n in [16u32, 0x00020003, 44100, 352800] {
        f.write_all(&n.to_le_bytes())?;
    }
    f.write_all(&8u16.to_le_bytes())?;
    f.write_all(&32u16.to_le_bytes())?;
    f.write_all(b"data")?;
    f.write_all(&len.to_le_bytes())?;
    for x in v {
        f.write_all(&x.to_le_bytes())?;
    }
    Ok(())
}
fn prepare_audio(
    j: &Job,
    d: &Value,
    music: &Path,
    chart: &str,
    unpacked: &Path,
    skin: &Path,
    stage: &Path,
    ff: &Path,
    end: f64,
    cancel: &AtomicBool,
) -> Result<PathBuf> {
    let output = stage.join("audio.wav");
    let mut cmd = Command::new(ff);
    cmd.args(["-y", "-v", "error", "-ss"])
        .arg(format!("{:.9}", j.start))
        .arg("-i")
        .arg(music);
    if j.hitsounds {
        let mut map = d["BPMList"]
            .as_array()
            .context("缺少BPMList")?
            .iter()
            .map(|v| {
                Ok((
                    beat(&v["startTime"])?,
                    v["bpm"].as_f64().context("无效BPM")?,
                ))
            })
            .collect::<Result<Vec<_>>>()?;
        map.sort_by(|a, b| a.0.total_cmp(&b.0));
        if map.is_empty() || map.iter().any(|(_, b)| *b <= 0.) {
            bail!("无效BPMList")
        }
        let mut values = vec![0f32; ((end - j.start) * 44100.).ceil() as usize * 2];
        let mut sounds = HashMap::<String, Vec<f32>>::new();
        for line in d["judgeLineList"].as_array().context("缺少判定线")? {
            for n in line["notes"].as_array().into_iter().flatten() {
                check_cancel(cancel)?;
                if n["isFake"]
                    .as_bool()
                    .unwrap_or_else(|| n["isFake"].as_i64().unwrap_or(0) != 0)
                {
                    continue;
                };
                let at = seconds(beat(&n["startTime"])?, &map)
                    - d["META"]["offset"].as_f64().unwrap_or(0.) / 1000.
                    - j.start;
                if at < 0. || at >= end - j.start {
                    continue;
                };
                let kind = match n["type"].as_u64().unwrap_or(1) {
                    3 => "flick",
                    4 => "drag",
                    _ => "click",
                };
                let sound = n["hitsound"].as_str().filter(|s| !s.is_empty());
                let kind = match sound {
                    Some("tap.mp3") => "click",
                    Some("drag.mp3") => "drag",
                    Some("flick.mp3") => "flick",
                    _ => kind,
                };
                let name = match sound {
                    Some("tap.mp3" | "drag.mp3" | "flick.mp3") | None => kind,
                    Some(name) => name,
                }
                .to_owned();
                if !sounds.contains_key(&name) {
                    let path = if name != kind {
                        resource(unpacked, chart, &name)?
                    } else {
                        ["ogg", "wav", "mp3"]
                            .iter()
                            .map(|ext| skin.join(format!("{kind}.{ext}")))
                            .find(|p| p.is_file())
                            .context("皮肤缺少打击音")?
                    };
                    sounds.insert(
                        name.clone(),
                        decode(ff, &path, &stage.join("clip.f32"), stage, cancel)?,
                    );
                }
                let first = (at * 44100.).round() as usize * 2;
                for (slot, x) in values[first..].iter_mut().zip(&sounds[&name]) {
                    *slot += x;
                }
            }
        }
        let hits = stage.join("hits.wav");
        write_wave(&hits, &values)?;
        cmd.arg("-i").arg(hits).arg("-filter_complex").arg(format!("[0:a]volume={:.6}[m];[1:a]volume={:.6}[h];[m][h]amix=inputs=2:duration=longest:normalize=0[a]",j.music_volume,j.hitsound_volume)).args(["-map","[a]"]);
    } else {
        cmd.arg("-af").arg(format!("volume={:.6}", j.music_volume));
    }
    cmd.arg("-t")
        .arg(format!("{:.9}", end - j.start))
        .args(["-ar", "44100", "-ac", "2", "-c:a", "pcm_f32le"])
        .arg(&output);
    run_quiet(&mut cmd, stage, cancel)?;
    Ok(output)
}
fn font_family(path: &Path) -> Result<String> {
    let b = std::fs::read(path)?;
    let u16at =
        |i: usize| -> Option<u16> { Some(u16::from_be_bytes(b.get(i..i + 2)?.try_into().ok()?)) };
    let u32at =
        |i: usize| -> Option<u32> { Some(u32::from_be_bytes(b.get(i..i + 4)?.try_into().ok()?)) };
    for i in 0..u16at(4).context("无效字体")? as usize {
        let p = 12 + 16 * i;
        if b.get(p..p + 4) != Some(b"name") {
            continue;
        };
        let start = u32at(p + 8).context("无效字体目录")? as usize;
        let count = u16at(start + 2).context("无效字体名称")? as usize;
        let data = start + u16at(start + 4).context("无效字体名称")? as usize;
        for j in 0..count {
            let p = start + 6 + j * 12;
            if u16at(p + 6) != Some(1) {
                continue;
            };
            let off = data + u16at(p + 10).unwrap_or(0) as usize;
            let end = off + u16at(p + 8).unwrap_or(0) as usize;
            if let Some(s) = b.get(off..end) {
                let name = if matches!(u16at(p), Some(0 | 3)) {
                    String::from_utf16_lossy(
                        &s.chunks_exact(2)
                            .map(|b| u16::from_be_bytes([b[0], b[1]]))
                            .collect::<Vec<_>>(),
                    )
                } else {
                    String::from_utf8_lossy(s).into_owned()
                };
                if !name.is_empty() {
                    return Ok(name);
                }
            }
        }
    }
    bail!("无法读取字体家族：{}", path.display())
}
pub(super) fn run(job: Job) -> Result<()> {
    let cancel = Arc::new(AtomicBool::new(false));
    let c = cancel.clone();
    std::thread::spawn(move || {
        for l in std::io::stdin().lock().lines().map_while(|l| l.ok()) {
            if l.trim() == "cancel" {
                c.store(true, Ordering::Relaxed);
                break;
            }
        }
    });
    let result = render(&job, &cancel);
    if result.is_err() || cancel.load(Ordering::Relaxed) {
        let _ = std::fs::remove_file(&job.output);
        for index in 0..8 {
            for suffix in [format!(".seg{index}.mp4"), format!(".seg{index}.mp4.log")] {
                let mut p = job.output.as_os_str().to_os_string();
                p.push(suffix);
                let _ = std::fs::remove_file(PathBuf::from(p));
            }
        }
    }
    if cancel.load(Ordering::Relaxed) {
        event(json!({"type":"cancelled"}));
        return Ok(());
    }
    result
}

fn render(j: &Job, cancel: &Arc<AtomicBool>) -> Result<()> {
    if !j.music_volume.is_finite()
        || !j.hitsound_volume.is_finite()
        || !(0.0..=8.0).contains(&j.music_volume)
        || !(0.0..=8.0).contains(&j.hitsound_volume)
    {
        bail!("无效音量")
    };
    let stage = Stage::new()?;
    let unpacked = stage.0.join("chart");
    std::fs::create_dir(&unpacked)?;
    let bytes = std::fs::read(&j.chart)?;
    let report = archive::inspect_archive(&bytes)?;
    let files = archive::read_zip(&bytes)?;
    for (name, data) in &files {
        let p = unpacked.join(name);
        if let Some(d) = p.parent() {
            std::fs::create_dir_all(d)?;
        }
        std::fs::write(p, data)?;
    }
    let chart_path = unpacked.join(&report.chart_path);
    let mut doc: Value =
        serde_json::from_slice(files.get(&report.chart_path).context("缺少谱面")?)?;
    let song = doc["META"]["song"]
        .as_str()
        .filter(|s| !s.is_empty())
        .map(str::to_owned)
        .or_else(|| info_value(&files, "Song"))
        .context("未关联音乐")?;
    let music = resource(&unpacked, &report.chart_path, &song)?;
    let image = doc["META"]["background"]
        .as_str()
        .filter(|s| !s.is_empty())
        .map(str::to_owned)
        .or_else(|| info_value(&files, "Picture"))
        .filter(|s| !s.is_empty())
        .map(|s| resource(&unpacked, &report.chart_path, &s))
        .transpose()?;
    doc["META"]["offset"] = json!(doc["META"]["offset"].as_f64().unwrap_or(0.) - j.chart_delay_ms);
    std::fs::write(&chart_path, serde_json::to_vec(&doc)?)?;
    let ff = ffmpeg(&j.project_root)?;
    let mut probe = Command::new(ff.with_file_name("ffprobe.exe"));
    hidden(&mut probe);
    let out = probe
        .args([
            "-v",
            "error",
            "-show_entries",
            "format=duration",
            "-of",
            "default=nw=1:nk=1",
        ])
        .arg(&music)
        .output()
        .context("无法启动ffprobe")?;
    let duration = String::from_utf8_lossy(&out.stdout)
        .trim()
        .parse::<f64>()
        .context("无法读取歌曲时长")?;
    let end = j.end.unwrap_or(duration).min(duration);
    if j.kind != "preview" && (j.start < 0. || end <= j.start) {
        bail!("导出区间无效")
    };
    let sibling = std::env::current_exe()?
        .parent()
        .context("无法定位程序")?
        .to_path_buf();
    let executable = [
        sibling.join("phirender-recorder.exe"),
        j.project_root
            .join("outputs/phi-recorder-1.1.1/out/phirender.exe"),
    ]
    .into_iter()
    .find(|p| p.is_file())
    .context("找不到Phi-Recorder渲染组件")?;
    let source_assets = [
        sibling.join("recorder-assets"),
        j.project_root
            .join("outputs/phi-recorder-1.1.1/assets/default"),
    ]
    .into_iter()
    .find(|p| p.is_dir())
    .context("找不到Phi-Recorder资源")?;
    let assets = stage.0.join("assets");
    for dir in ["noise", "blockarea", "hud"] {
        copy_dir(&source_assets.join(dir), &assets.join(dir))?;
    }
    let skin = prepare_skin(j, &j.project_root.join("work/Phira-Pro/assets"))?;
    copy_dir(&skin.dir, &assets.join("respack"))?;
    let params = j
        .experimental_config
        .as_object()
        .context("实验参数必须为JSON对象")?;
    for key in params.keys() {
        if !["flowSpeed", "noteScale", "backgroundBrightness"].contains(&key.as_str()) {
            bail!("新引擎暂不支持实验参数 {key}；请清空该项或在设置切换旧引擎")
        }
    }
    let factor = (j.sample_count as f64).sqrt();
    let width = ((j.width as f64 * factor / 2.).ceil() * 2.) as u32;
    let height = ((j.height as f64 * factor / 2.).ceil() * 2.) as u32;
    let mut command = Command::new(executable);
    command
        .arg("--chart")
        .arg(&chart_path)
        .arg("--assets")
        .arg(&assets)
        .args([
            "--w",
            &width.to_string(),
            "--h",
            &height.to_string(),
            "--fps",
            &j.fps.to_string(),
        ]);
    if let Some(image) = image {
        command.arg("--bg").arg(image);
    };
    for (key, arg) in [
        ("flowSpeed", "--flow-speed"),
        ("noteScale", "--note-size"),
        ("backgroundBrightness", "--bg-brightness"),
    ] {
        if let Some(v) = params.get(key) {
            command.arg(arg).arg(v.to_string());
        }
    }
    if j.show_hud {
        let hud = j.project_root.join("work/Phira-Pro/assets/phigros.ttf");
        let font = stage.0.join("info.ttf");
        let bytes = if let Some(path) = &j.font {
            std::fs::read(path).with_context(|| format!("无法读取字体 {}", path.display()))?
        } else {
            include_bytes!("../apps/desktop/src-tauri/resources/saira-regular.ttf").to_vec()
        };
        std::fs::write(&font, bytes)?;
        command
            .arg("--hud")
            .arg("--hud-font")
            .arg(hud)
            .args(["--hud-font-family", "Untitled2", "--info-font"])
            .arg(&font)
            .arg("--info-font-family")
            .arg(font_family(&font)?);
    }
    let png = stage.0.join("preview.png");
    if j.kind == "preview" {
        command
            .arg("--time")
            .arg(j.time.clamp(0., duration).to_string())
            .arg("--out")
            .arg(&png)
            .arg("--no-audio");
    } else {
        command.arg("--video").arg(&j.output).args([
            "--from",
            &j.start.to_string(),
            "--to",
            &end.to_string(),
            "--preset",
            "ultrafast",
            "--encoders",
            "1",
            "--no-hits",
            "--output-w",
            &j.width.to_string(),
            "--output-h",
            &j.height.to_string(),
        ]);
        if j.nvenc {
            command.arg("--nvenc");
        };
        if j.include_audio {
            command.arg("--audio").arg(prepare_audio(
                j,
                &doc,
                &music,
                &report.chart_path,
                &unpacked,
                &skin.dir,
                &stage.0,
                &ff,
                end,
                cancel,
            )?);
        } else {
            command.arg("--no-audio");
        }
    }
    let mut paths = vec![ff.parent().context("FFmpeg路径无效")?.to_path_buf()];
    paths.extend(std::env::split_paths(
        &std::env::var_os("PATH").unwrap_or_default(),
    ));
    command.env("PATH", std::env::join_paths(paths)?);
    hidden(&mut command);
    let log = stage.0.join("recorder-error.log");
    let mut child = command
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(std::fs::File::create(&log)?)
        .spawn()?;
    let stdout = child.stdout.take().context("无法读取渲染进度")?;
    let begun = Instant::now();
    let reader = std::thread::spawn(move || {
        for l in BufReader::new(stdout).lines().map_while(|l| l.ok()) {
            if let Some((a, b)) = l
                .trim()
                .strip_prefix("frame ")
                .and_then(|s| s.split_whitespace().next())
                .and_then(|s| s.split_once('/'))
            {
                if let (Ok(frame), Ok(total)) = (a.parse::<u64>(), b.parse::<u64>()) {
                    event(
                        json!({"type":"progress","frame":frame,"total":total,"elapsed":begun.elapsed().as_secs_f64()}),
                    );
                }
            }
        }
    });
    let status = loop {
        if cancel.load(Ordering::Relaxed) {
            kill_tree(&mut child);
            let _ = reader.join();
            bail!("任务已取消")
        };
        if let Some(s) = child.try_wait()? {
            break s;
        };
        std::thread::sleep(Duration::from_millis(50));
    };
    let _ = reader.join();
    if !status.success() {
        bail!(
            "Phi-Recorder渲染失败：{}",
            std::fs::read_to_string(log).unwrap_or_default()
        )
    }
    if j.kind == "preview" {
        if width != j.width || height != j.height {
            let mut c = Command::new(&ff);
            c.args(["-y", "-v", "error", "-i"])
                .arg(png)
                .arg("-vf")
                .arg(format!("scale={}:{}:flags=lanczos", j.width, j.height))
                .arg(&j.output);
            run_quiet(&mut c, &stage.0, cancel)?;
        } else {
            std::fs::copy(png, &j.output)?;
        }
        event(
            json!({"type":"preview","path":j.output,"title":doc["META"]["name"],"level":doc["META"]["level"],"duration":duration,"time":j.time}),
        );
    } else {
        event(json!({"type":"complete","path":j.output}));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn tempo_changes_and_negative_beats_are_absolute() {
        let map = [(0., 120.), (4., 240.)];
        assert_eq!(seconds(-2., &map), -1.);
        assert_eq!(seconds(4., &map), 2.);
        assert_eq!(seconds(8., &map), 3.);
        assert_eq!(beat(&json!([2, 1, 2])).unwrap(), 2.5);
        assert!(beat(&json!([0, 1, 0])).is_err());
    }
    #[test]
    fn builtin_font_family_is_read_instead_of_assumed() {
        assert_eq!(
            font_family(Path::new(
                "apps/desktop/src-tauri/resources/saira-regular.ttf"
            ))
            .unwrap(),
            "Saira"
        );
    }
    #[test]
    fn audio_staging_keeps_float_peaks_without_normalization() {
        let stage = Stage::new().unwrap();
        let p = stage.0.join("mix.wav");
        write_wave(&p, &[2.0, -1.5, 0.25, 0.]).unwrap();
        let b = std::fs::read(p).unwrap();
        assert_eq!(&b[20..24], &[3, 0, 2, 0]);
        assert_eq!(f32::from_le_bytes(b[44..48].try_into().unwrap()), 2.);
        assert_eq!(f32::from_le_bytes(b[48..52].try_into().unwrap()), -1.5);
    }
}
