# PhiVideo hit-effect alignment

Reference source: PhiVideo Application/RenderFrame.cpp RenderEffects(), LoadFiles.cpp LoadFxImgs() / clickEffectCollection construction, and HitFx.cpp Particles(). The atlas in the local default skin is byte-identical to PhiVideo resources/Notes/HitFx.png (SHA256 F9DC0BB3B6173D8364094D317AFD71E43D90DB1A8D8BB1FFBC6EE10E86F0D409). No new artwork was copied.

Changes in phi-recorder:
- 0.5-second animation; atlas frame floor(progress*frameCount), row-major top-to-bottom (equivalent to PhiVideo's flipped source and reversed row extraction).
- Width = noteWidth * 1.375 * 1.12; preserve per-frame aspect ratio.
- Screen-aligned flash, source-over blending with atlas RGB multiplied by gold. Use baked atlas alpha, without extra fading, additive blending, or repeat attenuation.
- Four fixed-size axis-aligned particles, reference size 33*0.75*width/4040*3; radius 185..265 in reference units; travel curve 9p/(8p+1); linear alpha 1-p. Seeded MT19937 stream is precomputed, preserving deterministic seeks and segment rendering.
- Hold ticks use 16 official ticks / half a quarter-note beat, evaluated through the RPE tempo map rather than fixed 0.15 seconds. PhiVideo's duplicated initial hold tick is retained explicitly.
- Preserve compositing order: flash then particles, per chronological effect, ahead of active noise compositing.

Validation:
- Actual PhiVideo desktop CLI rendered the original DS IN at 50.000 seconds. Its background conversion could not invoke ffmpeg in that run, so the reference has a black background; the new GPU engine was also rendered with black background for an isolated effect comparison. The new frame has matching flash placement/orientation/scale and seeded particle layout. CPU/GPU rasterization, rounding and antialiasing can still differ by pixels; this is not a claim of full-frame byte identity.
- hitfx_regression.cpp: flash width, atlas timing, particle size/travel, radius range and deterministic RNG passed.
- rpe_regression.cpp: real original/converted DS IN hold re-emission schedules compared, plus existing 48,180 line samples, original hold-body regressions and noise geometry checks passed.
- Windows executable rebuilt; full 1920x1080/60fps output has 9045 video frames, music and hits, 150.75 seconds. End-to-end 98.816 seconds on this machine, including another single-frame GPU comparison during the run.

Scope: phi-recorder in this directory. The separate Phira-based PhiRender GPU worker was not changed.
