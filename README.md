# PhiRender

![PhiRender logo](apps/desktop/src/assets/phirender-logo.svg)

PhiRender is a Windows desktop app and command-line toolkit for converting and rendering Phigros charts. It accepts official Phigros charts and RPE/RPEN charts, converts official charts to RPEN when needed, and renders previews or video using a GPU drawing pipeline.

> **Source license:** PhiRender source code is GPL-3.0-only. The RPEN maintainer explicitly confirmed that the RPEN-derived conversion code may be used, modified, and distributed with PhiRender under GPL-3.0-only. PhiRender is maintained for noncommercial use; GPL recipients still receive the rights stated in GPL-3.0, including commercial use. Third-party fonts, assets, and dependencies retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## What it includes

- **PhiRender GUI** — open JSON/PEZ charts, inspect chart information and conversion diagnostics, generate a preview, edit export metadata, and export video.
- **CLI renderer** — convert official charts as needed and render video, with segment export, sound controls, skins, fonts, anti-aliasing, and software or NVIDIA NVENC encoding.
- **Pure RPEN renderer** — render an existing RPEN chart without format conversion.
- **Chart converter library and CLI** — convert official Phigros v3 JSON/PEZ to RPEN, with standard and archive output modes.
- **GPU drawing** — the renderer uses Macroquad/OpenGL for drawing. Frames are read back to CPU memory before FFmpeg encodes them. NVENC accelerates encoding only; the full video pipeline is not GPU-only.

The renderer is based on the Phira Pro rendering code. The original Phira Pro application is not required at runtime. The GUI and command-line usage details are in [apps/desktop/README.md](apps/desktop/README.md) and [README-CLI.md](README-CLI.md).

## Windows development build

Target platform: Windows 10/11 x64. Install Git, Rust, Node.js, the Windows C++ build tools, and WebView2 Runtime. A first build also downloads Cargo and npm dependencies.

The renderer uses a pinned Phira Pro source checkout with PhiRender changes. Restore it before building:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\restore-phira.ps1
```

The restore script refuses to overwrite an existing `work\Phira-Pro` directory. It checks out the recorded upstream commit, applies the tracked patch, and copies the included project-generated CC0 fallback skin and notices. It does not include or grant redistribution rights for the upstream skin or fonts.

Build the CLI programs and GPU worker:

```powershell
cargo build --release --bin phigros-rpen
cargo build --release --bin phigros-render-cli
cargo build --release --bin phigros-render-pure
cargo build --release --bin phirender-gpu-worker
```

Build the desktop GUI:

```powershell
Push-Location .\apps\desktop
npm ci
npm run build
Pop-Location
cargo build --release --features custom-protocol --manifest-path .\apps\desktop\src-tauri\Cargo.toml
```

The Tauri desktop executable and `phirender-gpu-worker.exe` must be placed together. FFmpeg is required for video export. The CLI can use an FFmpeg path supplied on the command line or through `PHIRA_FFMPEG`; see [README-CLI.md](README-CLI.md). The development renderer also reads Phira Pro runtime assets from `work\Phira-Pro\assets`; those assets are intentionally not copied into this repository.

## Project layout

```text
src/                         Rust converter library and CLI renderers
apps/desktop/                Tauri 2 + React GUI
tests/                       converter semantic tests
vendor-snapshots/phira-pro/  pinned source commit, local patch, and project notices
tools/                       local source restore and snapshot maintenance scripts
README-CLI.md                command-line usage
```

User charts, downloaded assets, generated videos, build outputs, and the local Phira Pro checkout are excluded by `.gitignore`.

## Repository description for GitHub

> A Windows Phigros chart converter and GPU renderer with GUI, CLI, preview, and video export.

Suggested topics: `phigros`, `rhythm-game`, `chart-renderer`, `rust`, `windows`, `gpu-rendering`, `tauri`.
