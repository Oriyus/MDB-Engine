# Abandoned village modular kit for MDB Engine (Blender 4.2+ / 5.x, run headless):
#   blender --background --factory-startup --python Tools/village_kit.py -- <repo> [--preview <folder>] [--log <file>]
# Writes Examples/Assets/Models/Village/*.glb (+ kit.json, which makes the editor place them at their own size),
# and Examples/Assets/Scenes/village.json (a demo village built from the pieces). Deterministic (fixed seeds).
#
# Kit rules (engine axes, Y up; 1 unit = 1 m):
#   module 2 m wide, storey 3 m, walls 0.2 m thick, centred on their line. Pivots:
#   walls / gable / fences  : bottom centre of the wall line, running along X, outside face towards +Z
#   corner_post / fence_post: the grid point the walls meet at
#   floor                   : centre of a 2 x 2 m tile, top at 0.06
#   roof / roof_broken      : at wall-top height (3 m), centre of the 2 m strip between the wall line and the ridge,
#                             eave towards +Z, ridge 1 m towards -Z and 2 m up (45 deg; houses are 4 m deep)
#   roof_ridge              : at wall-top height on the ridge line
#   door                    : its hinge edge, bottom (rotate about Y to open)
#   props                   : bottom centre
# Keep walls, posts and roofs on whole metres: nav cells are 1 m, so a doorway centred on a whole metre has two
# cell centres inside it.

import bpy, bmesh, math, random, sys, os, json, traceback
import numpy as np
from mathutils import Matrix, Vector

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
REPO = argv[0] if argv else os.getcwd()
PREVIEW = argv[argv.index("--preview") + 1] if "--preview" in argv else None
LOG = argv[argv.index("--log") + 1] if "--log" in argv else None
if LOG:
    sys.stdout = sys.stderr = open(LOG, "w", encoding="utf-8", buffering=1)

OUT = os.path.join(REPO, "Examples", "Assets", "Models", "Village")
SCENES = os.path.join(REPO, "Examples", "Assets", "Scenes")
import tempfile
TEX_DIR = os.path.join(PREVIEW or os.path.join(tempfile.gettempdir(), "mdb_village_kit"), "textures")   # sources; the .glb files embed them
S = 512          # texture size
TILE = 2.0       # metres per texture repeat

# ============================================================================ textures (numpy, tileable)
def noise(rng, cu, cv, size=S):
    """Periodic value noise, cu x cv cells over the tile. Returns [v, u]."""
    g = rng.random((cv, cu))
    def axis(cells):
        x = np.arange(size) * cells / size
        i0 = np.floor(x).astype(int)
        f = x - i0
        f = f * f * (3 - 2 * f)
        return i0 % cells, (i0 + 1) % cells, f
    u0, u1, fu = axis(cu)
    v0, v1, fv = axis(cv)
    a = g[np.ix_(v0, u0)]; b = g[np.ix_(v0, u1)]; c = g[np.ix_(v1, u0)]; d = g[np.ix_(v1, u1)]
    fu = fu[None, :]; fv = fv[:, None]
    return (a * (1 - fu) + b * fu) * (1 - fv) + (c * (1 - fu) + d * fu) * fv

def fbm(rng, cu, cv, octaves=4, gain=0.5):
    total, amp, norm = 0.0, 1.0, 0.0
    for i in range(octaves):
        total = total + amp * noise(rng, cu * 2 ** i, cv * 2 ** i)
        norm += amp; amp *= gain
    return total / norm

def mix(a, b, t):
    t = np.asarray(t)[..., None] if np.ndim(t) else t
    return np.asarray(a) * (1 - t) + np.asarray(b) * t

def col(c):
    return np.array(c, dtype=np.float32)[None, None, :]

def tex_wood(rng):
    """Weathered boards: grain along U, 10 boards per tile along V (0.2 m each)."""
    rows = 10
    v = np.arange(S)[:, None]; u = np.arange(S)[None, :]
    rowf = v * rows / S
    row = np.floor(rowf).astype(int) % rows
    fr = rowf - np.floor(rowf)
    G = fbm(rng, 3, 48, 4)
    off = rng.integers(0, S, rows)
    grain = G[np.arange(S)[:, None], (u + off[row]) % S]
    streak = noise(rng, 2, 96)
    shade = rng.uniform(0.78, 1.12, rows)[row]
    grey = rng.uniform(0.0, 0.7, rows)[row]
    base = mix(col((0.40, 0.30, 0.21)), col((0.47, 0.45, 0.41)), grey * (0.6 + 0.4 * noise(rng, 4, 4)))
    lum = shade * (0.72 + 0.45 * grain) * (0.9 + 0.2 * streak)
    img = base * lum[..., None]
    # seams between boards, bevel highlight beside them
    edge = np.minimum(fr, 1 - fr) * S / rows                      # pixels to the board edge
    img *= (0.35 + 0.65 * np.clip(edge / 2.0, 0, 1))[..., None]
    img *= (1.0 + 0.10 * np.clip(1 - np.abs(edge - 3.0) / 2.0, 0, 1))[..., None]
    # butt joints and nails
    for r in range(rows):
        y0, y1 = int(r * S / rows), int((r + 1) * S / rows)
        for _ in range(rng.integers(1, 3)):
            j = rng.integers(0, S)
            for dx in range(-1, 2):
                img[y0:y1, (j + dx) % S] *= 0.35 if dx == 0 else 0.7
            for side in (-6, 6):
                for ny in (y0 + (y1 - y0) // 4, y1 - (y1 - y0) // 4):
                    x = (j + side) % S
                    img[ny - 1:ny + 2, (x - 1) % S:(x - 1) % S + 3] = (0.16, 0.12, 0.10)
        # a knot or two
        for _ in range(rng.integers(0, 2)):
            cx, cy = rng.integers(0, S), rng.integers(y0 + 8, max(y0 + 9, y1 - 8))
            yy, xx = np.ogrid[y0:y1, 0:S]
            dxw = (xx - cx + S // 2) % S - S // 2
            d = (dxw / 9.0) ** 2 + ((yy - cy) / 4.0) ** 2
            img[y0:y1] *= (0.55 + 0.45 * np.clip(d, 0, 1))[..., None]
    return img

def tex_plaster(rng):
    v = np.arange(S)[:, None]; u = np.arange(S)[None, :]
    base = col((0.78, 0.72, 0.60))
    lo = fbm(rng, 3, 3, 4)
    fine = noise(rng, 96, 96)
    img = base * (0.86 + 0.22 * lo + 0.06 * fine)[..., None]
    stain = np.clip((fbm(rng, 4, 4, 5) - 0.5) * 3.0, 0, 1)
    img = mix(img, img * col((0.62, 0.58, 0.50)), stain)
    grime = np.clip((fbm(rng, 6, 3, 4) - 0.45) * 2.0, 0, 1) * 0.5
    img = mix(img, col((0.30, 0.27, 0.22)), grime * 0.5)
    # fallen plaster shows the wattle and daub behind it
    hole = fbm(rng, 5, 5, 5)
    patch = np.clip((hole - 0.66) * 25.0, 0, 1)
    rim = np.clip(1 - np.abs(hole - 0.66) * 40.0, 0, 1)
    daub = col((0.42, 0.31, 0.22)) * (0.7 + 0.5 * noise(rng, 64, 32))[..., None]
    sticks = (np.sin(u * 2 * np.pi * 24 / S + 3 * noise(rng, 2, 8)) > 0.6)
    daub = np.where(sticks[..., None], daub * 0.75, daub)
    img = mix(img, daub, patch)
    img *= (1 - 0.45 * rim)[..., None]
    # cracks: wandering lines
    for _ in range(14):
        x, y = rng.uniform(0, S), rng.uniform(0, S)
        a = rng.uniform(0, 2 * np.pi)
        for _ in range(rng.integers(60, 220)):
            a += rng.normal(0, 0.35)
            x = (x + np.cos(a)) % S; y = (y + np.sin(a)) % S
            img[int(y), int(x)] *= 0.45
            img[int(y), (int(x) + 1) % S] *= 0.8
    return np.clip(img, 0, 1)

def tex_stone(rng):
    """Rubble masonry: 7 courses, blocks of mixed length, dark mortar, some moss."""
    img = np.zeros((S, S, 3), np.float32)
    heights = rng.dirichlet(np.ones(7) * 6) * S
    edges = np.concatenate([[0], np.cumsum(heights)]).astype(int)
    edges[-1] = S
    detail = fbm(rng, 24, 24, 3)
    moss = np.clip((fbm(rng, 5, 5, 4) - 0.55) * 4, 0, 1)
    u = np.arange(S)
    for r in range(7):
        y0, y1 = edges[r], edges[r + 1]
        widths = []
        while sum(widths) < S - 140:
            widths.append(int(rng.integers(60, 130)))
        widths.append(S - sum(widths))
        starts = np.concatenate([[0], np.cumsum(widths)])
        shift = int(rng.integers(0, S))
        rel = (u - shift) % S
        blk = np.searchsorted(starts, rel, side="right") - 1
        du = np.minimum(rel - starts[blk], starts[blk + 1] - rel)
        tint = rng.uniform(0.42, 0.6, len(widths))
        warm = rng.uniform(0.0, 0.06, len(widths))
        for y in range(y0, y1):
            dv = min(y - y0, y1 - y)
            d = np.minimum(du, dv).astype(np.float32)
            t = tint[blk]
            c = np.stack([t + warm[blk], t, t - warm[blk] * 0.5], -1)
            c *= (0.8 + 0.35 * detail[y])[:, None]
            ao = np.clip(d / 7.0, 0, 1)
            c *= (0.55 + 0.45 * ao)[:, None]
            mortar = d < 2.5
            c[mortar] = (0.25, 0.23, 0.20)
            img[y] = c
    img = mix(img, img * col((0.78, 0.84, 0.58)), moss * 0.5)
    return np.clip(img, 0, 1)

def tex_shingles(rng):
    """8 courses per tile (0.25 m), exposed edge at the bottom of each course (low V)."""
    rows = 8
    img = np.zeros((S, S, 3), np.float32)
    rh = S // rows
    grain = fbm(rng, 48, 3, 3)
    moss = np.clip((fbm(rng, 4, 4, 4) - 0.52) * 4, 0, 1)
    u = np.arange(S)
    for r in range(rows):
        widths = []
        while sum(widths) < S - 80:
            widths.append(int(rng.integers(36, 72)))
        widths.append(S - sum(widths))
        starts = np.concatenate([[0], np.cumsum(widths)])
        shift = int(rng.integers(0, S))
        rel = (u - shift) % S
        blk = np.searchsorted(starts, rel, side="right") - 1
        du = np.minimum(rel - starts[blk], starts[blk + 1] - rel)
        tone = rng.uniform(0.75, 1.15, len(widths))
        greyness = rng.uniform(0.2, 0.9, len(widths))
        drop = rng.integers(0, 6, len(widths))          # ragged lower edges
        for y in range(r * rh, (r + 1) * rh):
            fr = (y - r * rh) / rh                        # 0 = exposed lower edge
            base = (1 - greyness[blk])[:, None] * np.array((0.42, 0.31, 0.22)) + greyness[blk][:, None] * np.array((0.45, 0.43, 0.40))
            c = base * (tone[blk] * (0.75 + 0.4 * grain[y]))[:, None]
            c *= 1.0 - 0.45 * max(0.0, fr - 0.7) / 0.3      # shadow of the course above
            c *= np.where(du < 2, 0.3, 1.0)[:, None]        # gaps between shingles
            gone = (y - r * rh) < drop[blk]
            c[gone] *= 0.25
            img[y] = c
        img[r * rh: r * rh + 2] *= 0.6
    img = mix(img, img * col((0.6, 0.75, 0.42)), moss * 0.75)
    return np.clip(img, 0, 1)

def tex_bark(rng):
    v = np.arange(S)[:, None]
    ridges = fbm(rng, 40, 3, 4)
    cracks = np.abs(noise(rng, 22, 4) - 0.5) < 0.06
    img = mix(col((0.20, 0.17, 0.14)), col((0.43, 0.39, 0.34)), ridges)
    img = np.where(cracks[..., None], img * 0.45, img)
    img *= (0.85 + 0.25 * noise(rng, 6, 12))[..., None]
    return np.clip(img, 0, 1)

def tex_ground(rng):
    """Trodden dirt with patches of dry grass and pebbles (a 4 m tile)."""
    d = fbm(rng, 6, 6, 5)
    grass = np.clip((fbm(rng, 4, 4, 5) - 0.42) * 3.5, 0, 1)
    dirt = mix(col((0.30, 0.24, 0.17)), col((0.42, 0.35, 0.26)), d)
    blades = noise(rng, 160, 160)
    straw = mix(col((0.38, 0.36, 0.20)), col((0.52, 0.47, 0.28)), blades)
    straw *= (0.75 + 0.4 * noise(rng, 96, 96))[..., None]
    img = mix(dirt, straw, grass)
    yy, xx = np.ogrid[0:S, 0:S]
    for _ in range(140):
        cx, cy, rr = rng.integers(0, S), rng.integers(0, S), rng.uniform(1.5, 4.0)
        dx = (xx - cx + S // 2) % S - S // 2; dy = (yy - cy + S // 2) % S - S // 2
        m = (dx * dx + dy * dy) < rr * rr
        img[m] = img[m] * 0.4 + np.array((0.52, 0.50, 0.46)) * rng.uniform(0.6, 1.0)
    return np.clip(img, 0, 1)

def tex_needles(rng):
    """Pine needles: dark streaky greens with lighter tips and shadowed clumps."""
    streak = fbm(rng, 96, 12, 3)
    clump = fbm(rng, 6, 6, 4)
    img = mix(col((0.08, 0.15, 0.08)), col((0.19, 0.30, 0.13)), streak)
    img *= (0.65 + 0.55 * clump)[..., None]
    tips = noise(rng, 180, 180) > 0.86
    img = np.where(tips[..., None], img * 1.35 + col((0.02, 0.04, 0.0)), img)
    return np.clip(img, 0, 1)

def tex_leaves(rng):
    """Broadleaf canopy: overlapping leaves in mixed greens over dark gaps (tileable)."""
    img = np.zeros((S, S, 3), np.float32) + col((0.05, 0.09, 0.03))[0, 0]
    greens = [(0.18, 0.30, 0.08), (0.25, 0.38, 0.11), (0.32, 0.44, 0.13), (0.38, 0.47, 0.15), (0.45, 0.46, 0.15)]
    for _ in range(1400):
        cx, cy = rng.integers(0, S), rng.integers(0, S)
        r = rng.uniform(5, 11)
        a = rng.uniform(0, np.pi)
        ca, sa = np.cos(a), np.sin(a)
        n = int(r) + 2
        dy, dx = np.mgrid[-n:n + 1, -n:n + 1]
        u = (dx * ca + dy * sa) / r
        v = (-dx * sa + dy * ca) / (r * 0.55)
        m = (u * u + v * v) < 1.0
        rows, cols = (cy + np.arange(-n, n + 1)) % S, (cx + np.arange(-n, n + 1)) % S
        patch = img[np.ix_(rows, cols)]
        c = np.array(greens[rng.integers(0, len(greens))]) * rng.uniform(0.8, 1.15)
        shade = (1.0 - 0.35 * np.clip(u, 0, 1))[..., None]          # one side of each leaf a bit darker
        patch[m] = (c * shade)[m]
        img[np.ix_(rows, cols)] = patch
    return np.clip(img * (0.85 + 0.3 * fbm(rng, 5, 5, 3))[..., None], 0, 1)

# ---------------------------------------------------------------------------- tree textures (realistic trees)
def tex_bark_real(rng, size=512):
    """Furrowed bark, tileable: U = once round the trunk, V = 2 m up it. Returns (rgb, height 0..1)."""
    ridges = fbm(rng, 11, 2, 5)                                   # long vertical ridges
    split = np.abs(noise(rng, 14, 3) - 0.5) * 2.0                 # where the bark splits into plates
    plates = np.clip((ridges - 0.32) * 2.2, 0, 1) * np.clip(split * 1.6, 0, 1)
    fine = fbm(rng, 48, 24, 3)
    cross = (noise(rng, 6, 40) > 0.78).astype(np.float32) * 0.5    # horizontal cracks across the plates
    height = np.clip(plates * (0.75 + 0.25 * fine) - cross * plates * 0.6, 0, 1)
    dark, mid, light = col((0.14, 0.12, 0.10)), col((0.35, 0.30, 0.25)), col((0.55, 0.50, 0.44))
    rgb = mix(dark, mid, np.clip(height * 1.8, 0, 1))
    rgb = mix(rgb, light, np.clip((height - 0.55) * 2.2, 0, 1) * (0.6 + 0.4 * fine))
    lichen = np.clip((fbm(rng, 5, 7, 4) - 0.6) * 4.0, 0, 1) * np.clip(height * 2, 0, 1)
    rgb = mix(rgb, col((0.44, 0.48, 0.36)), lichen * 0.55)
    return np.clip(rgb, 0, 1), height

def normal_map(height, strength):
    """Tangent-space normals (OpenGL / glTF: green up) from a tileable height field."""
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * strength
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * strength
    n = np.stack([-dx, -dy, np.ones_like(height)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return n * 0.5 + 0.5

class Sprite:
    """An RGBA drawing (y up, like Blender images) for the leaf cards: segments and leaves, painted over."""
    def __init__(self, size=512):
        self.size = size
        self.rgb = np.zeros((size, size, 3), np.float32)
        self.a = np.zeros((size, size), np.float32)

    def _paint(self, x0, y0, cov, color):
        h, w = cov.shape
        rgb = self.rgb[y0:y0 + h, x0:x0 + w]; a = self.a[y0:y0 + h, x0:x0 + w]
        c = np.asarray(color, np.float32)
        rgb[...] = rgb * (1 - cov[..., None]) + (c if c.ndim == 1 else c) * cov[..., None]
        a[...] = np.maximum(a, cov)

    def segment(self, p0, p1, w0, w1, color):
        """A tapered stroke from p0 to p1 (pixels), anti-aliased."""
        p0, p1 = np.array(p0, float), np.array(p1, float)
        pad = max(w0, w1) + 2
        x0 = int(max(0, min(p0[0], p1[0]) - pad)); x1 = int(min(self.size, max(p0[0], p1[0]) + pad + 1))
        y0 = int(max(0, min(p0[1], p1[1]) - pad)); y1 = int(min(self.size, max(p0[1], p1[1]) + pad + 1))
        if x1 <= x0 or y1 <= y0: return
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        d = p1 - p0; L2 = max(float(d @ d), 1e-6)
        t = np.clip(((xx - p0[0]) * d[0] + (yy - p0[1]) * d[1]) / L2, 0, 1)
        dist = np.hypot(xx - (p0[0] + t * d[0]), yy - (p0[1] + t * d[1]))
        half = (w0 + (w1 - w0) * t) * 0.5
        self._paint(x0, y0, np.clip(half - dist + 0.5, 0, 1), color)

    def leaf(self, base, angle, length, width, color, rng):
        """A pointed leaf from base along angle: lit on one side, darker rim, a pale midrib."""
        ca, sa = math.cos(angle), math.sin(angle)
        cx, cy = base[0] + ca * length * 0.5, base[1] + sa * length * 0.5
        r = int(length * 0.5 + 3)
        x0, x1 = int(max(0, cx - r)), int(min(self.size, cx + r + 1))
        y0, y1 = int(max(0, cy - r)), int(min(self.size, cy + r + 1))
        if x1 <= x0 or y1 <= y0: return
        yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
        u = ((xx - cx) * ca + (yy - cy) * sa) / (length * 0.5)        # -1 base .. 1 tip
        v = (-(xx - cx) * sa + (yy - cy) * ca) / (width * 0.5)
        edge = np.power(np.clip(1 - u * u, 0, 1), 0.65)              # pointed at both ends
        inside = edge - np.abs(v)
        cov = np.clip(inside * width * 0.5 + 0.5, 0, 1)
        if not cov.any(): return
        c = np.asarray(color, np.float32)
        shade = 0.82 + 0.22 * np.clip(v, -1, 1) + 0.08 * u            # one half catches more light, tips lighter
        rim = np.clip(1 - inside * 4, 0, 1) * 0.25
        rib = np.clip(1 - np.abs(v) * 9, 0, 1) * 0.12
        tint = c[None, None, :] * (shade - rim + rib)[..., None]
        self._paint(x0, y0, cov, tint)

    def finish(self):
        """Fill the see-through pixels with the colour of the nearest leaves, so mipmaps don't darken the edges."""
        rgb, a = self.rgb * self.a[..., None], self.a.copy()
        filled_rgb, filled_a = rgb.copy(), a.copy()
        for _ in range(24):
            blur = lambda x: (x + np.roll(x, 1, 0) + np.roll(x, -1, 0) + np.roll(x, 1, 1) + np.roll(x, -1, 1)) / 5
            filled_rgb, filled_a = blur(filled_rgb), blur(filled_a)
        fill = filled_rgb / np.maximum(filled_a[..., None], 1e-4)
        out = np.where(self.a[..., None] > 0.02, self.rgb, fill)
        return np.clip(out, 0, 1), self.a

def sprite_broadleaf(rng):
    """A leafy twig, growing up from the bottom centre (the card's base)."""
    s = Sprite()
    bark = (0.22, 0.16, 0.11)
    greens = [(0.22, 0.36, 0.10), (0.28, 0.43, 0.12), (0.35, 0.49, 0.14), (0.42, 0.52, 0.16), (0.49, 0.51, 0.16)]
    stem = [(256 + rng.uniform(-8, 8), 8)]
    for i in range(1, 9):
        stem.append((stem[-1][0] + rng.uniform(-14, 14), 8 + i * 58))
    twigs = []
    for i in range(len(stem) - 1):
        s.segment(stem[i], stem[i + 1], 9 - i * 0.9, 9 - (i + 1) * 0.9, bark)
        if i >= 1:
            side = 1 if i % 2 else -1
            a = math.radians(90 - side * rng.uniform(35, 60))
            L = rng.uniform(110, 190) * (1.0 - i / 12)
            end = (stem[i][0] + math.cos(a) * L, stem[i][1] + math.sin(a) * L)
            s.segment(stem[i], end, 5, 2, bark)
            twigs.append((stem[i], end))
    twigs.append((stem[-3], stem[-1]))
    for p0, p1 in twigs:
        ang = math.atan2(p1[1] - p0[1], p1[0] - p0[0])
        for k in range(rng.integers(5, 8)):
            t = 0.25 + 0.75 * k / 6
            at = (p0[0] + (p1[0] - p0[0]) * t, p0[1] + (p1[1] - p0[1]) * t)
            for side in (-1, 1):
                la = ang + side * math.radians(rng.uniform(30, 70))
                s.leaf(at, la, rng.uniform(48, 74), rng.uniform(22, 32), greens[rng.integers(0, len(greens))], rng)
        s.leaf(p1, ang + rng.uniform(-0.3, 0.3), rng.uniform(55, 80), rng.uniform(26, 34), greens[rng.integers(0, len(greens))], rng)
    return s.finish()

def sprite_pine(rng):
    """A spray of needles on a twig with side shoots, the twig from the bottom centre up."""
    s = Sprite()
    bark = (0.24, 0.16, 0.10)
    needles = [(0.16, 0.27, 0.11), (0.20, 0.33, 0.13), (0.25, 0.38, 0.15), (0.30, 0.42, 0.17)]
    def shoot(p0, angle, length, w, depth):
        steps = int(length / 4)
        pts = [np.array(p0, float)]
        a = angle
        for i in range(steps):
            a += rng.uniform(-0.03, 0.03)
            pts.append(pts[-1] + 4 * np.array((math.cos(a), math.sin(a))))
        if depth == 0:                            # side shoots all along, alternating, longest at the bottom (a fan)
            k = 0
            for i in range(4, len(pts) - 8, 7):
                side = 1 if k % 2 else -1
                k += 1
                f = i / len(pts)
                shoot(pts[i], a + side * math.radians(rng.uniform(38, 55)), 250 * (1 - 0.75 * f) * rng.uniform(0.8, 1.0), w * 0.5, 1)
        for i in range(len(pts) - 1):
            f = i / max(1, len(pts) - 1)
            s.segment(pts[i], pts[i + 1], w * (1 - 0.7 * f), w * (1 - 0.7 * (f + 1 / len(pts))), bark)
        for i in range(1, len(pts)):              # needles: pairs leaning forward, shorter towards the tip
            f = i / len(pts)
            nl = (1 - 0.5 * f) * rng.uniform(40, 58) * (0.8 if depth else 1.0)
            for side in (-1, 1):
                na = a + side * math.radians(rng.uniform(38, 62))
                tip = pts[i] + nl * np.array((math.cos(na), math.sin(na)))
                c = needles[rng.integers(0, len(needles))]
                s.segment(pts[i], tip, 3.0, 1.4, (c[0] * 1.15, c[1] * 1.15, c[2] * 1.1) if rng.random() < 0.3 else c)
    shoot((256, 6), math.radians(90 + rng.uniform(-4, 4)), 480, 7, 0)
    return s.finish()

def make_image(name, rgb, fmt="JPEG", alpha=None, noncolor=False):
    """Saved as JPEG (or PNG when it has alpha): the glTF export keeps each image's format (export_image_format AUTO)."""
    os.makedirs(TEX_DIR, exist_ok=True)
    h, w = rgb.shape[:2]
    img = bpy.data.images.new(name, w, h, alpha=alpha is not None)
    if noncolor: img.colorspace_settings.name = "Non-Color"     # before the pixels: changing it resets them
    px = np.ones((h, w, 4), np.float32)
    px[..., :3] = np.clip(rgb, 0, 1)
    if alpha is not None: px[..., 3] = np.clip(alpha, 0, 1)
    img.pixels.foreach_set(px.ravel())
    img.filepath_raw = os.path.join(TEX_DIR, name + (".jpg" if fmt == "JPEG" else ".png"))
    img.file_format = fmt
    try: img.save(quality=90)
    except TypeError: img.save()
    return img

# ============================================================================ materials
def make_mat(name, img=None, color=(1, 1, 1), rough=0.9, metal=0.0, normal=None, alpha_clip=None):
    """normal: a tangent-space normal map image (non-colour). alpha_clip: cut the texture's alpha at this value
    (glTF alphaMode MASK, which the engine reads as Cutout; export() also writes it into the .gltf)."""
    m = _make_mat(name, img, color, rough, metal)
    nt = m.node_tree
    bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
    tex = next((n for n in nt.nodes if n.type == "TEX_IMAGE"), None)
    if normal is not None:
        nimg = nt.nodes.new("ShaderNodeTexImage"); nimg.image = normal
        nmap = nt.nodes.new("ShaderNodeNormalMap")
        nt.links.new(nimg.outputs["Color"], nmap.inputs["Color"])
        nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    if alpha_clip is not None and tex is not None:
        cmp = nt.nodes.new("ShaderNodeMath"); cmp.operation = "GREATER_THAN"
        cmp.inputs[1].default_value = alpha_clip
        nt.links.new(tex.outputs["Alpha"], cmp.inputs[0])
        nt.links.new(cmp.outputs[0], bsdf.inputs["Alpha"])
        CLIPPED[name] = alpha_clip
    return m

CLIPPED = {}   # material name -> alpha cutoff (export() writes alphaMode MASK for these)

def _make_mat(name, img=None, color=(1, 1, 1), rough=0.9, metal=0.0):
    m = bpy.data.materials.new(name)
    try: m.use_nodes = True
    except Exception: pass
    nt = m.node_tree
    bsdf = next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)
    if bsdf is None:
        bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
        out = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL"), None) or nt.nodes.new("ShaderNodeOutputMaterial")
        nt.links.new(bsdf.outputs[0], out.inputs[0])
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    lin = [c ** 2.2 for c in color]
    bsdf.inputs["Base Color"].default_value = (*lin, 1.0)
    if img is not None:
        t = nt.nodes.new("ShaderNodeTexImage")
        t.image = img
        nt.links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
        if color != (1, 1, 1):      # a tint: texture x colour (exported as baseColorFactor)
            mul = nt.nodes.new("ShaderNodeMix")
            mul.data_type = "RGBA"; mul.blend_type = "MULTIPLY"
            mul.inputs[0].default_value = 1.0
            nt.links.new(t.outputs["Color"], mul.inputs[6])
            mul.inputs[7].default_value = (*lin, 1.0)
            nt.links.new(mul.outputs[2], bsdf.inputs["Base Color"])
    return m

# ---- the atlas: one texture for every textured surface of the building pieces and props (one material, so a
# piece is one draw). Textures repeat, an atlas cell cannot, so each cell holds CELL_M x CELL_M metres of its
# texture already tiled, with a border of the continued texture (bilinear filtering and the first mips stay
# inside), and every UV island is moved to fit inside its cell (Builder.object).
CELL_PX, PAD_PX, CELL_M = 1024, 32, 4.0
ATLAS_COLS, ATLAS_ROWS = 4, 2
PX_PER_M = (CELL_PX - 2 * PAD_PX) / CELL_M          # 240 px per metre (the 512 px / 2 m tiles had 256)
CELLS = {"plaster": 0, "stone": 1, "wood": 2, "timber": 3, "shingles": 4, "bark": 5, "needles": 6, "leaves": 7}

def atlas_cell(tile):
    """A 2 m tile (S x S, repeating) resampled into a cell: CELL_M metres inside, the tiling continued in the border."""
    p = (np.arange(CELL_PX) - PAD_PX + 0.5) / PX_PER_M * (S / TILE) - 0.5      # source pixel per cell pixel
    i0 = np.floor(p).astype(int); f = (p - i0).astype(np.float32)
    a, b = i0 % S, (i0 + 1) % S
    rows = tile[a] * (1 - f)[:, None, None] + tile[b] * f[:, None, None]       # along V (rows)
    return rows[:, a] * (1 - f)[None, :, None] + rows[:, b] * f[None, :, None]   # along U

MATS = {}      # name -> (Blender material, atlas cell or None)
def build_materials():
    rng = np.random.default_rng(7)
    wood = tex_wood(rng)
    tiles = {"plaster": tex_plaster(rng), "stone": tex_stone(rng), "wood": wood,
             "timber": wood * col((0.62, 0.56, 0.52)), "shingles": tex_shingles(rng), "bark": tex_bark(rng)}
    ground = tex_ground(rng)
    tiles["needles"] = tex_needles(np.random.default_rng(31))     # own generators: the older textures stay as they were
    tiles["leaves"] = tex_leaves(np.random.default_rng(37))
    atlas = np.full((ATLAS_ROWS * CELL_PX, ATLAS_COLS * CELL_PX, 3), 0.5, np.float32)
    for name, cell in CELLS.items():
        cx, cy = cell % ATLAS_COLS, cell // ATLAS_COLS
        atlas[cy * CELL_PX:(cy + 1) * CELL_PX, cx * CELL_PX:(cx + 1) * CELL_PX] = atlas_cell(tiles[name])
    kit = make_mat("Village", make_image("village_atlas", atlas), rough=0.9)
    for name, cell in CELLS.items(): MATS[name] = (kit, cell)
    MATS["ground"] = (make_mat("Ground", make_image("village_ground", ground), rough=0.97), None)
    MATS["iron"] = (make_mat("Iron", None, (0.16, 0.14, 0.13), rough=0.55, metal=0.7), None)
    MATS["rope"] = (make_mat("Rope", None, (0.52, 0.44, 0.30), rough=0.95), None)
    MATS["water"] = (make_mat("Water", None, (0.05, 0.07, 0.07), rough=0.08), None)
    # trees: a repeating bark with a normal map, and the leaf cards' sprites (2 x 2: broadleaf twigs, pine sprays)
    bark_rgb, bark_h = tex_bark_real(np.random.default_rng(41))
    MATS["bark_real"] = (make_mat("Bark", make_image("village_bark", bark_rgb), rough=0.95,
                                  normal=make_image("village_bark_normal", normal_map(bark_h, 5.0), noncolor=True)), None)
    rgb = np.zeros((2 * SPRITE_PX, 2 * SPRITE_PX, 3), np.float32); alpha = np.zeros((2 * SPRITE_PX, 2 * SPRITE_PX), np.float32)
    for i, (fn, seed) in enumerate(((sprite_broadleaf, 51), (sprite_broadleaf, 52), (sprite_pine, 53), (sprite_pine, 54))):
        c, a = fn(np.random.default_rng(seed))
        cx, cy = i % 2, i // 2
        rgb[cy * SPRITE_PX:(cy + 1) * SPRITE_PX, cx * SPRITE_PX:(cx + 1) * SPRITE_PX] = c
        alpha[cy * SPRITE_PX:(cy + 1) * SPRITE_PX, cx * SPRITE_PX:(cx + 1) * SPRITE_PX] = a
    MATS["leaves"] = (make_mat("Leaves", make_image("village_foliage", rgb, fmt="PNG", alpha=alpha), rough=0.8,
                               alpha_clip=LEAF_CUTOFF), KEEP_UV)

SPRITE_PX = 512
LEAF_CUTOFF = 0.35
KEEP_UV = -1          # an island whose UVs are final (leaf cards: a sprite's rectangle)

# ============================================================================ geometry builder
def rot(rx=0.0, ry=0.0, rz=0.0):
    """Rotation from degrees, applied X then Y then Z (Blender axes: Z up)."""
    return (Matrix.Rotation(math.radians(rz), 4, "Z") @ Matrix.Rotation(math.radians(ry), 4, "Y")
            @ Matrix.Rotation(math.radians(rx), 4, "X"))

def T(x, y, z):
    return Matrix.Translation((x, y, z))

class Builder:
    def __init__(self, seed):
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.mats = []
        self.rng = random.Random(seed)
        self.stack = [Matrix.Identity(4)]
        # UV islands: (faces, atlas cell or None, (qu, qv)). UVs stay in metres of texture (TILE = one repeat)
        # until object() packs each island into its cell; q > 0 = only move by whole steps of q on that axis
        # (shingle courses, barrel staves stay lined up with the texture's rows)
        self.islands = []
        self.leaf_faces = []      # (face, crown centre): leaf cards get normals pointing out of the crown

    # ---- matrix stack (local frames: a shutter on its hinge, a roof on its slope)
    def push(self, m): self.stack.append(self.stack[-1] @ m)
    def pop(self): self.stack.pop()

    def mi(self, mat):
        if mat not in self.mats: self.mats.append(mat)
        return self.mats.index(mat)

    def j(self, deg):     # jitter
        return self.rng.uniform(-deg, deg)

    def _finish(self, verts, faces, mat, local, grain=None, smooth=False, uv_fn=None, quantum=(0.0, 0.0)):
        """Material, smoothing and box-projected UVs (metres, in the primitive's own frame) for `faces`, each its
        own island; then the vertices are moved into place. uv_fn(co, normal, axis) may return a UV instead."""
        material, cell = MATS[mat]
        idx = self.mi(material)
        for f in faces:
            f.material_index = idx
            f.smooth = smooth
            f.normal_update()
            n = f.normal
            a = max(range(3), key=lambda k: abs(n[k]))
            p, q = [k for k in range(3) if k != a]
            if grain is not None and grain in (p, q):
                p, q = grain, (q if grain == p else p)
            for loop in f.loops:
                co = loop.vert.co
                if uv_fn is not None:
                    r = uv_fn(co, n, a)
                    if r is not None:
                        loop[self.uv].uv = r
                        continue
                loop[self.uv].uv = (co[p], co[q])
            self.islands.append(([f], cell, quantum))
        bmesh.ops.transform(self.bm, matrix=self.stack[-1] @ local, verts=verts)

    def box(self, size, loc=(0, 0, 0), r=(0, 0, 0), mat="wood", grain="auto", uv_fn=None, pivot=None, quantum=(0.0, 0.0)):
        """A box of `size` centred at loc (or with its local `pivot` point at loc), rotated r degrees."""
        sx, sy, sz = (s * 0.5 for s in size)
        pts = [(-sx, -sy, -sz), (sx, -sy, -sz), (sx, sy, -sz), (-sx, sy, -sz),
               (-sx, -sy, sz), (sx, -sy, sz), (sx, sy, sz), (-sx, sy, sz)]
        if pivot is not None: pts = [(x - pivot[0], y - pivot[1], z - pivot[2]) for x, y, z in pts]
        v = [self.bm.verts.new(p) for p in pts]
        quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (2, 3, 7, 6), (1, 2, 6, 5), (3, 0, 4, 7)]
        faces = [self.bm.faces.new([v[i] for i in q]) for q in quads]
        if grain == "auto": grain = max(range(3), key=lambda k: size[k])
        self._finish(v, faces, mat, T(*loc) @ rot(*r), grain, uv_fn=uv_fn, quantum=quantum)

    def beam(self, p0, p1, w, d, mat="timber", side=(0, 1, 0), twist=0.0):
        """A beam from p0 to p1: w across (perpendicular to `side`), d along `side` (wall thickness)."""
        p0, p1 = Vector(p0), Vector(p1)
        x = (p1 - p0).normalized()
        s = Vector(side)
        y = (s - x * s.dot(x))
        if y.length < 1e-4: y = Vector((0, 0, 1)) - x * x.z
        y.normalize()
        z = x.cross(y)
        m = Matrix((x, y, z)).transposed().to_4x4()
        m.translation = (p0 + p1) * 0.5
        sz = ((p1 - p0).length, d, w)
        self.push(m @ rot(twist, 0, 0))
        self.box(sz, mat=mat, grain=0)
        self.pop()

    def prism(self, profile, thickness, loc=(0, 0, 0), r=(0, 0, 0), mat="plaster", y0=None):
        """A polygon in local X/Z (Blender), extruded `thickness` along Y, centred on y = 0."""
        area = sum(profile[i][0] * profile[(i + 1) % len(profile)][1] - profile[(i + 1) % len(profile)][0] * profile[i][1]
                   for i in range(len(profile)))
        if area < 0: profile = profile[::-1]
        h = thickness * 0.5
        front = [self.bm.verts.new((x, -h, z)) for x, z in profile]
        back = [self.bm.verts.new((x, h, z)) for x, z in profile]
        faces = [self.bm.faces.new(front), self.bm.faces.new(back[::-1])]
        n = len(profile)
        for i in range(n):
            k = (i + 1) % n
            faces.append(self.bm.faces.new((front[i], back[i], back[k], front[k])))
        self._finish(front + back, faces, mat, T(*loc) @ rot(*r))

    def lathe(self, profile, seg=16, loc=(0, 0, 0), r=(0, 0, 0), mat="wood", cap_bottom=True, cap_top=True,
              smooth=True, uv="box", centres=None, phase=0.0):
        """Rings of (radius, z) around local Z. A closed outline (well ring, hoop) is given counter-clockwise in
        (r, z) with both caps off. centres: an (x, y) offset per ring (a bent trunk). uv: "box", "staves"
        (U along the height, ten boards around: barrels) or "bark" (U around, V up)."""
        rings = []
        for i, (rr, z) in enumerate(profile):
            cx, cy = centres[i] if centres else (0.0, 0.0)
            rings.append([self.bm.verts.new((cx + rr * math.cos(phase + 2 * math.pi * j / seg),
                                             cy + rr * math.sin(phase + 2 * math.pi * j / seg), z)) for j in range(seg)])
        faces, side = [], []
        uvmap = {}
        for i in range(len(rings) - 1):
            for j in range(seg):
                k = (j + 1) % seg
                f = self.bm.faces.new((rings[i][j], rings[i][k], rings[i + 1][k], rings[i + 1][j]))
                faces.append(f); side.append(f)
                for loop, (ri, jj) in zip(f.loops, ((i, j), (i, j + 1), (i + 1, j + 1), (i + 1, j))):
                    z = profile[ri][1]
                    if uv == "staves": uvmap[loop] = (z, TILE * jj / seg)      # ten 0.2 m boards around
                    elif uv == "bark": uvmap[loop] = (TILE * jj / seg, z)
        caps = []
        if cap_bottom: caps.append(self.bm.faces.new(rings[0][::-1]))
        if cap_top: caps.append(self.bm.faces.new(rings[-1]))
        verts = [v for ring in rings for v in ring]
        self._finish(verts, caps, mat, T(*loc) @ rot(*r))
        # sides: smooth, own UVs (the vertices are already transformed, so map them by loop)
        material, cell = MATS[mat]
        idx = self.mi(material)
        for f in side:
            f.material_index = idx
            f.smooth = smooth
            for loop in f.loops:
                if loop in uvmap: loop[self.uv].uv = uvmap[loop]
        if uv == "box":
            for f in side:
                f.normal_update()
                n = f.normal
                a = max(range(3), key=lambda k: abs(n[k]))
                p, q = [k for k in range(3) if k != a]
                for loop in f.loops:
                    loop[self.uv].uv = (loop.vert.co[p], loop.vert.co[q])
                self.islands.append(([f], cell, (0.0, 0.0)))
        elif side:     # the whole side is one island, so the staves / bark stay continuous around it
            self.islands.append((side, cell, (0.0, 0.2 if uv == "staves" else 0.0)))

    def chunk(self, cx, cy, size, mat="stone", tilt=35):
        """A rough lump (a box with a random turn) resting on the ground."""
        sx, sy, sz = size * self.rng.uniform(0.7, 1.3), size * self.rng.uniform(0.7, 1.3), size * self.rng.uniform(0.45, 0.9)
        self.box((sx, sy, sz), (cx, cy, sz * 0.35), (self.j(tilt), self.j(tilt), self.rng.uniform(0, 180)), mat=mat, grain=None)

    def pack_uvs(self, name):
        """Moves every island into its atlas cell (somewhere random that fits); islands without a cell (the
        ground's own repeating texture) just go from metres to repeats."""
        aw, ah = ATLAS_COLS * CELL_PX, ATLAS_ROWS * CELL_PX
        for faces, cell, quantum in self.islands:
            if cell == KEEP_UV: continue
            loops = [l for f in faces for l in f.loops]
            if cell is None:
                for l in loops: l[self.uv].uv = l[self.uv].uv / TILE
                continue
            place = []
            for axis in (0, 1):
                vals = [l[self.uv].uv[axis] for l in loops]
                mn, ext = min(vals), max(vals) - min(vals)
                q = quantum[axis]
                if ext > CELL_M - q + 1e-6:          # too big for a cell: squeeze it in
                    print(f"  {name}: a {ext:.2f} m island squeezed into a {CELL_M} m cell")
                    place.append((-mn, (CELL_M - 1e-3) / ext))
                    continue
                if q > 0:
                    shift = q * math.ceil(-mn / q - 1e-9)             # now mn + shift is in [0, q)
                    room = CELL_M - (mn + shift + ext)
                    shift += q * math.floor(self.rng.random() * (room / q + 1e-6))
                else:
                    shift = -mn + self.rng.random() * (CELL_M - ext)
                place.append((shift, 1.0))
            cx, cy = cell % ATLAS_COLS, cell // ATLAS_COLS
            for l in loops:
                u = (l[self.uv].uv[0] + place[0][0]) * place[0][1]
                v = (l[self.uv].uv[1] + place[1][0]) * place[1][1]
                l[self.uv].uv = ((cx * CELL_PX + PAD_PX + u * PX_PER_M) / aw, (cy * CELL_PX + PAD_PX + v * PX_PER_M) / ah)

    def object(self, name):
        self.pack_uvs(name)
        bm = self.bm
        bm.faces.index_update()
        centres = {f.index: c for f, c in self.leaf_faces}
        me = bpy.data.meshes.new(name)
        bm.to_mesh(me)
        bm.free()
        for m in self.mats: me.materials.append(m)
        if centres:
            # Leaf cards: normals pointing out of the crown (a bit up), so the canopy shades as one rounded mass
            # instead of flat cards facing every way. Everything else keeps the normals it has.
            normals = [tuple(c.vector) for c in me.corner_normals]
            for poly in me.polygons:
                c = centres.get(poly.index)
                if c is None: continue
                for li in poly.loop_indices:
                    d = Vector(me.vertices[me.loops[li].vertex_index].co) - c
                    n = d.normalized() if d.length > 1e-4 else Vector((0, 0, 1))
                    normals[li] = tuple((n * 0.8 + Vector((0, 0, 0.35))).normalized())
            me.normals_split_custom_set(normals)
        ob = bpy.data.objects.new(name, me)
        bpy.context.scene.collection.objects.link(ob)
        return ob

# ============================================================================ pieces
W, H = 2.0, 3.0
PLASTER_T = 0.16
BASE_H, SILL_TOP, PLATE_BOT = 0.5, 0.64, 2.82

def wall_frame(b, x0=-1.0, x1=1.0, base=True, sill=True, plate=True):
    if base: b.box((x1 - x0, 0.28, BASE_H), ((x0 + x1) / 2, 0, BASE_H / 2), mat="stone", grain=None)
    if sill: b.box((x1 - x0, 0.2, SILL_TOP - BASE_H), ((x0 + x1) / 2, 0, (BASE_H + SILL_TOP) / 2), (0, b.j(0.4), 0), mat="timber")
    if plate: b.box((x1 - x0, 0.22, H - PLATE_BOT), ((x0 + x1) / 2, 0, (PLATE_BOT + H) / 2), (0, b.j(0.3), 0), mat="timber")

def plaster(b, x0, x1, z0, z1):
    b.box((x1 - x0, PLASTER_T, z1 - z0), ((x0 + x1) / 2, 0, (z0 + z1) / 2), mat="plaster", grain=None)

def stud(b, x, z0=SILL_TOP, z1=PLATE_BOT, w=0.16):
    b.beam((x + b.j(0.015), 0, z0), (x + b.j(0.015), 0, z1), w, 0.19)

def wall(b):
    wall_frame(b)
    plaster(b, -1, 1, SILL_TOP, PLATE_BOT)
    stud(b, 0.0)
    b.beam((-0.9, 0, SILL_TOP), (-0.08, 0, PLATE_BOT), 0.14, 0.185)
    b.beam((0.9, 0, SILL_TOP), (0.08, 0, PLATE_BOT), 0.14, 0.185)
    b.beam((-1, 0, 1.75), (1, 0, 1.75), 0.12, 0.18)

def shutter(b, hinge_x, open_deg, hang_deg, left):
    """Three boards and two battens, hinged on the outside face (-Y in Blender = +Z in the engine)."""
    sgn = 1 if left else -1          # closed, a shutter covers the window: it reaches from its hinge towards the middle
    b.push(T(hinge_x, -0.13, 2.1) @ rot(0, 0, -sgn * open_deg) @ rot(0, sgn * hang_deg, 0))
    for i in range(3):
        cx = sgn * (0.075 + 0.15 * i)
        length = 0.88 - (0.12 if (i == 2 and not left) else 0.0)     # a broken board
        b.box((0.145, 0.035, length), (cx, 0, -length / 2), (0, b.j(1.0), 0), mat="wood")
    for z in (-0.18, -0.7):
        b.box((0.44, 0.03, 0.09), (sgn * 0.225, 0.03, z), mat="wood")
    b.pop()

def wall_window(b):
    wall_frame(b)
    plaster(b, -1, -0.45, SILL_TOP, PLATE_BOT)
    plaster(b, 0.45, 1, SILL_TOP, PLATE_BOT)
    plaster(b, -0.45, 0.45, SILL_TOP, 1.2)
    plaster(b, -0.45, 0.45, 2.1, PLATE_BOT)
    for x in (-0.5, 0.5):
        b.box((0.1, 0.2, 1.12), (x, 0, 1.66), mat="timber")
    b.box((1.1, 0.2, 0.12), (0, 0, 2.16), mat="timber")
    b.box((1.2, 0.28, 0.1), (0, -0.02, 1.15), (0, b.j(1.5), 0), mat="timber")
    b.box((0.05, 0.06, 0.9), (0.0, 0, 1.65), mat="timber")            # mullion
    b.box((0.42, 0.05, 0.05), (-0.22, 0, 1.68), (0, 8, 0), mat="timber")   # half a broken cross bar
    stud(b, -0.82); stud(b, 0.82)
    shutter(b, -0.55, 172, 0, True)        # folded back against the wall
    shutter(b, 0.55, 14, 22, False)        # hanging from one hinge

def wall_door(b):
    wall_frame(b, -1, -0.6, plate=False); wall_frame(b, 0.6, 1, plate=False)
    wall_frame(b, base=False, sill=False)
    b.box((1.2, 0.34, 0.1), (0, 0, 0.05), mat="stone", grain=None)          # threshold
    plaster(b, -1, -0.6, SILL_TOP, PLATE_BOT)
    plaster(b, 0.6, 1, SILL_TOP, PLATE_BOT)
    plaster(b, -0.6, 0.6, 2.3, PLATE_BOT)
    for x in (-0.65, 0.65):
        b.box((0.1, 0.2, 2.2), (x, 0, 1.2), mat="timber")
    b.box((1.5, 0.22, 0.16), (0, 0, 2.3), (0, b.j(1.0), 0), mat="timber")
    stud(b, -0.85); stud(b, 0.85)
    for x in (-0.66, 0.66):                                                  # hinge pins
        b.box((0.04, 0.24, 0.05), (x, 0, 0.5), mat="iron", grain=None)
        b.box((0.04, 0.24, 0.05), (x, 0, 1.9), mat="iron", grain=None)

def wall_broken(b):
    b.prism([(-1, 0), (1, 0), (1, 0.32), (0.82, 0.36), (0.7, 0.5), (-1, 0.5)], 0.28, mat="stone")
    wall_frame(b, -1, 0.75, base=False, plate=False)
    b.prism([(-1, SILL_TOP), (0.75, SILL_TOP), (0.78, 1.0), (0.62, 1.22), (0.5, 1.15), (0.38, 1.62), (0.18, 1.5),
             (0.05, 2.02), (-0.18, 1.96), (-0.32, 2.45), (-0.5, PLATE_BOT), (-1, PLATE_BOT)], PLASTER_T, mat="plaster")
    b.box((0.62, 0.22, 0.18), (-0.69, 0, 2.91), (0, -3, 0), mat="timber")     # what is left of the top plate
    stud(b, -0.62)
    b.beam((0.3, 0, SILL_TOP), (0.33, 0, 1.45), 0.16, 0.19)                  # snapped stud
    b.beam((-0.95, 0, SILL_TOP), (-0.66, 0, 1.4), 0.14, 0.185)
    b.beam((0.15, -0.75, 0.07), (0.92, -0.18, 1.2), 0.18, 0.18, side=(1, 0, 0), twist=b.j(10))   # fallen plate, leaning
    for i in range(16):
        side = -1 if i % 2 else 1
        b.chunk(b.rng.uniform(-0.3, 0.95), side * b.rng.uniform(0.22, 0.7), b.rng.uniform(0.08, 0.2),
                mat="stone" if i % 3 else "plaster")

def wall_ruin(b):
    """A low, broken stone wall (about 0.6 - 1.1 m: half cover)."""
    top = [(1.0, 0.62), (0.7, 0.75), (0.45, 0.68), (0.2, 0.95), (-0.15, 1.05), (-0.45, 0.9), (-0.7, 1.12), (-1.0, 1.08)]
    b.prism([(-1, 0), (1, 0)] + top, 0.32, mat="stone")
    for i in range(7):
        x = b.rng.uniform(-0.9, 0.9)
        b.box((b.rng.uniform(0.18, 0.32), 0.3, b.rng.uniform(0.1, 0.16)), (x, b.j(0.03), 0.62 + b.rng.uniform(0, 0.08)),
              (b.j(4), b.j(8), b.j(6)), mat="stone", grain=None)
    for i in range(10):
        side = -1 if i % 2 else 1
        b.chunk(b.rng.uniform(-1.0, 1.0), side * b.rng.uniform(0.25, 0.8), b.rng.uniform(0.1, 0.22))

def corner_post(b):
    b.box((0.34, 0.34, BASE_H), (0, 0, BASE_H / 2), mat="stone", grain=None)
    b.box((0.26, 0.26, H - BASE_H), (0, 0, (BASE_H + H) / 2), mat="timber")

def floor(b, broken=False):
    missing = {2: (-1, 0.15), 3: (-0.4, 0.9), 4: (-0.7, 0.6), 7: (0.2, 1)} if broken else {}
    for i in range(10):
        y = -0.9 + 0.2 * i
        cut = b.rng.uniform(-0.6, 0.6)
        for x0, x1 in ((-1, cut), (cut, 1)):
            if i in missing:
                m0, m1 = missing[i]
                if x0 >= m0 and x1 <= m1: continue
                if x0 < m0 < x1: x1 = m0
                elif x0 < m1 < x1: x0 = m1
                if x1 - x0 < 0.1: continue
            b.box((x1 - x0 - 0.006, 0.194, 0.06), ((x0 + x1) / 2, y, 0.03 + b.rng.uniform(0, 0.006)), (b.j(0.6), b.j(0.6), b.j(0.3)), mat="wood")
    if broken:
        b.box((1.1, 0.19, 0.05), (0.1, 0.0, 0.1), (b.j(4), 6, 25), mat="wood")
        b.box((0.7, 0.19, 0.05), (-0.5, -0.3, 0.035), (0, 2, -40), mat="wood")
        b.chunk(0.5, 0.6, 0.12); b.chunk(-0.6, -0.5, 0.1, mat="plaster")

SLOPE = math.sqrt(8.0)          # 2 m run, 2 m rise
def roof(b, broken=False):
    """Sheathing, rafters and 13 overlapping shingle courses, in the slope frame:
    x across, s up the slope (0 = wall line), t out of the roof."""
    c = math.sqrt(0.5)
    frame = Matrix(((1, 0, 0, 0), (0, c, -c, -1.0), (0, c, c, 0.0), (0, 0, 0, 1)))     # (x, s, t) -> Blender
    b.push(frame)
    s0, s1 = -0.45, SLOPE
    hole = (-0.35, 0.55, 0.85, 1.95) if broken else None        # x0, x1, s0, s1
    def slab(x0, x1, a, z):
        b.box((x1 - x0, z - a, 0.05), ((x0 + x1) / 2, (a + z) / 2, 0.025), mat="wood")
    if hole:
        hx0, hx1, hs0, hs1 = hole
        slab(-1, hx0, s0, s1); slab(hx1, 1, s0, s1); slab(hx0, hx1, s0, hs0); slab(hx0, hx1, hs1, s1)
    else:
        slab(-1, 1, s0, s1)
    for x in (-0.5, 0.5):
        if broken and x > 0:
            b.box((0.09, 1.25 - s0, 0.13), (x, (s0 + 1.25) / 2, -0.065), mat="timber")
            b.push(T(x, 1.25, -0.065) @ rot(-38, 0, 6))
            b.box((0.09, 0.9, 0.13), (0, 0.45, 0), mat="timber")
            b.pop()
        else:
            b.box((0.09, s1 - s0 - 0.12, 0.13), (x, (s0 + s1) / 2 - 0.06, -0.065), mat="timber")
    b.box((2.0, 0.04, 0.16), (0, s0 - 0.02, -0.03), mat="timber")       # fascia at the eave
    k, s = 0, s0
    tilt = math.degrees(math.atan2(0.03, 0.4))
    while s < s1 - 0.05:
        length = min(0.4, s1 + 0.05 - s)
        def uv_top(co, n, a, s=s, k=k):      # the exposed 0.25 m of the course is one row of the texture
            return (co[0], co[1] + length / 2 + k * 0.25) if a == 2 else None
        pieces = [(-1.0, 1.0)]
        if hole and s + 0.25 > hole[2] - 0.15 and s < hole[3]:
            l = hole[0] + b.rng.uniform(-0.15, 0.1); r = hole[1] + b.rng.uniform(-0.1, 0.15)
            pieces = [(-1.0, l), (r, 1.0)]
        for x0, x1 in pieces:
            b.push(T((x0 + x1) / 2, s + length, 0.05) @ rot(-tilt, 0, 0))     # lower edge raised onto the course below
            b.box((x1 - x0, length, 0.03), (0, -length / 2, 0.015), mat="shingles", grain=None, uv_fn=uv_top, quantum=(0.0, 0.25))
            b.pop()
        s += 0.25; k += 1
    if broken:   # loose shingles fallen into the hole
        for i in range(4):
            b.box((0.3, 0.28, 0.025), (b.rng.uniform(-0.2, 0.4), b.rng.uniform(0.9, 1.6), -0.15 - 0.05 * i),
                  (b.j(30), b.j(30), b.j(40)), mat="shingles", grain=None)
    b.pop()

def roof_ridge(b):
    b.box((2.0, 0.21, 0.21), (0, 0, 2.2), (45, 0, 0), mat="timber")      # half sunk into the two roof tops

def gable(b):
    """The triangle above a 4 m end wall (sits on the side walls' top plates, under the roof)."""
    b.prism([(-1.96, 0.16), (1.96, 0.16), (0, 1.94)], PLASTER_T, mat="plaster")
    b.box((4.0, 0.22, 0.16), (0, 0, 0.08), mat="timber")
    b.beam((0, 0, 0.16), (0, 0, 1.8), 0.14, 0.19)
    off = 0.11 * math.sqrt(2)
    b.beam((-1.85, 0, 2 - 1.85 - off), (0, 0, 2 - off), 0.14, 0.19)
    b.beam((1.85, 0, 2 - 1.85 - off), (0, 0, 2 - off), 0.14, 0.19)
    b.beam((-1.0, 0, 0.16), (-0.08, 0, 1.0), 0.12, 0.185)
    b.beam((1.0, 0, 0.16), (0.08, 0, 1.0), 0.12, 0.185)

def door(b):
    """1.15 x 2.15 plank door, hinge edge on the pivot (x = 0 .. 1.15)."""
    for i in range(5):
        hgt = 2.15 - (0.25 if i == 4 else 0.0) - b.rng.uniform(0, 0.03)
        b.box((0.226, 0.05, hgt), (0.115 + 0.23 * i, 0, hgt / 2), (0, b.j(0.4), 0), mat="wood")
    for z in (0.35, 1.8):
        b.box((1.05, 0.04, 0.13), (0.575, 0.045, z), mat="wood")
        b.box((0.32, 0.06, 0.05), (0.14, -0.035, z), mat="iron", grain=None)
    b.beam((0.12, 0.045, 0.42), (1.02, 0.045, 1.72), 0.12, 0.04, side=(0, 1, 0))

def fence_post(b):
    b.box((0.13, 0.13, 1.15), (0, 0, 0.575), (b.j(1.5), b.j(1.5), b.j(10)), mat="timber")
    b.box((0.09, 0.09, 0.06), (0, 0, 1.17), (0, 0, 45), mat="timber", grain=None)

def fence(b, broken=False):
    if broken:
        b.box((0.13, 0.13, 1.15), (-1, 0, 0), (-6, 11, 4), mat="timber", pivot=(0, 0, -0.575))     # leaning post
        b.beam((-1, -0.1, 0.42), (1, -0.1, 0.5), 0.11, 0.07, side=(0, 1, 0), twist=b.j(5))
        b.beam((-0.95, -0.1, 0.8), (-0.05, -0.12, 0.62), 0.11, 0.07, side=(0, 1, 0), twist=10)
        b.beam((0.05, -0.35, 0.05), (0.95, -0.2, 0.62), 0.11, 0.07, side=(0, 0, 1), twist=20)
        b.chunk(0.3, 0.25, 0.08, mat="wood")
    else:
        fence_post(b)
        for z in (0.45, 0.85):
            b.beam((-1, -0.1, z + b.j(0.03)), (1, -0.1, z + b.j(0.03)), 0.11, 0.07, side=(0, 1, 0), twist=b.j(6))
        b.push(T(-1, 0, 0)); fence_post(b); b.pop()

def barrel(b):
    prof = [(0.27, 0.0), (0.305, 0.18), (0.325, 0.45), (0.305, 0.72), (0.27, 0.9)]
    b.lathe(prof, 20, mat="wood", uv="staves", cap_top=False)
    b.lathe([(0.27, 0.9), (0.25, 0.9), (0.25, 0.85)], 20, mat="wood", cap_bottom=False, smooth=False)
    def r_at(z):
        for (r0, z0), (r1, z1) in zip(prof, prof[1:]):
            if z0 <= z <= z1: return r0 + (r1 - r0) * (z - z0) / (z1 - z0)
        return prof[-1][0]
    for z in (0.08, 0.3, 0.6, 0.82):
        r = r_at(z) + 0.008
        b.lathe([(r - 0.012, z - 0.025), (r, z - 0.025), (r, z + 0.025), (r - 0.012, z + 0.025)], 20, mat="iron",
                cap_bottom=False, cap_top=False, smooth=True)

def crate(b, broken=False):
    s = 0.8
    if not broken:
        b.box((s - 0.06, s - 0.06, s - 0.06), (0, 0, s / 2), mat="wood")
    else:
        b.box((s - 0.06, s - 0.06, 0.04), (0, 0, 0.05), mat="wood")
        for i in range(3):   # side boards, one side caved in
            z = 0.14 + 0.25 * i
            b.box((s - 0.06, 0.03, 0.24), (0, -0.37, z), mat="wood")
            b.box((0.03, s - 0.06, 0.24), (0.37, 0, z), mat="wood")
            b.box((0.03, s - 0.06, 0.24), (-0.37, 0, z), mat="wood")
            if i < 2: b.box((s - 0.06, 0.03, 0.24), (b.j(0.05), 0.37, z), (b.j(6), 0, b.j(4)), mat="wood")
        b.box((s - 0.06, 0.03, 0.24), (0.1, 0.6, 0.03), (90, 0, 25), mat="wood")
        b.box((0.82, 0.24, 0.03), (-0.2, -0.75, 0.02), (0, 0, -30), mat="wood")
        b.box((0.6, 0.24, 0.03), (0.0, 0.02, 0.83), (8, -12, 20), mat="wood")
    for x in (-1, 1):
        for y in (-1, 1):
            b.box((0.08, 0.08, s), (x * 0.36, y * 0.36, s / 2), mat="timber")
    for z in (0.04, s - 0.04):
        if broken and z > 0.5: continue
        for k in range(4):
            a = k * 90
            b.push(rot(0, 0, a))
            b.box((s, 0.04, 0.08), (0, -0.39, z), mat="timber")
            b.pop()
    for a in (0, 90) if not broken else (90,):
        b.push(rot(0, 0, a))
        b.beam((-0.33, -0.39, 0.08), (0.33, -0.39, s - 0.08), 0.08, 0.04)
        b.pop()

def rubble(b):
    for i in range(46):
        a = b.rng.uniform(0, 2 * math.pi); r = 0.95 * math.sqrt(b.rng.random())
        h = 0.55 * (1 - (r / 0.95) ** 2)
        sz = b.rng.uniform(0.12, 0.34)
        x, y = r * math.cos(a), r * math.sin(a)
        b.box((sz * b.rng.uniform(0.8, 1.4), sz, sz * b.rng.uniform(0.5, 0.9)), (x, y, h * b.rng.uniform(0.3, 1.0) + sz * 0.2),
              (b.j(40), b.j(40), b.rng.uniform(0, 180)), mat="stone" if i % 5 else "plaster", grain=None)
    b.beam((-0.9, 0.3, 0.05), (0.7, -0.2, 0.55), 0.17, 0.17, side=(0, 0, 1), twist=12)
    b.beam((0.8, 0.6, 0.04), (-0.1, -0.5, 0.42), 0.14, 0.14, side=(0, 0, 1), twist=-20)

def debris(b):
    z = 0.0
    for i in range(9):
        L = b.rng.uniform(0.9, 1.8)
        a = b.rng.uniform(-50, 50) + (90 if i % 3 == 0 else 0)
        z += 0.03 if i % 2 else 0.0
        b.box((L, 0.19, 0.05), (b.rng.uniform(-0.3, 0.3), b.rng.uniform(-0.3, 0.3), 0.025 + z), (b.j(4), b.j(4), a), mat="wood")
    b.beam((-0.7, -0.4, 0.04), (0.6, 0.5, 0.35), 0.16, 0.16, side=(0, 0, 1), twist=b.j(15))

def well(b):
    b.lathe([(0.55, 0.0), (0.8, 0.0), (0.8, 0.78), (0.55, 0.78)], 20, mat="stone", cap_bottom=False, cap_top=False, smooth=False)
    b.lathe([(0.5, 0.78), (0.86, 0.78), (0.86, 0.9), (0.5, 0.9)], 20, mat="stone", cap_bottom=False, cap_top=False, smooth=False)
    b.lathe([(0.56, 0.3)], 20, mat="water", cap_bottom=False, cap_top=True)
    for x in (-0.68, 0.68):
        b.box((0.14, 0.14, 1.75), (x, 0, 0.78 + 0.875), (0, b.j(1.5), 0), mat="timber")
    b.lathe([(0.06, -0.75), (0.06, 0.75)], 10, loc=(0, 0, 2.15), r=(0, 90, 0), mat="timber", uv="bark")
    b.box((0.08, 0.3, 0.04), (0.82, 0.12, 2.15), (0, 0, 0), mat="timber")          # crank
    b.lathe([(0.075, -0.18), (0.075, 0.18)], 10, loc=(0.0, 0, 2.15), r=(0, 90, 0), mat="rope")
    b.lathe([(0.012, 1.25), (0.012, 2.1)], 6, mat="rope", cap_bottom=False, cap_top=False)
    b.lathe([(0.13, 1.0), (0.16, 1.25)], 12, mat="wood", uv="staves", cap_top=False)
    b.lathe([(0.17, 1.12), (0.17, 1.16)], 12, mat="iron", cap_bottom=False, cap_top=False)
    for side in (-1, 1):   # a small shingled roof, one board missing
        b.push(T(0, 0, 2.62) @ rot(-side * 40, 0, 0))
        b.box((1.9, 0.62, 0.05), (0, side * 0.31, 0), mat="shingles", grain=None)
        b.pop()
    b.box((1.9, 0.08, 0.1), (0, 0, 2.66), mat="timber")
    b.beam((-0.68, 0, 2.5), (0.68, 0, 2.5), 0.1, 0.1)

def dead_tree(b):
    rng = b.rng
    n = 9
    prof = [(0.34, 0.0), (0.22, 0.25)] + [(0.2 - 0.15 * (i / n), 0.25 + 3.0 * i / n) for i in range(1, n + 1)]
    bend = [(0.05 * math.sin(z * 1.3), 0.12 * (z / 3.2) ** 2) for _, z in prof]
    b.lathe(prof, 9, mat="bark", uv="bark", centres=bend)
    def branch(base, direction, length, r0, depth):
        d = Vector(direction).normalized()
        tip = Vector(base) + d * length
        m = Matrix.Translation(base) @ d.to_track_quat("Z", "Y").to_matrix().to_4x4()
        b.push(m)
        b.lathe([(r0, 0.0), (r0 * 0.35, length)], 6, mat="bark", uv="bark", cap_bottom=False)
        b.pop()
        if depth > 0:
            for k in range(2):
                nd = d + Vector((rng.uniform(-0.8, 0.8), rng.uniform(-0.8, 0.8), rng.uniform(0.1, 0.6)))
                at = Vector(base) + d * length * rng.uniform(0.55, 0.9)
                branch(at, nd, length * rng.uniform(0.45, 0.65), r0 * 0.5, depth - 1)
    for z, ang, up in ((1.6, 30, 0.5), (2.2, 150, 0.7), (2.6, 260, 0.9), (2.9, 80, 1.3)):
        i = min(range(len(prof)), key=lambda k: abs(prof[k][1] - z))
        cx, cy = bend[i]
        d = (math.cos(math.radians(ang)), math.sin(math.radians(ang)), up)
        branch((cx, cy, z), d, rng.uniform(1.0, 1.5), prof[i][0] * 0.55, 1)
    for ang in (20, 140, 250):
        d = (math.cos(math.radians(ang)), math.sin(math.radians(ang)), -0.25)
        branch((0, 0, 0.18), d, 0.6, 0.12, 0)

def signpost(b):
    b.box((0.12, 0.12, 2.1), (0, 0, 1.05), (b.j(1), b.j(1), 0), mat="timber")
    arrow = [(0.0, -0.09), (0.62, -0.09), (0.74, 0.0), (0.62, 0.09), (0.0, 0.09)]
    b.push(T(0.04, 0, 1.75) @ rot(0, 0, 15))
    b.prism(arrow, 0.035, mat="wood")
    b.pop()
    b.push(T(-0.04, 0, 1.45) @ rot(0, 0, 200) @ rot(0, 25, 0))      # hanging from one nail
    b.prism(arrow, 0.035, mat="wood")
    b.pop()

# ---------------------------------------------------------------------------- trees (the forest around the village)
# Pivot at the foot of the trunk. Give them Wind 1 in the engine (MeshRenderer.Wind): the crown sways, the base stays.

# Trunk and big limbs are geometry (bent, tapering tubes with a repeating bark and its normal map); twigs, leaves
# and needles are alpha-cut cards with the sprites of village_foliage.png, in crossed pairs, seen from both sides.

def tube(b, pts, radii, seg=8, mat="bark_real"):
    """A bent tube through pts (radius per point), closed at its end. UV: once round, 2 m of bark per repeat."""
    pts = [Vector(p) for p in pts]
    n = len(pts)
    tangents = [(pts[min(i + 1, n - 1)] - pts[max(i - 1, 0)]).normalized() for i in range(n)]
    ref = Vector((0, 0, 1)) if abs(tangents[0].z) < 0.9 else Vector((1, 0, 0))
    nrm = (ref - tangents[0] * ref.dot(tangents[0])).normalized()
    rings, along = [], [0.0]
    for i, p in enumerate(pts):
        t = tangents[i]
        nrm = (nrm - t * nrm.dot(t)).normalized()          # carried along the tube (no sudden twists)
        bi = t.cross(nrm)
        rings.append([b.bm.verts.new(p + radii[i] * (math.cos(2 * math.pi * j / seg) * nrm + math.sin(2 * math.pi * j / seg) * bi))
                      for j in range(seg)])
        if i: along.append(along[-1] + (pts[i] - pts[i - 1]).length)
    material, cell = MATS[mat]
    idx = b.mi(material)
    faces = []
    for i in range(n - 1):
        for j in range(seg):
            f = b.bm.faces.new((rings[i][j], rings[i][(j + 1) % seg], rings[i + 1][(j + 1) % seg], rings[i + 1][j]))
            for loop, (ri, jj) in zip(f.loops, ((i, j), (i, j + 1), (i + 1, j + 1), (i + 1, j))):
                loop[b.uv].uv = (TILE * jj / seg, along[ri])
            faces.append(f)
    tip = b.bm.verts.new(pts[-1] + tangents[-1] * radii[-1])
    for j in range(seg):
        f = b.bm.faces.new((rings[-1][j], rings[-1][(j + 1) % seg], tip))
        for loop, (u, v) in zip(f.loops, ((j, along[-1]), (j + 1, along[-1]), (j + 0.5, along[-1] + radii[-1]))):
            loop[b.uv].uv = (TILE * u / seg, v)
        faces.append(f)
    for f in faces:
        f.material_index = idx
        f.smooth = True
    b.islands.append((faces, cell, (0.0, 0.0)))

def grow(rng, start, direction, length, r0, r1, segs, up=0.0, wobble=0.1):
    """A limb's centre line: it bends at random and towards the sky (up > 0) or the ground (up < 0)."""
    pts, d = [Vector(start)], Vector(direction).normalized()
    for i in range(segs):
        d = (d + Vector((rng.uniform(-wobble, wobble), rng.uniform(-wobble, wobble), up))).normalized()
        pts.append(pts[-1] + d * (length / segs))
    return pts, [r0 + (r1 - r0) * i / segs for i in range(segs + 1)]

def card(b, base, direction, side, width, height, sprite, centre):
    """A leaf card: a sprite of village_foliage.png standing on base, growing along direction, side across it.
    Two faces (front and back), so it shows from both sides with back-face culling."""
    d = Vector(direction).normalized()
    s = Vector(side).normalized() * (width * 0.5)
    base = Vector(base)
    corners = [base - s, base + s, base + s + d * height, base - s + d * height]
    cx, cy = sprite % 2, sprite // 2
    pad = 2.0 / (2 * SPRITE_PX)
    u0, u1 = cx * 0.5 + pad, (cx + 1) * 0.5 - pad
    v0, v1 = cy * 0.5 + pad, (cy + 1) * 0.5 - pad
    uvs = [(u0, v0), (u1, v0), (u1, v1), (u0, v1)]
    material, cell = MATS["leaves"]
    idx = b.mi(material)
    faces = []
    for order in ((0, 1, 2, 3), (3, 2, 1, 0)):
        vs = [b.bm.verts.new(corners[k]) for k in order]
        f = b.bm.faces.new(vs)
        for loop, k in zip(f.loops, order): loop[b.uv].uv = uvs[k]
        f.material_index = idx
        faces.append(f)
        b.leaf_faces.append((f, Vector(centre)))
    b.islands.append((faces, KEEP_UV, (0.0, 0.0)))

def card_pair(b, rng, base, direction, size, sprite, centre, aspect=1.0):
    """Two cards crossed along the same direction (a twig with some volume from every side)."""
    d = Vector(direction).normalized()
    s1 = (Matrix.Rotation(rng.uniform(0, math.pi), 3, d) @ d.orthogonal().normalized()).normalized()
    s2 = d.cross(s1).normalized()
    for s in (s1, s2):
        card(b, base, d, s, size * aspect, size, sprite, centre)

def oak(b, height=9.0, spread=3.4, limbs=6):
    rng = b.rng
    # trunk: a flared foot, slightly crooked, carrying on up as the leader
    tpts, trad = grow(rng, (0, 0, 0), (0, 0, 1), height * 0.78, 0.36, 0.05, 10, up=0.0, wobble=0.05)
    trad[0] = 0.55; trad[1] *= 1.18
    tube(b, tpts, trad, seg=10)
    centre = Vector((0, 0, height * 0.62))
    tips = []
    for i in range(limbs):
        at = int(rng.uniform(0.38, 0.66) * (len(tpts) - 1))
        az = i * 2.39996 + rng.uniform(-0.3, 0.3)                    # golden angle round the trunk
        el = rng.uniform(0.3, 0.85)
        d = Vector((math.cos(az) * math.cos(el), math.sin(az) * math.cos(el), math.sin(el)))
        pts, rad = grow(rng, tpts[at], d, spread * rng.uniform(0.75, 1.05), min(trad[at] * 0.62, 0.17), 0.03, 6, up=0.07, wobble=0.16)
        tube(b, pts, rad, seg=7)
        for j in range(int(rng.randrange(3, 5))):
            p = int(rng.uniform(0.3, 0.92) * (len(pts) - 1))
            dd = (d + Vector((rng.uniform(-0.9, 0.9), rng.uniform(-0.9, 0.9), rng.uniform(0.0, 0.8)))).normalized()
            pts2, rad2 = grow(rng, pts[p], dd, rng.uniform(1.1, 2.0), max(rad[p] * 0.55, 0.025), 0.012, 3, up=0.1, wobble=0.22)
            tube(b, pts2, rad2, seg=5)
            tips += [(q, dd) for q in pts2[1:]]
        tips += [(q, d) for q in pts[-3:]]
    tips += [(q, Vector((0, 0, 1))) for q in tpts[-3:]]
    for p, d in tips:                                                 # twigs with leaves
        for k in range(2):
            dir_ = (d + Vector((rng.uniform(-0.8, 0.8), rng.uniform(-0.8, 0.8), rng.uniform(-0.3, 0.9)))).normalized()
            size = rng.uniform(1.3, 1.9)
            card_pair(b, rng, p - dir_ * size * 0.2, dir_, size, int(rng.randrange(0, 2)), centre)

def pine(b, height=12.0, reach=3.0):
    rng = b.rng
    tpts, trad = grow(rng, (0, 0, 0), (0, 0, 1), height, 0.32, 0.025, 14, up=0.0, wobble=0.015)
    trad[0] = 0.48; trad[1] *= 1.15
    tube(b, tpts, trad, seg=9)
    def trunk_at(z):
        i = min(len(tpts) - 2, max(0, int(z / height * (len(tpts) - 1))))
        f = (z - tpts[i].z) / max(tpts[i + 1].z - tpts[i].z, 1e-4)
        return tpts[i].lerp(tpts[i + 1], min(max(f, 0.0), 1.0))
    z, base_z = 2.0, 2.0
    while z < height - 0.5:
        f = (z - base_z) / (height - base_z)                           # 0 at the lowest whorl, 1 at the top
        L = reach * (1 - f) ** 0.9 + 0.35
        n = int(rng.randrange(5, 7))
        a0 = rng.uniform(0, 2 * math.pi)
        for k in range(n):
            az = a0 + 2 * math.pi * k / n + rng.uniform(-0.25, 0.25)
            el = math.radians(-18 + 34 * f + rng.uniform(-6, 6))     # droop low down, rise near the top
            d = Vector((math.cos(az) * math.cos(el), math.sin(az) * math.cos(el), math.sin(el)))
            pts, rad = grow(rng, trunk_at(z), d, L, 0.06 * (1 - f) + 0.018, 0.008, 3, up=-0.04, wobble=0.05)
            tube(b, pts, rad, seg=5)
            m = max(1, int(math.ceil(L / 0.6)))
            for c in range(m):                                          # needle sprays along the branch, lying flat-ish
                t = c / m
                i = min(len(pts) - 2, int(t * (len(pts) - 1)))
                p = pts[i].lerp(pts[i + 1], t * (len(pts) - 1) - i)
                cd = (d + Vector((rng.uniform(-0.25, 0.25), rng.uniform(-0.25, 0.25), rng.uniform(-0.05, 0.15)))).normalized()
                size = min(1.8, L - t * L + 0.6)
                flat = cd.cross(Vector((0, 0, 1)))
                side = Matrix.Rotation(math.radians(rng.uniform(-25, 25)), 3, cd) @ (flat.normalized() if flat.length > 1e-3 else cd.orthogonal().normalized())
                centre = trunk_at(p.z) - Vector((0, 0, 0.6))
                card(b, p, cd, side, size * 1.0, size, 2 + int(rng.randrange(0, 2)), centre)
                card(b, p, cd, Matrix.Rotation(math.radians(55), 3, cd) @ side, size * 0.8, size * 0.9, 2 + int(rng.randrange(0, 2)), centre)
        z += rng.uniform(0.5, 0.7) * (1 - 0.35 * f)
    for k in range(4):                                                  # the top of the leader
        d = Vector((rng.uniform(-0.35, 0.35), rng.uniform(-0.35, 0.35), 1.0)).normalized()
        card_pair(b, rng, tpts[-2], d, 0.9, 2 + int(rng.randrange(0, 2)), tpts[-4], aspect=0.7)

def bush(b):
    rng = b.rng
    centre = Vector((0, 0, 0.35))
    for i in range(3):
        a = 2 * math.pi * i / 3 + rng.uniform(-0.4, 0.4)
        pts, rad = grow(rng, (0, 0, 0), (math.cos(a) * 0.5, math.sin(a) * 0.5, 1), 0.7, 0.035, 0.01, 3, up=0.05, wobble=0.2)
        tube(b, pts, rad, seg=5)
    for i in range(14):
        a = rng.uniform(0, 2 * math.pi)
        d = Vector((math.cos(a) * rng.uniform(0.4, 1.0), math.sin(a) * rng.uniform(0.4, 1.0), rng.uniform(0.3, 1.0))).normalized()
        card_pair(b, rng, Vector((0, 0, rng.uniform(0.05, 0.3))), d, rng.uniform(0.8, 1.15), int(rng.randrange(0, 2)), centre)

def ground(b):
    """A 10 x 10 m ground tile, top at 0 (5 m texture repeat, so tiles side by side continue the texture)."""
    def uv(co, n, a):       # UVs are in texture metres (TILE per repeat): one repeat per 5 m
        return (co[0] * TILE / 5.0, co[1] * TILE / 5.0) if a == 2 else None
    b.box((10, 10, 0.2), (0, 0, -0.1), mat="ground", grain=None, uv_fn=uv)

PIECES = [
    ("wall", wall), ("wall_window", wall_window), ("wall_door", wall_door), ("wall_broken", wall_broken),
    ("wall_ruin", wall_ruin), ("corner_post", corner_post), ("gable", gable),
    ("floor", floor), ("floor_broken", lambda b: floor(b, True)),
    ("roof", roof), ("roof_broken", lambda b: roof(b, True)), ("roof_ridge", roof_ridge),
    ("door", door), ("fence", fence), ("fence_broken", lambda b: fence(b, True)), ("fence_post", fence_post),
    ("barrel", barrel), ("crate", crate), ("crate_broken", lambda b: crate(b, True)),
    ("rubble", rubble), ("debris", debris), ("well", well), ("dead_tree", dead_tree), ("signpost", signpost),
    ("pine", lambda b: pine(b, 11.0, 2.6)), ("pine_tall", lambda b: pine(b, 15.0, 3.0)), ("oak", oak), ("oak_wide", lambda b: oak(b, 9.5, 4.2, 7)),
    ("bush", bush),
    ("ground", ground),
]

# ============================================================================ export
def export(ob, path):
    for o in bpy.context.scene.objects: o.select_set(False)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    # .gltf + .bin with the images beside them: every piece points at the same village_atlas.jpg (stored once,
    # and the engine shares one GPU texture per image file)
    kw = dict(filepath=path, export_format="GLTF_SEPARATE", use_selection=True, export_yup=True, export_apply=True,
              export_image_format="AUTO", export_materials="EXPORT", export_animations=False)   # JPEGs stay JPEG, the leaves PNG
    try:
        bpy.ops.export_scene.gltf(**kw, export_image_quality=88)
    except TypeError:
        bpy.ops.export_scene.gltf(**kw)
    # Alpha-cut materials: make sure the file says MASK with our cutoff (the engine reads it as Cutout).
    # And no material is double-sided: Blender writes doubleSided true whenever backface culling is off (its
    # default), but every kit piece is closed or has real back faces (leaf cards are two faces), and the engine
    # would draw all their back faces too.
    with open(path, encoding="utf-8") as f: g = json.load(f)
    changed = False
    for m in g.get("materials", []):
        if m.get("name") in CLIPPED:
            m["alphaMode"] = "MASK"; m["alphaCutoff"] = CLIPPED[m["name"]]; changed = True
        if m.pop("doubleSided", False): changed = True
    if changed:
        with open(path, "w", encoding="utf-8") as f: json.dump(g, f, indent=2)

# ============================================================================ previews (Cycles, CPU)
def render_previews(objs, folder):
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    sc.cycles.device = "CPU"
    sc.cycles.samples = 24
    try: sc.cycles.use_denoising = True
    except Exception: pass
    sc.render.resolution_x = sc.render.resolution_y = 320
    sc.render.film_transparent = False
    world = bpy.data.worlds.new("W"); sc.world = world
    try: world.use_nodes = True
    except Exception: pass
    bg = next(n for n in world.node_tree.nodes if n.type == "BACKGROUND")
    bg.inputs[0].default_value = (0.55, 0.6, 0.68, 1); bg.inputs[1].default_value = 0.9
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = 3.2; sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(-35))
    sc.collection.objects.link(sun)
    cam = bpy.data.objects.new("Cam", bpy.data.cameras.new("Cam"))
    cam.data.type = "ORTHO"
    sc.collection.objects.link(cam); sc.camera = cam
    files = []
    for ob in objs:
        for o in objs: o.hide_render = o is not ob
        bb = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
        mn = Vector((min(v.x for v in bb), min(v.y for v in bb), min(v.z for v in bb)))
        mx = Vector((max(v.x for v in bb), max(v.y for v in bb), max(v.z for v in bb)))
        centre = (mn + mx) / 2
        d = Vector((0.9, -1.3, 0.85)).normalized()
        cam.location = centre + d * 30
        cam.rotation_euler = (-d).to_track_quat("-Z", "Y").to_euler()
        cam.data.ortho_scale = (mx - mn).length * 1.05
        cam.data.clip_end = 100
        f = os.path.join(folder, ob.name + ".png")
        sc.render.filepath = f
        bpy.ops.render.render(write_still=True)
        files.append((ob.name, f))
    for o in objs: o.hide_render = False
    # contact sheet
    cols, cell = 5, 320
    rows = (len(files) + cols - 1) // cols
    sheet = np.ones((rows * cell, cols * cell, 4), np.float32)
    for i, (name, f) in enumerate(files):
        im = bpy.data.images.load(f)
        px = np.array(im.pixels[:], np.float32).reshape(cell, cell, 4)
        r, c = rows - 1 - i // cols, i % cols
        sheet[r * cell:(r + 1) * cell, c * cell:(c + 1) * cell] = px
    out = bpy.data.images.new("sheet", cols * cell, rows * cell)
    out.pixels.foreach_set(sheet.ravel())
    out.filepath_raw = os.path.join(folder, "_sheet.png"); out.file_format = "PNG"; out.save()
    print("sheet", out.filepath_raw, [n for n, _ in files])

# ============================================================================ demo scene (engine JSON)
CAM = [float(v) for v in os.environ.get("VILLAGE_CAM", "15,9,17,-0.38,0.72").split(",")]   # x, y, z, pitch, yaw (radians)
SUN_ROT = [float(v) for v in os.environ.get("VILLAGE_SUN", "-0.36,3.75,0").split(",")]       # pitch, yaw, roll (radians): low, behind the village

class SceneOut:
    def __init__(self):
        self.entities = []
        self.rng = random.Random(1234)

    def id(self): return self.rng.getrandbits(63)

    def piece(self, name, x, z, ry=0.0, y=0.0, label=None, rx=0.0, rz=0.0, body=None, scale=1.0, wind=0.0, trunk=None):
        """trunk = (width, height): a static box body around the trunk instead of a mesh body (trees)."""
        if body is None: body = not name.startswith(("roof", "gable", "bush"))      # nothing reaches the roofs
        e = {
            "id": self.id(), "name": label or name,
            "transform": {"position": [x, y, z], "rotation": [math.radians(rx), math.radians(ry), math.radians(rz)], "scale": [scale, scale, scale]},
            "meshRenderer": {"geometry": f"model:Models/Village/{name}.gltf", "texture": "", "color": [1, 1, 1],
                             "localOffset": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1], "unlit": False,
                             "useModelMaterials": True, "metallic": 0.0, "roughness": 0.9, "static": True, "castShadows": True},
        }
        if wind: e["meshRenderer"]["wind"] = wind
        if trunk:
            e["rigidBody"] = {"type": 0, "shape": 0, "size": [trunk[0], trunk[1], trunk[0]], "center": [0, trunk[1] / 2, 0], "friction": 0.6}
        elif body:
            e["rigidBody"] = {"type": 0, "shape": 3, "size": [1, 1, 1], "center": [0, 0, 0], "friction": 0.6}
        self.entities.append(e)

    def forest(self):
        """A ring of forest around the 30 x 30 m village: trees no closer than 3.6 m, a clearing around the
        houses and a gap where the road leaves past the signpost (x -1..5, south)."""
        rng = random.Random(77)
        trees, bushes = [], []
        def clear(x, z, margin):
            if abs(x) < 15 + margin and abs(z) < 15 + margin: return False      # the village clearing
            if -1.0 < x < 5.0 and z > 13: return False                       # the road out
            if (x - 15.0) ** 2 + (z - 17.0) ** 2 < 11.0 ** 2: return False      # room for the default game camera
            return True
        for _ in range(9000):
            x, z = rng.uniform(-44, 44), rng.uniform(-44, 44)
            if not clear(x, z, 2.5 + rng.uniform(0, 3)): continue
            if any((x - tx) ** 2 + (z - tz) ** 2 < 3.6 ** 2 for tx, tz in trees): continue
            trees.append((x, z))
        for _ in range(4000):
            x, z = rng.uniform(-44, 44), rng.uniform(-44, 44)
            if not clear(x, z, 0.5): continue
            if any((x - tx) ** 2 + (z - tz) ** 2 < 1.6 ** 2 for tx, tz in trees + bushes): continue
            bushes.append((x, z))
            if len(bushes) >= 140: break
        for x, z in trees:
            r = rng.random()
            kind = "pine" if r < 0.35 else "pine_tall" if r < 0.6 else "oak" if r < 0.85 else "oak_wide"
            s = rng.uniform(0.8, 1.25)
            self.piece(kind, round(x, 2), round(z, 2), rng.uniform(0, 360), scale=round(s, 3), wind=1.0,
                       trunk=(0.7 if kind.startswith("oak") else 0.5, 4.0), label="Tree")
        for x, z in bushes:
            self.piece("bush", round(x, 2), round(z, 2), rng.uniform(0, 360), scale=round(rng.uniform(0.7, 1.4), 3),
                       wind=0.6, body=False, label="Bush")
        print(f"forest: {len(trees)} trees, {len(bushes)} bushes")

    def house(self, ox, oz, ry, front, back, left, right, roof="roof", ridge=True, gables=(True, True), floors=None, roofs=None):
        """A house n modules wide and 4 m deep (front/back/left/right: wall pieces per module, None = gap).
        Local frame: front wall at +2 (z), ridge along x. (ox, oz) = centre, ry = degrees about Y.
        roofs: {("f" or "b", module): piece or None} overrides `roof` (the default for every slope, None = no roof)."""
        n = len(front)
        hw = n  # half width in metres
        c, s = math.cos(math.radians(ry)), math.sin(math.radians(ry))
        def put(name, lx, lz, lry, y=0.0, **kw):
            # engine rotation about Y: x' = x c + z s, z' = -x s + z c
            self.piece(name, ox + lx * c + lz * s, oz - lx * s + lz * c, (ry + lry) % 360, y, **kw)
        for i, p in enumerate(front):
            if p: put(p, -hw + 1 + 2 * i, 2, 0)
        for i, p in enumerate(back):      # seen from outside (behind): left to right = +x to -x
            if p: put(p, hw - 1 - 2 * i, -2, 180)
        for i, p in enumerate(left):      # left end (x = -hw), seen from outside: front to back
            if p: put(p, -hw, 1 - 2 * i, -90)
        for i, p in enumerate(right):
            if p: put(p, hw, -1 + 2 * i, 90)
        for i in range(n + 1):
            for lz in (-2, 2):
                put("corner_post", -hw + 2 * i, lz, 0)
        for lx in (-hw, hw):
            put("corner_post", lx, 0, 0)
        for i in range(n):
            lx = -hw + 1 + 2 * i
            for side, lz, r in (("f", 1, 0), ("b", -1, 180)):
                kind = (roofs or {}).get((side, i), roof)
                if kind: put(kind, lx, lz, r, 3.0)
            if roof and ridge: put("roof_ridge", lx, 0, 0, 3.0)
        if gables[0]: put("gable", -hw, 0, -90, 3.0)
        if gables[1]: put("gable", hw, 0, 90, 3.0)
        for i in range(n):
            for lz in (-1, 1):
                kind = (floors or {}).get((i, lz), "floor")
                if kind: put(kind, -hw + 1 + 2 * i, lz, 0)

    def build(self):
        for gx in range(-40, 41, 10):    # 90 x 90 m of ground: the village and the forest around it
            for gz in range(-40, 41, 10):
                self.piece("ground", gx, gz, label="Ground")
        self.forest()
        # sparse, darker grass on the forest floor (the village field adds to it in the clearing)
        self.entities.append({"id": self.id(), "name": "Forest floor",
                              "foliage": {"size": [90.0, 90.0], "density": 2.5, "seed": 3, "height": 0.32, "heightVariation": 0.5,
                                          "baseColor": [0.08, 0.13, 0.05], "tipColor": [0.30, 0.40, 0.16], "dryAmount": 0.45,
                                          "flowerAmount": 0.01, "windStrength": 0.6, "windDirection": 35.0,
                                          "drawDistance": 45.0, "avoidStatic": True},
                              "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]}})
        # House A: the best kept, 3 modules (6 x 4 m, x -9..-3, z -7..-3), facing the square
        self.house(-6, -5, 0, ["wall_window", "wall_door", "wall_window"], ["wall", "wall_window", "wall"],
                   ["wall", "wall_window"], ["wall_window", "wall"], roofs={("b", 2): "roof_broken"})
        self.piece("door", -6.55, -3.15, 95, label="Door")       # hung open, inwards
        # House B: half ruined, 2 modules (x 5..9, z -5..-1), its front (door) facing east; only half a roof left
        self.house(7, -3, 90, ["wall_door", "wall_broken"], ["wall_ruin", "wall"], ["wall_window", "wall_broken"], ["wall", "wall_ruin"],
                   roof=None, gables=(True, False), roofs={("b", 0): "roof", ("b", 1): "roof_broken"},
                   floors={(0, -1): "floor_broken", (1, 1): "floor_broken", (1, -1): None})
        self.piece("rubble", 10.0, -4.4, 30, label="Rubble")
        self.piece("rubble", 6.3, 0.1, 160, label="Rubble")
        self.piece("debris", 7.6, -2.0, 70, label="Debris")
        self.piece("door", 10.4, -1.2, 30, 0.06, label="Fallen door", rx=-90)
        # House C: small, 2 modules (x -11..-7, z 3..7), facing east onto the square
        self.house(-9, 5, 90, ["wall_door", "wall_window"], ["wall_broken", "wall"], ["wall", "wall"], ["wall_window", "wall"],
                   roofs={("f", 1): "roof_broken"})
        # the square
        self.piece("well", 1, 3, 20, label="Well")
        self.piece("dead_tree", 5, 7, 40, label="Dead tree")
        self.piece("signpost", 2, 12, 200, label="Signpost")
        for x, z, r, n in ((-3.2, -2.4, 10, "barrel"), (-2.5, -2.3, 70, "barrel"), (-2.8, -1.6, 0, "crate"),
                           (-8.6, -2.6, 20, "crate_broken"), (4.6, 2.2, 0, "barrel"), (-5.5, 8.6, 45, "crate"),
                           (-4.8, 9.2, 15, "barrel"), (-1.5, 6.0, 80, "debris")):
            self.piece(n, x, z, r, label=n.replace("_", " ").capitalize())
        self.piece("barrel", 3.6, 3.0, 0, 0.33, label="Tipped barrel", rx=90)
        # fenced yard south-east: posts on whole metres (a fence's own post is at its -X end), a gap for the gate
        for i, kind in enumerate(["fence", "fence", "fence_broken", None, "fence"]):
            if kind: self.piece(kind, 4 + 2 * i, 10, 0, label="Fence")
        self.piece("fence_post", 9, 10, 0, label="Fence post")      # the gate's other side
        for i, kind in enumerate(["fence", "fence_broken", "fence"]):   # turned 90: its post end is at +Z
            self.piece(kind, 13, 9 - 2 * i, 90, label="Fence")
        self.piece("fence_post", 13, 4, 0, label="Fence post")
        # low ruins in the north-east
        self.piece("wall_ruin", 9, -10, 0, label="Ruin"); self.piece("wall_ruin", 11, -10, 0, label="Ruin")
        self.piece("wall_ruin", 12, -9, 90, label="Ruin")
        self.piece("rubble", 10.5, -8.2, 0, label="Rubble")
        # grass over the whole village (it keeps off walls, floors and props by itself: avoidStatic)
        self.entities.append({"id": self.id(), "name": "Grass",
                              "foliage": {"size": [30.0, 30.0], "density": 15.0, "seed": 7, "height": 0.42, "heightVariation": 0.45,
                                          "dryAmount": 0.35, "flowerAmount": 0.03, "windStrength": 0.9, "windDirection": 35.0,
                                          "drawDistance": 50.0, "avoidStatic": True},
                              "transform": {"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]}})
        # sun and camera (the camera also makes Game.exe show the village)
        self.entities.append({"id": self.id(), "name": "Main Camera",
                              "camera": {"fov": 50.0, "near": 0.1, "far": 300.0, "primary": True},
                              "transform": {"position": [CAM[0], CAM[1], CAM[2]], "rotation": [CAM[3], CAM[4], 0.0], "scale": [1, 1, 1]}})
        self.entities.append({"id": self.id(), "name": "Sun",
                              "directionalLight": {"color": [1.0, 0.86, 0.68], "intensity": 1.8, "castShadows": True},
                              "transform": {"position": [0, 10, 0], "rotation": SUN_ROT, "scale": [1, 1, 1]}})
        return {"entities": self.entities, "version": 3, "settings": {
            "ao": {"bias": 0.025, "enabled": True, "radius": 0.5, "samples": 16, "strength": 1.0},
            "bloom": {"enabled": True, "intensity": 0.08, "knee": 0.5, "maxBrightness": 8.0, "radius": 1.0, "threshold": 1.0},
            "camera": {"focusDistance": 30.0, "pitch": -30.0, "position": [16.0, 15.0, 20.0], "yaw": -127.0},
            "environment": {"blur": 0.0, "intensity": 0.75, "map": "", "rotation": 0.0, "showSky": True},
            # a misty morning: low fog with drifting wisps, the low sun behind the village throwing shafts through it
            "volumetricFog": {"enabled": True, "density": 0.022, "heightFalloff": 1.5, "baseHeight": 0.0, "color": [0.80, 0.84, 0.92],
                              "anisotropy": 0.78, "sunIntensity": 1.5, "ambientIntensity": 0.3, "pointLightIntensity": 1.0,
                              "noiseScale": 0.12, "noiseAmount": 0.6, "windSpeed": 0.6, "windDirection": 35.0,
                              "maxDistance": 70.0, "steps": 32},
            "wind": {"direction": 35.0, "speed": 1.0, "strength": 1.0},       # sways the trees (grass has its own)
            "physics": {"gravity": 9.81},
            # the grade: deeper contrast and richer colour, cool shadows against warm lights, darker corners, a little grain
            "post": {"exposure": 1.45, "fxaa": True, "gamma": True, "toneMap": 2, "contrast": 1.14, "saturation": 1.22,
                     "temperature": 0.12, "tint": 0.0, "shadowTint": [0.88, 0.97, 1.08], "highlightTint": [1.08, 1.0, 0.88],
                     "vignette": 0.42, "grain": 0.022},
            "shadows": {"cascadeCount": 4, "cascadeSplit": 0.75, "enabled": True, "pointBias": 0.05, "pointFar": 30.0,
                        "shadowDistance": 60.0, "sunBias": 0.002, "sunCascades": True, "sunDistance": 30.0, "sunExtent": 16.0}}}

# ============================================================================ main
def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):       # the kit's own outputs only (kit.json is rewritten below)
        if os.path.splitext(f)[1].lower() in (".glb", ".gltf", ".bin", ".jpg", ".jpeg", ".png"):
            os.remove(os.path.join(OUT, f))
    build_materials()
    objs = []
    for i, (name, fn) in enumerate(PIECES):
        b = Builder(1000 + i)
        fn(b)
        ob = b.object(name)
        export(ob, os.path.join(OUT, name + ".gltf"))
        tris = sum(len(p.vertices) - 2 for p in ob.data.polygons)
        size = sum(os.path.getsize(os.path.join(OUT, name + e)) for e in (".gltf", ".bin"))
        print(f"{name:14s} {tris:6d} tris  {len(ob.data.materials)} material(s)  {size // 1024:5d} KB")
        objs.append(ob)
    for f in sorted(os.listdir(OUT)):
        if f.endswith((".jpg", ".png")): print(f"image {f}: {os.path.getsize(os.path.join(OUT, f)) // 1024} KB")
    with open(os.path.join(OUT, "kit.json"), "w") as f:
        json.dump({"kit": "Abandoned village", "module": 2.0, "storey": 3.0, "wallThickness": 0.2,
                   "note": "Models in this folder keep their own size and pivot when placed (Model::NativeSize). "
                           "Generated by Tools/village_kit.py."}, f, indent=2)
    with open(os.path.join(SCENES, "village.json"), "w") as f:
        json.dump(SceneOut().build(), f, indent=2)
    print("scene written")
    if PREVIEW:
        os.makedirs(PREVIEW, exist_ok=True)
        render_previews([o for o in objs if o.name != "ground"], PREVIEW)
    print("DONE")

try:
    main()
except Exception:
    traceback.print_exc()
    print("FAILED")
sys.stdout.flush()
