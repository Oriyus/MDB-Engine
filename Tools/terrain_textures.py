# Blender script: the example terrain layer textures (Examples/Assets/Textures/Terrain/{grass,earth,rock,sand}.jpg)
# and their maps, made from the same noise fields so they match the colour exactly:
#   <name>_height.png  grey, 0..1 over the tile (the terrain's height blending and stochastic seams)
#   <name>_rough.png   grey roughness
#   <name>_normal.png  tangent-space normal, OpenGL style (green = up the image), from the height at the
#                      layer's real relief (AMPLITUDE metres over a TILE_METRES tile)
# Each one tiles seamlessly (periodic noise only), 512 x 512, deterministic. Brightness still follows the height
# (stones and rock a little brighter than the gaps between), for layers used without their height map.
#
# Run headless (Microsoft Store Blender: through the launcher, whose stdout is lost, hence the log file):
#   %LOCALAPPDATA%\Microsoft\WindowsApps\BlenderFoundation.Blender_ppwjx1n5r4v9t\blender-launcher.exe
#       --background --factory-startup --python Tools/terrain_textures.py -- <repo folder> <log file>
import os
import sys
import numpy as np
import bpy

N = 512
args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ROOT = args[0] if args else os.path.join(os.path.dirname(__file__), "..")
LOG = args[1] if len(args) > 1 else os.path.join(ROOT, "terrain_textures.log")
OUT = os.path.join(ROOT, "Examples", "Assets", "Textures", "Terrain")
log = open(LOG, "w")


def periodic_noise(rng, cells):
    """Smooth value noise on an N x N image that wraps: a cells x cells random grid, interpolated with wrap-around."""
    g = rng.random((cells, cells)).astype(np.float32)
    t = np.arange(N, dtype=np.float32) * cells / N
    i0 = np.floor(t).astype(int) % cells
    i1 = (i0 + 1) % cells
    f = t - np.floor(t)
    f = f * f * f * (f * (f * 6 - 15) + 10)              # quintic fade: no creases
    rows = g[i0][:, None, :] * (1 - f)[:, None, None] + g[i1][:, None, :] * f[:, None, None]   # (N, 1, cells)
    rows = rows[:, 0, :]
    out = rows[:, i0] * (1 - f)[None, :] + rows[:, i1] * f[None, :]
    return out


def fbm(rng, base, octaves, persistence=0.5):
    total = np.zeros((N, N), np.float32)
    amp, norm, cells = 1.0, 0.0, base
    for _ in range(octaves):
        total += periodic_noise(rng, cells) * amp
        norm += amp
        amp *= persistence
        cells *= 2
        if cells > N:
            break
    return total / norm


def norm01(a):
    return (a - a.min()) / max(a.max() - a.min(), 1e-6)


def colorize(stops, t):
    """t in 0..1 -> RGB through a list of (position, (r, g, b)) colour stops"""
    pos = np.array([s[0] for s in stops], np.float32)
    cols = np.array([s[1] for s in stops], np.float32)
    out = np.zeros(t.shape + (3,), np.float32)
    for c in range(3):
        out[..., c] = np.interp(t, pos, cols[:, c])
    return out


def speckles(rng, count, rmin, rmax):
    """Round pebbles (0..1 mask, soft edged), wrapping across the edges"""
    m = np.zeros((N, N), np.float32)
    yy, xx = np.mgrid[0:N, 0:N].astype(np.float32)
    for _ in range(count):
        cx, cy = rng.random() * N, rng.random() * N
        r = rmin + (rmax - rmin) * rng.random()
        dx = np.abs(xx - cx); dx = np.minimum(dx, N - dx)
        dy = np.abs(yy - cy); dy = np.minimum(dy, N - dy)
        d = np.sqrt(dx * dx + dy * dy) / r
        m = np.maximum(m, np.clip(1.2 - d, 0, 1) ** 0.7 * (0.6 + 0.4 * rng.random()))
    return m


def save(name, rgb):
    rgb = np.clip(rgb, 0, 1)
    img = bpy.data.images.new(name, N, N, alpha=False)
    px = np.ones((N, N, 4), np.float32)
    px[..., :3] = rgb
    img.pixels.foreach_set(px.ravel())
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name + ".jpg")
    img.filepath_raw = path
    img.file_format = 'JPEG'
    img.save(filepath=path, quality=92)
    log.write("wrote %s (mean %.3f)\n" % (path, float(rgb.mean())))


def save_data(name, rgb):
    """A linear data map (no colour transform), PNG so normals keep no JPEG blocks"""
    rgb = np.clip(rgb, 0, 1)
    if rgb.ndim == 2:
        rgb = np.repeat(rgb[..., None], 3, axis=2)
    img = bpy.data.images.new(name, N, N, alpha=False)
    img.colorspace_settings.name = 'Non-Color'
    px = np.ones((N, N, 4), np.float32)
    px[..., :3] = rgb
    img.pixels.foreach_set(px.ravel())
    path = os.path.join(OUT, name + ".png")
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save(filepath=path)
    log.write("wrote %s (mean %.3f)\n" % (path, float(rgb.mean())))


TILE_METRES = 4.0                                    # the size one tile is drawn at (the demo uses 3 to 7 m)
AMPLITUDE = {"grass": 0.025, "earth": 0.03, "rock": 0.12, "sand": 0.015}   # metres from the lowest to the highest


def normal_map(height, amplitude):
    """Tangent-space normals from a 0..1 height field that wraps. Row 0 is the bottom of the image (Blender's pixel
    order), so +rows is +V: OpenGL style."""
    texel = TILE_METRES / N
    h = height * amplitude
    du = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) / (2 * texel)
    dv = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) / (2 * texel)
    n = np.stack([-du, -dv, np.ones_like(h)], axis=2)
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return n * 0.5 + 0.5


def grass():
    rng = np.random.default_rng(11)
    base = fbm(rng, 4, 6, 0.55)                     # patches of lighter and darker turf
    blades = fbm(rng, 64, 3, 0.6)                    # fine blade texture
    dry = norm01(fbm(rng, 3, 4, 0.5))
    t = norm01(base * 0.55 + blades * 0.45)
    rgb = colorize([(0.0, (0.10, 0.16, 0.05)), (0.45, (0.22, 0.33, 0.10)), (0.8, (0.34, 0.45, 0.16)), (1.0, (0.45, 0.52, 0.22))], t)
    straw = colorize([(0.0, (0.30, 0.27, 0.12)), (1.0, (0.55, 0.50, 0.26))], t)
    w = np.clip((dry - 0.62) * 3.0, 0, 1)[..., None] * 0.6
    height = norm01(blades * 0.7 + base * 0.3)
    rough = 0.82 + 0.12 * (1 - height) + 0.06 * w[..., 0] / 0.6
    return rgb * (1 - w) + straw * w, height, rough


def earth():
    rng = np.random.default_rng(23)
    base = norm01(fbm(rng, 4, 6, 0.55))
    grain = fbm(rng, 96, 2, 0.5)
    t = np.clip(base * 0.7 + grain * 0.3, 0, 1)
    rgb = colorize([(0.0, (0.16, 0.11, 0.07)), (0.5, (0.30, 0.22, 0.14)), (1.0, (0.42, 0.33, 0.22))], t)
    stones = speckles(rng, 260, 2.0, 6.5)
    stone_col = colorize([(0.0, (0.36, 0.33, 0.30)), (1.0, (0.58, 0.55, 0.50))], norm01(fbm(rng, 32, 2)))
    s = stones[..., None]
    height = norm01(t * 0.35 + stones * 0.9)
    rough = 0.96 - stones * 0.3
    return rgb * (1 - s) + stone_col * s, height, rough


def rock():
    rng = np.random.default_rng(37)
    base = norm01(fbm(rng, 3, 7, 0.55))
    ridged = 1.0 - np.abs(fbm(rng, 6, 5, 0.5) * 2 - 1)      # sharp crease lines
    cracks = np.clip((ridged - 0.93) * 10.0, 0, 1)
    layers = 0.5 + 0.5 * np.sin((np.arange(N)[:, None] / N * 2 * np.pi * 7) + base * 6.0)   # faint strata (wraps: whole periods)
    t = np.clip(base * 0.75 + layers * 0.15 + fbm(rng, 128, 2) * 0.1, 0, 1)
    rgb = colorize([(0.0, (0.24, 0.23, 0.22)), (0.5, (0.42, 0.41, 0.39)), (1.0, (0.62, 0.60, 0.56))], t)
    lichen = np.clip((norm01(fbm(rng, 5, 5)) - 0.72) * 3.0, 0, 1)[..., None] * 0.22
    rgb = rgb * (1 - lichen) + np.array([0.46, 0.48, 0.34], np.float32) * lichen
    height = norm01(t - cracks * 0.6)
    rough = 0.68 + 0.22 * (1 - t) + lichen[..., 0] * 0.4
    return rgb * (1 - cracks[..., None] * 0.45), height, rough


def sand():
    rng = np.random.default_rng(53)
    warp = fbm(rng, 4, 4, 0.5)
    yy = np.arange(N, dtype=np.float32)[:, None] / N
    ripples = 0.5 + 0.5 * np.sin(2 * np.pi * (yy * 18 + warp * 2.5))   # whole periods across the tile: wraps
    grain = fbm(rng, 256, 1)
    t = np.clip(norm01(fbm(rng, 3, 5)) * 0.45 + ripples * 0.25 + grain * 0.3, 0, 1)
    rgb = colorize([(0.0, (0.55, 0.47, 0.32)), (0.5, (0.72, 0.64, 0.46)), (1.0, (0.86, 0.79, 0.62))], t)
    height = norm01(ripples * 0.6 + t * 0.4)
    rough = 0.94 - ripples * 0.08
    return rgb, height, rough


try:
    for name, fn in (("grass", grass), ("earth", earth), ("rock", rock), ("sand", sand)):
        rgb, height, rough = fn()
        save(name, rgb)
        save_data(name + "_height", height)
        save_data(name + "_rough", rough)
        save_data(name + "_normal", normal_map(height, AMPLITUDE[name]))
    log.write("done\n")
except Exception as e:
    import traceback
    log.write("ERROR: %s\n%s" % (e, traceback.format_exc()))
log.close()
