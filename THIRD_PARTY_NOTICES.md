# Third-party notices

This file identifies major bundled, referenced, or source-derived components. It is not a replacement for each component's full license text. Rust and frontend dependency licenses remain those of their respective upstream projects.

## Phira Pro renderer

PhiRender's rendering code is based on Phira Pro, licensed under GPL-3.0-only. The development source is restored from the upstream commit recorded in `vendor-snapshots/phira-pro/BASE_COMMIT.txt`; `vendor-snapshots/phira-pro/local-changes.patch` contains PhiRender's local changes. See [Phira Pro](https://github.com/Phira-Pro/Phira-Pro) and its included GPL license. If distributing a combined application or binary, comply with GPL-3.0 obligations and resolve its interaction with the converter's separate license first.

## Official chart conversion

The converter behavior follows RPEN/Re:PhiEdit Next 0.13.0. The top-level `LICENSE` contains the PolyForm Noncommercial 1.0.0 terms. The RPEN maintainer replied that use is limited to noncommercial purposes (Bilibili reply dated 2026-10-28; the project owner retains the conversation as evidence). This permission does not by itself change the code's license to GPL or authorize commercial use. See [rpe-next](https://github.com/kclg-YSJ/rpe-next).

## PhiVideo

PhiVideo source is MIT-licensed. It was inspected as a rendering reference; PhiRender does not bundle its source or its `resources/` assets. The PhiVideo project states that those assets belong to Nanjing Pigeonplay Network Co., Ltd. See [PhiVideo](https://github.com/phigrostl/PhiVideo).

## Fonts and skins

- The GUI bundles Saira Regular under the SIL Open Font License 1.1. Its license notice is at `apps/desktop/src-tauri/resources/Saira-OFL.txt`; source: [Omnibus-Type/Saira](https://github.com/Omnibus-Type/Saira).
- `vendor-snapshots/phira-pro/untracked/assets/respack-open/` is a project-generated fallback skin dedicated to the public domain under CC0 1.0. Its notice is included as `LICENSE.txt` in that directory. The source generator is `vendor-snapshots/phira-pro/untracked/tools/generate_open_respack.py`.
- The upstream Phira Pro checkout contains other skins, fonts, and runtime assets whose redistribution terms are not documented here. They are not copied into this repository or release packages. Users must review the upstream terms before redistributing them.
- User-provided chart archives, skins, fonts, sound effects, and illustrations are not included in the source repository.

## Other software

- Tauri and associated Rust crates retain their upstream licenses.
- React, Vite, TypeScript, and other npm packages retain their upstream licenses. See `apps/desktop/package-lock.json` and each package's license files.
- FFmpeg is an external runtime dependency and retains the license of the build selected by the user or distributor. A release package must identify the exact FFmpeg build and comply with its configuration-specific license.

