# Phigros → RPEN converter

Rust library and command-line converter for official Phigros v3 charts. Standard output (the default) contains the mapped RPEN runtime chart without a duplicate source document. Archive output additionally preserves the complete original official document under `rpeNextLegacySource.document` for debugging and recovery.

## CLI

```text
phigros-rpen INPUT OUTPUT [--mode standard|archive]
```

Examples:

```powershell
phigros-rpen chart.json chart.rpe.json
phigros-rpen chart.pez converted.pez
phigros-rpen input.zip output.zip
phigros-rpen chart.json chart-archived.rpe.json --mode archive
```

Official JSON is converted. Ordinary RPE/RPEN JSON is detected and passed through unchanged. `--mode standard` is the default and emits only RPEN fields needed for playback, including all mapped notes, judge-line tracks, and noise-area geometry and animation. `--mode archive` additionally embeds the entire official source JSON. For PEZ/ZIP input, the converter uses `info.txt`/`info.yml` chart references when multiple recognized chart JSON files exist, replaces only the selected JSON at its existing path, and raw-copies every other archive member. Output is reopened and checked for valid CRCs, a parseable chart, and byte-identical non-chart assets. Ambiguous or unrecognized archives are reported as errors.

## Library

```rust
use phigros_rpen_converter::{convert_json, convert_json_with_mode, parse_document, OutputMode};

let input = std::fs::read_to_string("chart.json")?;
let parsed = parse_document(&input)?;
let (output_json, report) = convert_json(&input)?;
let (debug_json, _) = convert_json_with_mode(&input, OutputMode::Archive)?;
println!("{:?}: {} notes", parsed.format, report.notes);
std::fs::write("chart.rpe.json", output_json)?;
# Ok::<(), Box<dyn std::error::Error>>(())
```

The library API is available as `convert_json_with_mode` and `archive::convert_archive_with_mode`; each defaults to standard mode through `convert_json` and `archive::convert_archive`.

## Scope and fidelity

Implemented: official v3 BPM/time conversion, judge lines, Tap/Drag/Hold/Flick notes, line speed/move/rotate/alpha events, block/noise area geometry, active intervals, movement/scale/rotation keys, metadata, and archive asset preservation. Event keyframes and mapped values follow RPEN 0.13.0 import behavior, including alpha integer conversion and special noise easing handling. Both output modes serialize compact JSON. Archive-only source metadata is not required by the RPEN renderer.

Warnings in the conversion report identify fidelity limits. Future or unknown official fields are omitted from standard runtime output with a warning; archive mode preserves them in the original-source field, but they are not guaranteed to affect playback. Non-chart ZIP members retain their compressed data and metadata. The rewritten chart retains its original path, compression method, timestamp, and permissions. ZIP entry comments on the rewritten chart cannot be retained by the current ZIP library and are reported. Encrypted or nonstandard containers are not supported.

## Licensing

This project’s converter behavior is based on `kclg-YSJ/rpe-next` 0.13.0, licensed under PolyForm Noncommercial 1.0.0. The corresponding license is included in `LICENSE`; use and redistribution must follow its terms. Third-party Rust dependencies retain their own licenses.
