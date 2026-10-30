// common/mesh.h
#pragma once
#include <vector>
#include "vecmath.h"
#include "rasterizer.h"

// One point of the surface z = f(x, y), plus the direction the surface faces there.
struct gridvertex {
    vec3 pos;      // x, y = where we sampled; z = f(x, y)
    vec3 normal;   // unit vector perpendicular to the surface
};

// The camera moves on a sphere around the origin, always looking at the centre.
struct orbit_camera {
    float yaw      = 0.6f;   // rotation around the vertical axis (radians)
    float pitch    = 0.5f;   // elevation above the horizontal (radians)
    float distance = 9.f;    // how far from the centre

    vec3 eye() const;
    void clamp();            // keeps pitch and distance in a sensible range
};

// A res x res grid of sample points covering [xmin,xmax] x [ymin,ymax].
// Only x and y are filled in; z is left at 0 for the caller (the server) to compute.
std::vector<gridvertex> build_grid(float xmin, float xmax, float ymin, float ymax, int res);

// Two triangles per grid cell, as triples of indices into the vertex array.
std::vector<int> build_indices(int res);

// Fills in every vertex's normal by averaging the faces that touch it.
void compute_normals(std::vector<gridvertex> &verts, const std::vector<int> &indices);

// Draws the whole scene (bounding box, axes, surface) into fb/zb from the camera's viewpoint.
void render_scene(framebuffer &fb, zbuffer &zb, const orbit_camera &camera,
                  const std::vector<gridvertex> &verts, const std::vector<int> &indices);
