import math

import numpy as np
from PIL import Image, ImageDraw

BUCKET_EDGES = (2, 4, 8, 16, 32, 64, 128)
MAX_CANDIDATES = 2_000_000
FAR_KEY = np.iinfo(np.uint64).max
NEAR_PLANE = 0.05


def basis_from_forward(forward):
    forward = forward / np.linalg.norm(forward)
    world_up = np.array([0.0, 1.0, 0.0])
    right = np.cross(world_up, forward)
    if np.linalg.norm(right) < 1e-6:
        right = np.array([1.0, 0.0, 0.0])
    right /= np.linalg.norm(right)
    up = np.cross(forward, right)
    return np.stack([right, up, forward])


def orbit_basis(azimuth_degrees, elevation_degrees):
    azimuth = math.radians(azimuth_degrees)
    elevation = math.radians(elevation_degrees)
    forward = np.array([math.cos(elevation) * math.sin(azimuth), math.sin(elevation), math.cos(elevation) * math.cos(azimuth)])
    if abs(elevation_degrees) >= 89.9:
        right = np.array([math.cos(azimuth), 0.0, -math.sin(azimuth)])
        forward = np.array([0.0, math.copysign(1.0, elevation_degrees), 0.0])
        up = np.cross(forward, right)
        return np.stack([right, up, forward])
    return basis_from_forward(forward)


class Orbit:
    def __init__(self, azimuth, elevation, margin=0.06):
        self.rotation = orbit_basis(azimuth, elevation)
        self.margin = margin

    def project(self, positions, size):
        camera = positions @ self.rotation.T
        lo = camera[:, :2].min(axis=0)
        hi = camera[:, :2].max(axis=0)
        span = np.maximum(hi - lo, 1e-9)
        scale = min(size * (1 - 2 * self.margin) / span[0], size * (1 - 2 * self.margin) / span[1])
        center = (lo + hi) / 2
        screen = np.empty((len(positions), 2))
        screen[:, 0] = size / 2 + (camera[:, 0] - center[0]) * scale
        screen[:, 1] = size / 2 - (camera[:, 1] - center[1]) * scale
        return screen, -camera[:, 2], None


class Perspective:
    def __init__(self, eye, target, fov_degrees=75.0):
        self.eye = np.asarray(eye, dtype=np.float64)
        self.rotation = basis_from_forward(self.eye - np.asarray(target, dtype=np.float64))
        self.fov = math.radians(fov_degrees)

    def camera_space(self, positions):
        return (positions - self.eye) @ self.rotation.T

    def project(self, positions, size):
        camera = self.camera_space(positions)
        distance = np.maximum(-camera[:, 2], 1e-9)
        focal = (size / 2) / math.tan(self.fov / 2)
        screen = np.empty((len(positions), 2))
        screen[:, 0] = size / 2 + focal * camera[:, 0] / distance
        screen[:, 1] = size / 2 - focal * camera[:, 1] / distance
        return screen, -1.0 / distance, 1.0 / distance


def clip_near(camera, triangles):
    distance = -camera[:, 2]
    behind = distance[triangles] < NEAR_PLANE
    count_behind = behind.sum(axis=1)
    whole = np.nonzero(count_behind == 0)[0]
    kept = [triangles[whole]]
    source = [whole]
    extra_vertices = []
    extra_triangles = []
    extra_source = []
    next_index = len(camera)
    for triangle_index in np.nonzero((count_behind == 1) | (count_behind == 2))[0]:
        tri = triangles[triangle_index]
        flags = behind[triangle_index]
        indices = []
        for k in range(3):
            a, b = tri[k], tri[(k + 1) % 3]
            a_in, b_in = not flags[k], not flags[(k + 1) % 3]
            if a_in:
                indices.append(a)
            if a_in != b_in:
                t = (NEAR_PLANE - distance[a]) / (distance[b] - distance[a])
                extra_vertices.append((a, b, t))
                indices.append(next_index)
                next_index += 1
        for k in range(1, len(indices) - 1):
            extra_triangles.append((indices[0], indices[k], indices[k + 1]))
            extra_source.append(triangle_index)
    if extra_triangles:
        kept.append(np.array(extra_triangles, dtype=np.int64))
        source.append(np.array(extra_source, dtype=np.int64))
    return np.concatenate(kept), np.concatenate(source), extra_vertices


def extend_attribute(values, extra_vertices):
    if values is None or not extra_vertices:
        return values
    a = np.array([e[0] for e in extra_vertices])
    b = np.array([e[1] for e in extra_vertices])
    t = np.array([e[2] for e in extra_vertices])[:, None]
    return np.concatenate([values, values[a] * (1 - t) + values[b] * t])


def _edge(ax, ay, bx, by, px, py):
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax)


def rasterize(screen, depth, triangles, size, cull_back=False, subset=None, z_range=None):
    keys = np.full(size * size, FAR_KEY, dtype=np.uint64)
    if len(triangles) == 0 or (subset is not None and len(subset) == 0):
        return keys
    corners = screen[triangles]
    z = depth[triangles]
    area = _edge(corners[:, 0, 0], corners[:, 0, 1], corners[:, 1, 0], corners[:, 1, 1], corners[:, 2, 0], corners[:, 2, 1])
    keep = np.abs(area) > 1e-12
    if cull_back:
        keep &= area < 0
    if subset is not None:
        enabled = np.zeros(len(triangles), dtype=bool)
        enabled[subset] = True
        keep &= enabled
    z_lo, z_hi = z_range if z_range is not None else (depth[triangles].min(), depth[triangles].max())
    z_span = max(z_hi - z_lo, 1e-12)
    keep &= (corners[:, :, 0].max(axis=1) >= 0) & (corners[:, :, 0].min(axis=1) < size)
    keep &= (corners[:, :, 1].max(axis=1) >= 0) & (corners[:, :, 1].min(axis=1) < size)
    min_xy = np.clip(np.floor(np.clip(corners.min(axis=1), -1, size + 1)), 0, size - 1).astype(np.int64)
    max_xy = np.clip(np.floor(np.clip(corners.max(axis=1), -1, size + 1)), 0, size - 1).astype(np.int64)
    extent = np.maximum(max_xy[:, 0] - min_xy[:, 0], max_xy[:, 1] - min_xy[:, 1]) + 1
    ids = np.nonzero(keep)[0]
    lower = 0
    for edge in BUCKET_EDGES + (size + 1,):
        chosen = ids[(extent[ids] > lower) & (extent[ids] <= edge)]
        lower = edge
        if len(chosen) == 0:
            continue
        grid = np.arange(edge)
        offset_x, offset_y = np.meshgrid(grid, grid)
        offset_x = offset_x.ravel()
        offset_y = offset_y.ravel()
        per_chunk = max(1, MAX_CANDIDATES // (edge * edge))
        for start in range(0, len(chosen), per_chunk):
            batch = chosen[start:start + per_chunk]
            px = min_xy[batch, 0][:, None] + offset_x[None, :]
            py = min_xy[batch, 1][:, None] + offset_y[None, :]
            valid = (px <= max_xy[batch, 0][:, None]) & (py <= max_xy[batch, 1][:, None])
            cx = px + 0.5
            cy = py + 0.5
            c = corners[batch]
            a = area[batch][:, None]
            w0 = _edge(c[:, 1, 0][:, None], c[:, 1, 1][:, None], c[:, 2, 0][:, None], c[:, 2, 1][:, None], cx, cy) / a
            w1 = _edge(c[:, 2, 0][:, None], c[:, 2, 1][:, None], c[:, 0, 0][:, None], c[:, 0, 1][:, None], cx, cy) / a
            w2 = 1.0 - w0 - w1
            inside = valid & (w0 >= -1e-7) & (w1 >= -1e-7) & (w2 >= -1e-7)
            if not inside.any():
                continue
            zz = z[batch]
            frag_z = w0 * zz[:, 0][:, None] + w1 * zz[:, 1][:, None] + w2 * zz[:, 2][:, None]
            rows, cols = np.nonzero(inside)
            quantized = np.clip((frag_z[rows, cols] - z_lo) / z_span * 2147483647.0, 0, 2147483647).astype(np.uint64)
            fragment_keys = (quantized << np.uint64(32)) | batch[rows].astype(np.uint64)
            np.minimum.at(keys, py[rows, cols] * size + px[rows, cols], fragment_keys)
    return keys


def resolve(keys, screen, triangles, size, inverse_depth=None):
    covered = np.nonzero(keys != FAR_KEY)[0]
    triangle_ids = (keys[covered] & np.uint64(0xFFFFFFFF)).astype(np.int64)
    px = covered % size + 0.5
    py = covered // size + 0.5
    c = screen[triangles[triangle_ids]]
    area = _edge(c[:, 0, 0], c[:, 0, 1], c[:, 1, 0], c[:, 1, 1], c[:, 2, 0], c[:, 2, 1])
    w0 = _edge(c[:, 1, 0], c[:, 1, 1], c[:, 2, 0], c[:, 2, 1], px, py) / area
    w1 = _edge(c[:, 2, 0], c[:, 2, 1], c[:, 0, 0], c[:, 0, 1], px, py) / area
    bary = np.stack([w0, w1, 1.0 - w0 - w1], axis=1)
    if inverse_depth is not None:
        bary = bary * inverse_depth[triangles[triangle_ids]]
        bary /= np.maximum(bary.sum(axis=1, keepdims=True), 1e-20)
    return covered, triangle_ids, bary


def palette(count):
    return np.array([_hsv((i * 0.61803398875) % 1.0, 0.45, 0.95) for i in range(max(count, 1))])


def _hsv(h, s, v):
    i = int(h * 6) % 6
    f = h * 6 - int(h * 6)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    return [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]


def face_normals(positions, triangles):
    a = positions[triangles[:, 0]]
    n = np.cross(positions[triangles[:, 1]] - a, positions[triangles[:, 2]] - a)
    return n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-20)


def render_panel(positions, triangles, mode, size, view, triangle_groups=None, normals=None, uvs=None, cull_back=True,
                 background=(0.13, 0.13, 0.15)):
    positions = np.asarray(positions, dtype=np.float64)
    triangles = np.asarray(triangles, dtype=np.int64)
    if isinstance(view, Perspective):
        triangles, source, extra = clip_near(view.camera_space(positions), triangles)
        if extra:
            positions = extend_attribute(positions, extra)
            normals = extend_attribute(normals, extra)
            uvs = extend_attribute(uvs, extra)
        if triangle_groups is not None:
            triangle_groups = triangle_groups[source]
    screen, key_depth, inverse_depth = view.project(positions, size)
    keys = rasterize(screen, key_depth, triangles, size, cull_back=cull_back)
    image = np.empty((size * size, 3))
    image[:] = background
    covered, triangle_ids, bary = resolve(keys, screen, triangles, size, inverse_depth)
    if len(covered) == 0:
        return image.reshape(size, size, 3)
    rotation = view.rotation
    light = np.array([0.35, 0.8, 0.5])
    light /= np.linalg.norm(light)
    light_camera = light @ rotation.T
    n_face = face_normals(positions, triangles)[triangle_ids] @ rotation.T
    if mode == "flat":
        group_colors = palette(int(triangle_groups.max()) + 1 if triangle_groups is not None and len(triangle_groups) else 1)
        shade = 0.25 + 0.75 * np.abs(n_face @ light_camera)
        base = group_colors[triangle_groups[triangle_ids]] if triangle_groups is not None else np.array([0.85, 0.85, 0.85])
        image[covered] = base * shade[:, None]
    elif mode == "normals":
        n = (normals[triangles[triangle_ids]] * bary[:, :, None]).sum(axis=1)
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        image[covered] = n * 0.5 + 0.5
    elif mode == "uv":
        uv = (uvs[triangles[triangle_ids]] * bary[:, :, None]).sum(axis=1)
        checker = ((np.floor(uv[:, 0] * 8) + np.floor(uv[:, 1] * 8)) % 2).astype(np.float64)
        frac = uv - np.floor(uv)
        shade = 0.45 + 0.55 * np.abs(n_face @ light_camera)
        image[covered] = np.stack([frac[:, 0], frac[:, 1], 0.35 + 0.5 * checker], axis=1) * shade[:, None]
    return image.reshape(size, size, 3)


def render_textured(positions, triangles, size, view, material_ids, images, alpha_ids, uvs, cull_back=True,
                    background=(0.13, 0.13, 0.15)):
    positions = np.asarray(positions, dtype=np.float64)
    triangles = np.asarray(triangles, dtype=np.int64)
    if isinstance(view, Perspective):
        triangles, source, extra = clip_near(view.camera_space(positions), triangles)
        if extra:
            positions = extend_attribute(positions, extra)
            uvs = extend_attribute(uvs, extra)
        material_ids = material_ids[source]
    screen, key_depth, inverse_depth = view.project(positions, size)
    z_range = (key_depth[triangles].min(), key_depth[triangles].max()) if len(triangles) else (0.0, 1.0)
    alpha_triangles = np.isin(material_ids, sorted(alpha_ids))
    light = np.array([0.35, 0.8, 0.5])
    light /= np.linalg.norm(light)
    light_camera = light @ view.rotation.T
    normals = face_normals(positions, triangles) @ view.rotation.T
    layers = []
    for subset in (np.nonzero(~alpha_triangles)[0], np.nonzero(alpha_triangles)[0]):
        keys = rasterize(screen, key_depth, triangles, size, cull_back, subset, z_range)
        covered, triangle_ids, bary = resolve(keys, screen, triangles, size, inverse_depth)
        uv = (uvs[triangles[triangle_ids]] * bary[:, :, None]).sum(axis=1)
        rgb, alpha = sample_texture(images, material_ids[triangle_ids], uv)
        shade = 0.35 + 0.65 * np.abs(normals[triangle_ids] @ light_camera)
        layers.append((keys, covered, rgb * shade[:, None], alpha))
    image = np.empty((size * size, 3))
    image[:] = background
    opaque_keys, covered, color, _ = layers[0]
    image[covered] = color
    alpha_keys, covered, color, alpha = layers[1]
    visible = (alpha >= 0.5) & (alpha_keys[covered] < opaque_keys[covered])
    image[covered[visible]] = color[visible]
    return image.reshape(size, size, 3)


def sample_texture(images, material_ids, uv):
    rgb = np.full((len(material_ids), 3), 0.6)
    alpha = np.ones(len(material_ids))
    for key in np.unique(material_ids):
        image = images.get(int(key))
        if image is None:
            continue
        mask = material_ids == key
        height, width = image.shape[:2]
        x = np.floor((uv[mask, 0] % 1.0) * width).astype(np.int64) % width
        y = np.floor((uv[mask, 1] % 1.0) * height).astype(np.int64) % height
        rgb[mask] = image[y, x, :3]
        if image.shape[2] > 3:
            alpha[mask] = image[y, x, 3]
    return rgb, alpha


def save_grid(panels, labels, path, columns=2, title=None):
    size = panels[0].shape[0]
    rows = (len(panels) + columns - 1) // columns
    header = 22 if title else 0
    canvas = Image.new("RGB", (columns * size, rows * size + header), (20, 20, 22))
    draw = ImageDraw.Draw(canvas)
    if title:
        draw.text((6, 4), title, fill=(230, 230, 230))
    for index, (panel, label) in enumerate(zip(panels, labels)):
        tile = Image.fromarray((np.clip(panel, 0, 1) * 255).astype(np.uint8))
        x = (index % columns) * size
        y = header + (index // columns) * size
        canvas.paste(tile, (x, y))
        draw.text((x + 6, y + 4), label, fill=(255, 255, 160))
    canvas.save(path)


def standard_panels(positions, triangles, size, groups=None, normals=None, uvs=None, eye=None, target=None, fov=75.0):
    panels = [
        render_panel(positions, triangles, "flat", size, Orbit(35, 25), groups),
        render_panel(positions, triangles, "flat", size, Orbit(215, 25), groups),
        render_panel(positions, triangles, "flat", size, Orbit(0, 90), groups),
    ]
    labels = ["front-left, back faces culled", "back-right, back faces culled", "top down, back faces culled"]
    if normals is not None:
        panels.append(render_panel(positions, triangles, "normals", size, Orbit(35, 25), normals=normals))
        labels.append("vertex normals (world space as RGB)")
    if uvs is not None:
        panels.append(render_panel(positions, triangles, "uv", size, Orbit(35, 25), uvs=uvs))
        labels.append("UV0 checker")
    if eye is not None and target is not None:
        panels.append(render_panel(positions, triangles, "flat", size, Perspective(eye, target, fov), groups))
        labels.append("perspective from %s" % ", ".join("%.1f" % v for v in eye))
    else:
        panels.append(render_panel(positions, triangles, "flat", size, Orbit(35, 25), groups, cull_back=False))
        labels.append("front-left, no culling")
    return panels, labels


def contact_sheet(tiles, path, columns=8, title=None):
    size = tiles[0][1].shape[0]
    rows = (len(tiles) + columns - 1) // columns
    header = 22 if title else 0
    canvas = Image.new("RGB", (columns * size, rows * size + header), (20, 20, 22))
    draw = ImageDraw.Draw(canvas)
    if title:
        draw.text((6, 4), title, fill=(230, 230, 230))
    for index, (label, panel) in enumerate(tiles):
        tile = Image.fromarray((np.clip(panel, 0, 1) * 255).astype(np.uint8))
        x = (index % columns) * size
        y = header + (index // columns) * size
        canvas.paste(tile, (x, y))
        draw.rectangle([x, y, x + size - 1, y + size - 1], outline=(50, 50, 55))
        for line_index, line in enumerate(label.split("|")):
            draw.text((x + 4, y + 3 + 11 * line_index), line, fill=(255, 255, 160))
    canvas.save(path)


def facing_orbit(positions, triangles, default=(35.0, 25.0), threshold=0.6):
    positions = np.asarray(positions, dtype=np.float64)
    a = positions[triangles[:, 0]]
    cross = np.cross(positions[triangles[:, 1]] - a, positions[triangles[:, 2]] - a)
    total = np.linalg.norm(cross, axis=1).sum()
    direction = cross.sum(axis=0)
    if total <= 0 or np.linalg.norm(direction) < threshold * total:
        return Orbit(*default)
    direction /= np.linalg.norm(direction)
    azimuth = math.degrees(math.atan2(direction[0], direction[2]))
    elevation = math.degrees(math.asin(max(-1.0, min(1.0, direction[1]))))
    return Orbit(azimuth + 25.0, max(-80.0, min(80.0, elevation + 15.0)))
