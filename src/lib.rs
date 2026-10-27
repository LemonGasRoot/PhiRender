//! Official Phigros v3 to RPEN chart conversion.
//!
//! This library keeps the original official document in `rpeNextLegacySource` so
//! fields not represented in the current conversion model remain recoverable.

pub mod archive;
pub mod converter;
pub mod error;
pub mod model;
pub mod parser;

pub use converter::{
    convert_official, convert_official_with_mode, ConversionReport, Diagnostic, OutputMode,
};
pub use error::{Error, Result};
pub use parser::{detect_format, parse_document, ChartFormat, ParsedChart};

/// Convert an official v3 JSON string to RPEN-compatible JSON.
pub fn convert_json(input: &str) -> Result<(String, ConversionReport)> {
    convert_json_with_mode(input, OutputMode::Standard)
}

/// Convert official v3 JSON, selecting whether to archive the original source.
/// Existing RPE and RPEN JSON is returned unchanged for either mode.
pub fn convert_json_with_mode(input: &str, mode: OutputMode) -> Result<(String, ConversionReport)> {
    let parsed = parse_document(input)?;
    match parsed.format {
        ChartFormat::OfficialV3 => {
            let (chart, report) = convert_official_with_mode(&parsed.document, mode)?;
            // Compact serialization avoids expanding the embedded source chart
            // by an order of magnitude on large noise-heavy charts.
            Ok((serde_json::to_string(&chart)?, report))
        }
        ChartFormat::Rpe | ChartFormat::Rpen => Ok((input.to_owned(), ConversionReport::default())),
    }
}
