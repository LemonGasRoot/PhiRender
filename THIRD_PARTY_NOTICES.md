# Third-party notices

This file identifies major bundled, referenced, or source-derived components. It is not a replacement for each component's full license text. Rust and frontend dependency licenses remain those of their respective upstream projects.

## Phira Pro renderer

PhiRender's rendering code is based on Phira Pro, licensed under GPL-3.0-only. The development source is restored from the upstream commit recorded in `vendor-snapshots/phira-pro/BASE_COMMIT.txt`; `vendor-snapshots/phira-pro/local-changes.patch` contains PhiRender's local changes. See [Phira Pro](https://github.com/Phira-Pro/Phira-Pro) and its included GPL license. The project source is distributed under GPL-3.0-only. Any combined application or binary distribution must comply with GPL-3.0 obligations.

## Official chart conversion

The converter includes code and conversion behavior derived from RPEN/Re:PhiEdit Next 0.13.0, whose upstream project is licensed under PolyForm Noncommercial 1.0.0. The RPEN maintainer explicitly authorized PhiRender to use, modify, and distribute the relevant code with this project under GPL-3.0-only in a Bilibili reply dated 2026-10-29; the project owner retains the exchange as evidence. PhiRender is maintained for noncommercial use. The GPL-3.0-only grant itself carries the rights stated in GPL-3.0 for recipients. This project-specific permission does not change the license of the separate rpe-next upstream repository. See [rpe-next](https://github.com/kclg-YSJ/rpe-next).

## PhiVideo

PhiVideo source is MIT-licensed. It was inspected as a rendering reference; PhiRender does not bundle its source or its `resources/` assets. The PhiVideo project states that those assets belong to Nanjing Pigeonplay Network Co., Ltd. See [PhiVideo](https://github.com/phigrostl/PhiVideo).

## Fonts and skins

- The GUI and Windows release bundle Saira Regular under the SIL Open Font License 1.1. The release also uses this font as a fallback for renderer font files. Its license notice is at `apps/desktop/src-tauri/resources/Saira-OFL.txt`; source: [Omnibus-Type/Saira](https://github.com/Omnibus-Type/Saira).
- `vendor-snapshots/phira-pro/untracked/assets/respack-open/` is a project-generated fallback skin dedicated to the public domain under CC0 1.0. Its notice is included as `LICENSE.txt` in that directory. The source generator is `vendor-snapshots/phira-pro/untracked/tools/generate_open_respack.py`.
- The Windows release includes the Phira Pro runtime images required by the renderer (rank, modifier, and gameplay UI assets) from its GPL-3.0-only source tree. It excludes upstream fonts, sample charts, songs, and undocumented skins; Saira and the CC0 fallback skin are used instead.
- The RPEN noise-skin and shader resources included in the Windows release retain their upstream PolyForm Noncommercial 1.0.0 license and notices (`assets/rpen/LICENSE-RPEN.txt` and `assets/rpen/NOTICE.txt`). They are separate from the RPEN conversion-code permission described above.
- Other upstream Phira Pro skins and fonts whose redistribution terms are not documented here are not copied into this repository or release packages. Users must review the upstream terms before redistributing them.
- User-provided chart archives, skins, fonts, sound effects, and illustrations are not included in the source repository.

## Other software

- The Windows release includes the x64 `WebView2Loader.dll` distributed with the WebView2 SDK. Microsoft documents that this loader must be shipped with WebView2 applications; the Microsoft Edge WebView2 Runtime is a separate prerequisite and is not included. See [Microsoft's WebView2 distribution guidance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution).
- Tauri and associated Rust crates retain their upstream licenses.
- React, Vite, TypeScript, and other npm packages retain their upstream licenses. See `apps/desktop/package-lock.json` and each package's license files.
- FFmpeg is an external runtime dependency and retains the license of the build selected by the user or distributor. A release package must identify the exact FFmpeg build and comply with its configuration-specific license.

