"""Inspect a GPU effects dump: hard edge R must be binary, glow G retains gradients.
Usage: python check_noise_edges.py <ffmpeg.exe> <effects.png>
Use phi-recorder --time 4 --debug-noise PREFIX with the DS IN regression chart.
"""
import subprocess, sys
raw = subprocess.run([sys.argv[1], '-v', 'error', '-i', sys.argv[2], '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], check=True, capture_output=True).stdout
edge, glow = set(raw[0::3]), set(raw[1::3])
assert edge == {0, 255}, f'Interpolated edge values: {sorted(edge)}'
assert len(glow) > 2, f'Glow lost its gradient: {sorted(glow)}'
print(f'PASS: GPU edge has only {sorted(edge)}; glow retains {len(glow)} intensity levels')
