# PhiRender Desktop

This is the PhiRender Tauri 2 / React desktop application. The original Rust
converter and CLI entry points remain in the repository root. The desktop
backend calls the converter library directly. The separate
`phirender-gpu-worker` program owns the existing Macroquad/OpenGL context and
calls `ChartVideoRenderer` directly. It is not the Phira Pro application or a
wrapper around the command-line renderer.

## Current capabilities

- Open official, RPE and RPEN JSON/PEZ and display conversion diagnostics.
- Automatically generate a playable full-chart 720p/30 FPS preview video on
  import, or turn automatic generation off and start it manually. The preview
  uses the same GPU drawing and encoding path as video export; GPU frames are
  read back before encoding. Playback starts after generation completes.
- Export MP4 in a background GPU worker using the existing FFmpeg software or
  NVENC encoder. Cancellation requests the worker to stop and remove its
  temporary output. Existing destination files are not overwritten.
- Import a local Phira skin directory or ZIP and a local TTF/OTF gameplay
  font; choose Tap/Hold, Drag and Flick sounds independently and adjust gain.
  Sound files are prepared in a temporary resource pack before the existing
  renderer loads them. Chart-specific sounds still use the renderer's original
  behavior, and its existing peak normalization may compress volume changes.
- Persist theme and export/skin settings. The default skin uses the existing
  local `work/Phira-Pro/assets/respack`; the project's generated skin is not
  selected as the desktop default.

The generated video supports playback, pause, and seeking. Live rendering and
audio monitoring during generation are not implemented. The existing older
GUI and CLI have not been removed. NVIDIA NVENC is the default encoder; select
software H.264 in settings if it is unavailable. On this development machine,
the desktop backend uses the bundled FFmpeg 8 for NVENC compatibility.

## Build on Windows

Install Node.js, Rust, the Windows C++ build tools and the WebView2 runtime.
The recommended Tauri Windows toolchain is MSVC. From the repository root:

```powershell
cd apps/desktop
npm ci
npm run build
cd ../..
cargo build --release --bin phirender-gpu-worker
cargo build --release --features custom-protocol --manifest-path apps/desktop/src-tauri/Cargo.toml
```

Put `phirender-desktop.exe` and `phirender-gpu-worker.exe` together. Put the
optional `saira-regular.ttf` and its OFL notice next to them for the default
gameplay font. The GNU Windows build also needs the matching x64
`WebView2Loader.dll` beside the desktop executable and an installed WebView2
Runtime. On this development machine the program finds the existing
`work/Phira-Pro/assets` and FFmpeg in the project tree. It is therefore a
development build rather than a fully portable public distribution. Custom
skins and fonts are read from the selected paths and are not included in the
package.

## Licensing

Do not label the combined project GPL-3.0-only without resolving the existing
license conflict. The root converter declares PolyForm Noncommercial 1.0.0;
the reused Phira Pro rendering code is GPL-3.0. The intended public
distribution model needs rights/compatibility review and may require replacing
or obtaining different permission for the noncommercial converter. The
existing `work/Phira-Pro/assets/respack` and several bundled upstream fonts
do not have a documented separate redistribution grant in this checkout;
the desktop uses those files locally rather than copying them into a public
release. Saira is accompanied by its SIL OFL 1.1 notice. Tauri is MIT/Apache-2.0;
frontend packages retain their own licenses. See the root `LICENSE` and
`work/Phira-Pro/THIRD_PARTY_NOTICES.md`.
