# Dual-engine desktop integration

The current desktop GUI defaults to `recorder`. Settings → 渲染引擎 selects `legacy` for the original Phira-based engine. Schema version 10 migrates existing preferences without resetting skin, font, sound, volume, resolution, theme or AA settings.

The desktop worker keeps its existing job/event protocol. The recorder adapter stages archive resources and the selected skin, reads the actual font family, applies the chart delay to a private JSON copy, and calls the bundled native renderer. It stages audio with the same static music/hit gains and non-normalizing addition as the original exporter; it does not use the native engine's RMS balancing. Audio-only export keeps the existing path. NVENC is passed through to h264_nvenc, with the existing software alternative. Both paths still perform GPU-to-CPU readback.

Old engine official holds now have independent duration-based bodies. New standard and archive conversions preserve original hold speeds in `phiRenderOfficialHold`. Native RPE holds are unchanged. Existing standard files missing those values need reconversion from the official source.

The native renderer uses supersampling for the AA quality selector; the legacy engine keeps MSAA. The UI labels this explicitly. All Phira experimental settings remain available through legacy; the recorder currently maps flowSpeed, noteScale and backgroundBrightness and reports unsupported overrides rather than ignoring them.

## Validation performed

- Rust check/build; 9 converter/archive tests, 3 adapter tests, one legacy hold regression and a desktop preference migration/persistence test passed.
- Actual jobs: new/old still previews, old engine video export, new NVENC export with audio and supersampling, no-audio export, user ZIP skin at 8x AA, embedded Saira fallback, and cancellation after GPU progress.
- Music-muted hit-volume test: doubling the setting produced amplitude ratio 1.999358; no automatic normalization.
- Packaged worker ran using its own runtime assets/FFmpeg rather than the development directory.
- GUI was actually launched, its default recorder selection observed, switched to legacy and restored to recorder. Other saved preferences remained unchanged.

## Build

Build the native snapshot with tools/build-recorder.ps1 (MinGW-w64), or pass an already-patched source checkout via -SourceDirectory. Build the Rust worker/CLIs and Tauri GUI with Cargo, then place phirender-recorder.exe, libwinpthread-1.dll and recorder-assets next to the desktop worker. Both workers need the existing Phira runtime assets and FFmpeg under the packaged project root. See the local package instructions for exact directory layout.

The native upstream does not include a standalone LICENSE. The current package is a local development build; public redistribution needs upstream license clarification and the existing asset-license review.
