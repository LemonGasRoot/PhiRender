# Third-party assets and notices

## Project-generated fallback skin

`assets/respack-open/` contains original geometric textures and synthesized sound cues generated for this project by `tools/generate_open_respack.py`. They are dedicated to the public domain under CC0 1.0; see `assets/respack-open/LICENSE.txt`. These files were not copied from Phigros or PhiVideo.

## PhiVideo source audit

PhiVideo source code is offered under MIT; its repository README separately states that files under `resources/` belong to Nanjing Pigeonplay Network Co., Ltd. and that the repository author does not own their copyrights. This project inspected those resources and their loading code, but does not copy or redistribute PhiVideo's images, audio, or fonts. See the [PhiVideo repository](https://github.com/phigrostl/PhiVideo) and [MIT license](https://github.com/phigrostl/PhiVideo/blob/main/LICENSE).

## Existing resource packs

The legacy `assets/respack/` files in the upstream checkout have no separate redistribution grant in this repository. They must not be included in a public distribution unless permission is confirmed. The renderer's fallback path uses `assets/respack-open/` instead.

## Existing fonts

The upstream checkout also contains `assets/font.ttf`, `assets/harmonyos.ttf`, `assets/bold.ttf`, and `assets/phigros.ttf`, which are used by other Phira Pro UI/rendering code. Their individual source and redistribution terms are not documented in this checkout's notices. This skin change does not add or copy these fonts; confirm their licenses or exclude/replace them before a public distribution of the complete application.

## Saira Regular default gameplay font

The application can load `saira-regular.ttf` beside the executable and uses it as the primary gameplay HUD font, with the existing runtime font as fallback for glyphs Saira does not include. The supplied Saira family is authored by Héctor Gatti and the Omnibus-Type team and is licensed under the SIL Open Font License 1.1. The font and `OFL.txt` are distributed together in the GUI delivery folder. Source: https://github.com/Omnibus-Type/Saira.
