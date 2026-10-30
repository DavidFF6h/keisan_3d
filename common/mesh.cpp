// common/mesh.cpp
#include "mesh.h"
#include <algorithm>
#include <cmath>
#include <limits>

// ------------------------------------------------------------------ camera --

vec3 orbit_camera::eye() const {
    return { distance * std::cos(pitch) * std::sin(yaw),
             distance * std::sin(pitch),
             distance * std::cos(pitch) * std::cos(yaw) };
}

void orbit_camera::clamp() {
    pitch    = std::clamp(pitch, -1.5f, 1.5f);   // stop just short of straight up/down
    distance = std::clamp(distance, 5.f, 30.f);  // never inside the plot box
}

// -------------------------------------------------------------------- mesh --

std::vector<gridvertex> build_grid(float xmin, float xmax, float ymin, float ymax, int res) {
    if (res < 2) res = 2;
    std::vector<gridvertex> verts((size_t)res * res);
    for (int j = 0; j < res; j++) {
        for (int i = 0; i < res; i++) {
            float x = xmin + (xmax - xmin) * i / (res - 1);
            float y = ymin + (ymax - ymin) * j / (res - 1);
            verts[(size_t)j * res + i] = { { x, y, 0.f }, { 0.f, 0.f, 1.f } };
        }
    }
    return verts;
}

std::vector<int> build_indices(int res) {
    if (res < 2) res = 2;
    std::vector<int> idx;
    idx.reserve((size_t)(res - 1) * (res - 1) * 6);
    for (int j = 0; j < res - 1; j++) {
        for (int i = 0; i < res - 1; i++) {
            int a = j * res + i;   // this cell's four corners, seen from above (x right, y up):
            int b = a + 1;         //                            c --- d
            int c = a + res;       //                            |     |
            int d = c + 1;         //                            a --- b
            idx.insert(idx.end(), { a, b, c,   b, d, c });   // counter-clockwise seen from above, so normals point up
        }
    }
    return idx;
}

void compute_normals(std::vector<gridvertex> &verts, const std::vector<int> &indices) {
    for (auto &v : verts) v.normal = { 0.f, 0.f, 0.f };

    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        gridvertex &v0 = verts[indices[t]];
        gridvertex &v1 = verts[indices[t + 1]];
        gridvertex &v2 = verts[indices[t + 2]];

        // The cross product of two edges is perpendicular to the face, and its
        // length is proportional to the face's area, so big faces count for more.
        vec3 n = cross(v1.pos - v0.pos, v2.pos - v0.pos);
        v0.normal = v0.normal + n;
        v1.normal = v1.normal + n;
        v2.normal = v2.normal + n;
    }
    for (auto &v : verts) v.normal = normalize(v.normal);
}

// ---------------------------------------------------------------- rendering --

namespace {

// The plot lives inside a cube of half-size BOX, however large or small the data is.
constexpr float BOX = 2.f;

// How to squeeze the data's bounding box into that cube, one axis at a time.
struct view_fit {
    float lo[3], hi[3];       // data range on x, y, z (finite values only)
    float center[3], scale[3];
};

view_fit fit_view(const std::vector<gridvertex> &verts) {
    const float inf = std::numeric_limits<float>::infinity();
    view_fit f = { { inf, inf, inf }, { -inf, -inf, -inf }, {}, {} };

    for (const gridvertex &v : verts) {
        const float p[3] = { v.pos.x, v.pos.y, v.pos.z };
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
        for (int a = 0; a < 3; a++) {
            f.lo[a] = std::min(f.lo[a], p[a]);
            f.hi[a] = std::max(f.hi[a], p[a]);
        }
    }
    for (int a = 0; a < 3; a++) {
        if (f.lo[a] > f.hi[a]) { f.lo[a] = -1.f; f.hi[a] = 1.f; }   // no data at all
        float extent = f.hi[a] - f.lo[a];
        f.center[a] = (f.lo[a] + f.hi[a]) / 2.f;
        f.scale[a]  = extent > 1e-9f ? (2.f * BOX) / extent : 1.f;
    }
    return f;
}

// Data space -> the box -> world space. Maths puts "up" on z, but our camera
// puts "up" on y, so (x, y, z) becomes (x, z, -y): a 90-degree turn about the x axis.
vec3 box_to_world(float qx, float qy, float qz) { return { qx, qz, -qy }; }

vec3 data_to_world(const view_fit &f, vec3 p) {
    return box_to_world((p.x - f.center[0]) * f.scale[0],
                        (p.y - f.center[1]) * f.scale[1],
                        (p.z - f.center[2]) * f.scale[2]);
}

// Normals do NOT transform like positions: stretching a surface along one axis
// tilts its normals the other way. The right rule is to divide by the scale.
vec3 normal_to_world(const view_fit &f, vec3 n) {
    vec3 q = { n.x / f.scale[0], n.y / f.scale[1], n.z / f.scale[2] };
    return normalize(box_to_world(q.x, q.y, q.z));
}

// World position -> screen pixel (x, y) and depth (z). Returns false if the point
// is behind the camera, where the perspective divide would be meaningless.
bool project(const mat4 &view_proj_viewport, vec3 world, vec4 &screen) {
    vec4 clip = view_proj_viewport * as_point(world);
    if (clip.w < 0.1f) return false;
    screen = { clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, 1.f };
    return true;
}

// A line is drawn as a thin rectangle (two triangles) a pixel and a half wide.
void draw_line(framebuffer &fb, zbuffer &zb, const mat4 &vpv, vec3 a, vec3 b, vec3 rgb) {
    vec4 sa, sb;
    if (!project(vpv, a, sa) || !project(vpv, b, sb)) return;

    float dx = sb.x - sa.x, dy = sb.y - sa.y;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    float nx = -dy / len * 0.75f, ny = dx / len * 0.75f;   // sideways offset, in pixels

    const float bias = 1e-3f;                              // nudge toward the camera so lines
    vec4 q0 = { sa.x + nx, sa.y + ny, sa.z - bias, 1.f };  // don't z-fight with the surface
    vec4 q1 = { sa.x - nx, sa.y - ny, sa.z - bias, 1.f };
    vec4 q2 = { sb.x + nx, sb.y + ny, sb.z - bias, 1.f };
    vec4 q3 = { sb.x - nx, sb.y - ny, sb.z - bias, 1.f };

    const vec3 c[3] = { rgb, rgb, rgb };
    const vec4 t1[3] = { q0, q1, q2 };
    const vec4 t2[3] = { q1, q2, q3 };
    rasterize_triangle(fb, zb, t1, c);
    rasterize_triangle(fb, zb, t2, c);
}

// Three-stop colour ramp: blue (low) -> green -> yellow (high). t is 0..1.
vec3 height_color(float t) {
    const vec3 stops[3] = { { 0.16f, 0.30f, 0.85f }, { 0.20f, 0.80f, 0.50f }, { 0.98f, 0.85f, 0.25f } };
    t = std::clamp(t, 0.f, 1.f) * 2.f;
    int i = t >= 1.f ? 1 : 0;
    float f = t - (float)i;
    return stops[i] * (1.f - f) + stops[i + 1] * f;
}

} // namespace

void render_scene(framebuffer &fb, zbuffer &zb, const orbit_camera &camera,
                  const std::vector<gridvertex> &verts, const std::vector<int> &indices)
{
    fb.clear({ 18, 18, 24, 255 });
    zb.clear();

    // The three matrices from vecmath.h, combined into one.
    mat4 view = look_at(camera.eye(), { 0.f, 0.f, 0.f }, { 0.f, 1.f, 0.f });
    mat4 proj = perspective(45.f * PI / 180.f, (float)fb.width / fb.height, 0.1f, 100.f);
    mat4 vpv  = viewport(fb.width, fb.height) * proj * view;

    view_fit fit = fit_view(verts);

    // --- the plot box: 12 dim edges ---------------------------------------
    for (int i = 0; i < 8; i++) {
        for (int bit = 0; bit < 3; bit++) {
            int j = i ^ (1 << bit);                   // the corner that differs in one coordinate
            if (i > j) continue;                      // draw each edge once
            vec3 a = { (i & 1) ? BOX : -BOX, (i & 2) ? BOX : -BOX, (i & 4) ? BOX : -BOX };
            vec3 b = { (j & 1) ? BOX : -BOX, (j & 2) ? BOX : -BOX, (j & 4) ? BOX : -BOX };
            draw_line(fb, zb, vpv, a, b, { 0.30f, 0.30f, 0.36f });
        }
    }

    // --- the axes: cross where the true origin is (or the nearest point of the box) ---
    float origin[3];
    for (int a = 0; a < 3; a++)
        origin[a] = std::clamp((0.f - fit.center[a]) * fit.scale[a], -BOX, BOX);
    draw_line(fb, zb, vpv, box_to_world(-BOX, origin[1], origin[2]),
                           box_to_world( BOX, origin[1], origin[2]), { 0.90f, 0.25f, 0.25f }); // x: red
    draw_line(fb, zb, vpv, box_to_world(origin[0], -BOX, origin[2]),
                           box_to_world(origin[0],  BOX, origin[2]), { 0.25f, 0.85f, 0.25f }); // y: green
    draw_line(fb, zb, vpv, box_to_world(origin[0], origin[1], -BOX),
                           box_to_world(origin[0], origin[1],  BOX), { 0.35f, 0.55f, 1.00f }); // z: blue

    // --- the surface -----------------------------------------------------
    // Work out each vertex's screen position and colour ONCE; every vertex is
    // shared by up to six triangles, so doing it per triangle would be 6x the work.
    const size_t n = verts.size();
    std::vector<vec4> screen(n);
    std::vector<vec3> shade(n);
    std::vector<char> usable(n, 0);

    const vec3 light = normalize({ 0.35f, 0.90f, 0.45f });   // fixed light, in world space
    float z_extent = fit.hi[2] - fit.lo[2];

    for (size_t i = 0; i < n; i++) {
        const gridvertex &v = verts[i];
        if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y) || !std::isfinite(v.pos.z)) continue;
        if (!project(vpv, data_to_world(fit, v.pos), screen[i])) continue;

        float lambert = std::fabs(dot(normal_to_world(fit, v.normal), light));   // two-sided
        if (!std::isfinite(lambert)) lambert = 0.f;
        float t = z_extent > 1e-9f ? (v.pos.z - fit.lo[2]) / z_extent : 0.5f;

        shade[i]  = height_color(t) * (0.35f + 0.65f * lambert);   // ambient + diffuse
        usable[i] = 1;
    }

    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        size_t i0 = (size_t)indices[t], i1 = (size_t)indices[t + 1], i2 = (size_t)indices[t + 2];
        if (i0 >= n || i1 >= n || i2 >= n) continue;
        if (!usable[i0] || !usable[i1] || !usable[i2]) continue;   // skip NaN / off-camera corners

        const vec4 p[3] = { screen[i0], screen[i1], screen[i2] };
        const vec3 c[3] = { shade[i0], shade[i1], shade[i2] };
        rasterize_triangle(fb, zb, p, c);
    }
}
