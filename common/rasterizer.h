// common/rasterizer.h
#pragma once
#include <cstdint>
#include <vector>
#include <limits>
#include "vecmath.h"

struct color { std::uint8_t r, g, b, a; };

// Converts a float colour (each channel 0..1) to bytes, clamping out-of-range values.
color to_color(vec3 c);

// A grid of pixels we draw into.
struct framebuffer {
    int width, height;
    std::vector<color> pixels;

    framebuffer(int w, int h) : width(w), height(h), pixels((size_t)w * h) {}

    void clear(color c) { for (auto &p : pixels) p = c; }
};

// For every pixel, the depth of the nearest thing drawn there so far.
// Depth runs 0 (near) .. 1 (far), so "closer" means "smaller".
struct zbuffer {
    int width, height;
    std::vector<float> depth;

    zbuffer(int w, int h) : width(w), height(h), depth((size_t)w * h, FAR_DEPTH) {}

    static constexpr float FAR_DEPTH = std::numeric_limits<float>::infinity();

    void clear() { for (auto &d : depth) d = FAR_DEPTH; }
};

// Draws one triangle. p[i] are SCREEN-space positions (x, y in pixels, z = depth
// 0..1). col[i] are the colours at the three corners; the rasterizer blends them
// across the face (Gouraud shading).
void rasterize_triangle(framebuffer &fb, zbuffer &zb, const vec4 p[3], const vec3 col[3]);

// Debug helper: writes the framebuffer as a .ppm image any viewer can open.
bool write_ppm(const framebuffer &fb, const char *path);
