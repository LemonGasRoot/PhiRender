use crate::{Error, Result};
use serde_json::Value;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChartFormat {
    OfficialV3,
    Rpe,
    Rpen,
}

#[derive(Clone, Debug)]
pub struct ParsedChart {
    pub format: ChartFormat,
    pub document: Value,
}

pub fn detect_format(value: &Value) -> Result<ChartFormat> {
    if value.get("formatVersion").and_then(Value::as_u64).is_some()
        && value.get("judgeLineList").is_some()
        && value.get("META").is_none()
    {
        return if value["formatVersion"].as_u64() == Some(3) {
            Ok(ChartFormat::OfficialV3)
        } else {
            Err(Error::Unsupported(format!(
                "official formatVersion {} (only v3 is implemented)",
                value["formatVersion"]
            )))
        };
    }
    if value.get("META").is_some()
        && value.get("BPMList").is_some()
        && value.get("judgeLineList").is_some()
    {
        let version = value["META"]["RPEVersion"].as_u64().unwrap_or(0);
        return Ok(
            if version >= 220
                || value.get("blockAreaList").is_some()
                || value.get("noiseAreaLayers").is_some()
            {
                ChartFormat::Rpen
            } else {
                ChartFormat::Rpe
            },
        );
    }
    Err(Error::Unsupported("unrecognized JSON chart schema".into()))
}

pub fn parse_document(text: &str) -> Result<ParsedChart> {
    let value: Value = serde_json::from_str(text.trim_start_matches('\u{feff}'))?;
    let format = detect_format(&value)?;
    Ok(ParsedChart {
        format,
        document: value,
    })
}
