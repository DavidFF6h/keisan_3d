// common/rasterizer.cpp
#include "rasterizer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

color to_color(vec3 c) {
    auto channel = [](float v) {
        if (!(v > 0.f)) return (std::uint8_t)0;     // also catches NaN, which fails every comparison
        if (v > 1.f)    return (std::uint8_t)255;
        return (std::uint8_t)(v * 255.f + 0.5f);
    };
    return { channel(c.x), channel(c.y), channel(c.z), 255 };
}

// Twice the signed area of triangle (a, b, p). Its SIGN says which side of the
// line a->b the point p is on; its MAGNITUDE grows with p's distance from that line.
static float edge(const vec4 &a, const vec4 &b, float px, float py) {
    return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

void rasterize_triangle(framebuffer &fb, zbuffer &zb, const vec4 p[3], const vec3 col[3]) {
    // Twice the area of the whole triangle. Zero means it is edge-on: skip it.
    float area = edge(p[0], p[1], p[2].x, p[2].y);
    if (area == 0.f) return;
    float inv_area = 1.f / area;

    // Only visit pixels inside the triangle's bounding box, clipped to the screen.
    float min_xf = std::min({ p[0].x, p[1].x, p[2].x });
    float max_xf = std::max({ p[0].x, p[1].x, p[2].x });
    float min_yf = std::min({ p[0].y, p[1].y, p[2].y });
    float max_yf = std::max({ p[0].y, p[1].y, p[2].y });
    if (max_xf < 0 || max_yf < 0 || min_xf >= fb.width || min_yf >= fb.height) return;

    int min_x = std::max(0, (int)std::floor(min_xf));
    int max_x = std::min(fb.width  - 1, (int)std::ceil(max_xf));
    int min_y = std::max(0, (int)std::floor(min_yf));
    int max_y = std::min(fb.height - 1, (int)std::ceil(max_yf));

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            // Test the pixel's centre.
            float px = x + 0.5f, py = y + 0.5f;

            // Barycentric coordinates: how much of each corner the point is made of.
            // They add up to 1, and are all >= 0 exactly when the point is inside.
            float b0 = edge(p[1], p[2], px, py) * inv_area;
            float b1 = edge(p[2], p[0], px, py) * inv_area;
            float b2 = edge(p[0], p[1], px, py) * inv_area;
            const float eps = -1e-5f;   // tolerance so neighbouring triangles leave no cracks
            if (b0 < eps || b1 < eps || b2 < eps) continue;

            // Blend the corners' depths with the same weights, then depth-test.
            float z = b0 * p[0].z + b1 * p[1].z + b2 * p[2].z;
            size_t idx = (size_t)y * fb.width + x;
            if (z >= zb.depth[idx]) continue;   // something closer is already here
            zb.depth[idx] = z;

            // Blend the corners' colours: Gouraud shading.
            vec3 c = col[0] * b0 + col[1] * b1 + col[2] * b2;
            fb.pixels[idx] = to_color(c);
        }
    }
}

bool write_ppm(const framebuffer &fb, const char *path) {
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", fb.width, fb.height);
    for (const color &c : fb.pixels) {
        std::fputc(c.r, f); std::fputc(c.g, f); std::fputc(c.b, f);
    }
    std::fclose(f);
    return true;
}
