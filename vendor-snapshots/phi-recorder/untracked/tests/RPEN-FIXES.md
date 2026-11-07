# RPEN importer regression fixes (2026-11-07)

Changed only phi-recorder in this directory. The main PhiRender rendering core is unchanged.

## Root causes

- RPEN rotateEvents use the opposite sign from the native official model. Import now negates the line rotation.
- The official-to-RPEN converter uses speed * 4.5. Import incorrectly divided by 10, making travel 45% of the intended speed. Import now divides by 4.5.
- RPEN hold notes carry a per-note speed. The native hold drawing path ignored it. Apply it for RPEN holds, preserving the native official path.
- Noise rectangle half-extents were passed as full extents: zones became half-sized on each axis.
- Anchored noise operations were projected before rotation. Evaluate history in the 1350x900 canvas, then apply 1.5/aspect to Y after rotation.
- Noise transformStart/transformEnd history adjustments were omitted, and rotation direction was reversed.
- An empty activeIntervals array incorrectly activated the complete lifetime. It now remains Disabled.
- Dense speed-burst charts amplified float event timestamps into visible floor-position errors. Beat conversion and event timestamps now use double; integration accumulates in double.
- Noise event sorting now preserves simultaneous step order; easing bounds and cubic Bezier are evaluated.
- META offset is applied to playback and note/noise absolute timestamps.

## Validation

`tests/rpe_regression.cpp` loads the actual original official chart and its converted RPEN chart. Counts are compared dynamically. It samples all 44 lines at 48,180 time/line combinations, checking X/Y, rotation, instantaneous speed and integrated floor position. Maximum floor difference: 0.00324249 internal units. The original official implementation still uses float accumulation, so this comparison is tolerant rather than bitwise equality.

Independent analytic checks cover full-screen noise rectangle corners, aspect projection, rotation around an off-center anchor, transform-reference metadata, empty activation intervals, and partial/full speed-ramp integration.

The 50-second rendered frame has also been inspected against the existing PhiRender reference: line direction and hold placement now agree. This does not establish identical hit particles or identical shader output.

## Scope

The real DS IN fixture has one event layer per line. This validation does not establish correctness for general RPE parent hierarchies, multiple additive event layers, custom noise-layer shaders, or nonlinear speed events. Those pre-existing importer limitations require separate coverage.

## Run regression

Build `tests/rpe_regression.cpp` with `src/chart.cpp`, `src/rpe.cpp`, `third_party/cJSON.c`, and include paths `src`, `third_party`. Run the resulting executable with two arguments: original official JSON and converted RPEN JSON.

## Hard noise-edge sampling fix

NoiseField/config.json specifies linear filtering only for `effects`. The RPEN skin loader (`prpr/src/core/rpen_skin.rs`) treats an omitted filter as nearest. The C++ implementation incorrectly enabled linear filtering on activeCompose, disabledCompose, both glow ping-pong targets and sceneColor. Restore nearest for those targets. Keep effects linear: main.glsl explicitly samples its red edge at texel centers while allowing smooth interpolation of the green glow channel.

Validated at DS IN 4.000 seconds, 1920x1080, using a GPU debug dump. `check_noise_edges.py` verifies that effects.R contains exactly {0,255}, while effects.G retains 6 intensity levels. Before/after screenshots confirm that the angular silhouette is restored while glow remains soft. No shader equations or chart motion parameters changed.

## Official hold duration model

Native official holds no longer use the RPE integral tail. Their head scrolls with the judge line, independently of the original hold speed. Body distance = max(0, tailTime - max(now, hitTime)) * originalHoldSpeed * PGRH. The renderer applies output height and flow speed, then attaches the ordinary head/tail artwork. Native RPE holds without compatibility metadata retain the existing integral model.

The Rust converter retains `phiRenderOfficialHold: {"version":1,"speed":<original note speed>}` on converted hold notes in both standard and archive modes. The ordinary RPE `speed` remains 1. This is a PhiRender compatibility extension, not a claim that RPEN itself implements duration-based bodies; third-party editors/renderers can ignore or remove this extension. Existing standard outputs lacking original speed need reconversion from the official source. No chart-specific values are guessed.

`tests/rpe_regression.cpp` now compares every original hold against its converted counterpart, including speed preservation and body length before hit, at hit, at 25/50/75% progress, at tail and after tail. Independent duration tests and native-RPE mode separation are included. Rust `official_hold_length_metadata_is_independent_of_rpe_scroll_speed` verifies both output modes and confirms ordinary notes have no hold marker. All 9 conversion/archive tests passed.

This change applies to the phi-recorder executable in this directory and the Rust conversion library. The separate Phira-based PhiRender GPU worker has not been changed to consume the compatibility extension.
