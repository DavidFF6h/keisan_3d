// common/vecmath.h
#pragma once
#include <cmath>

// ---------------------------------------------------------------- vectors --
struct vec3 { float x, y, z; };
struct vec4 { float x, y, z, w; };

inline vec3 operator+(vec3 a, vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline vec3 operator-(vec3 a, vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline vec3 operator*(vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }

inline float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline vec3 cross(vec3 a, vec3 b) {
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
}

inline vec3 normalize(vec3 v) {
    float len = std::sqrt(dot(v, v));
    // Zero-length, infinite, or NaN vectors have no direction: return zero instead
    // of dividing by zero and spreading NaNs through everything downstream.
    if (!(std::isfinite(len) && len > 1e-12f)) return { 0.f, 0.f, 0.f };
    return v * (1.f / len);
}

// A point has w = 1 (translations affect it); a direction has w = 0 (they don't).
inline vec4 as_point(vec3 v) { return { v.x, v.y, v.z, 1.f }; }

// --------------------------------------------------------------- matrices --
// Row-major 4x4 matrix. m[row][column]. We multiply column vectors: M * v.
struct mat4 {
    float m[4][4] = {};

    static mat4 identity() {
        mat4 r;
        for (int i = 0; i < 4; i++) r.m[i][i] = 1.f;
        return r;
    }
};

inline vec4 operator*(const mat4 &a, vec4 v) {
    const float in[4] = { v.x, v.y, v.z, v.w };
    float out[4] = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i] += a.m[i][j] * in[j];
    return { out[0], out[1], out[2], out[3] };
}

inline mat4 operator*(const mat4 &a, const mat4 &b) {
    mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// View matrix: moves the world so the camera sits at the origin looking down -Z.
inline mat4 look_at(vec3 eye, vec3 target, vec3 up) {
    vec3 f = normalize(target - eye);   // forward
    vec3 s = normalize(cross(f, up));   // right
    vec3 u = cross(s, f);               // true up (re-orthogonalised)

    mat4 m = mat4::identity();
    m.m[0][0] =  s.x; m.m[0][1] =  s.y; m.m[0][2] =  s.z; m.m[0][3] = -dot(s, eye);
    m.m[1][0] =  u.x; m.m[1][1] =  u.y; m.m[1][2] =  u.z; m.m[1][3] = -dot(u, eye);
    m.m[2][0] = -f.x; m.m[2][1] = -f.y; m.m[2][2] = -f.z; m.m[2][3] =  dot(f, eye);
    return m;
}

// Projection matrix: squeezes the viewing frustum into the cube [-1,1]^3 and
// leaves the distance from the camera in w, so dividing by w later shrinks
// far-away things.
inline mat4 perspective(float fov_y_radians, float aspect, float z_near, float z_far) {
    float t = 1.f / std::tan(fov_y_radians / 2.f);
    mat4 m;
    m.m[0][0] = t / aspect;
    m.m[1][1] = t;
    m.m[2][2] = (z_far + z_near) / (z_near - z_far);
    m.m[2][3] = (2.f * z_far * z_near) / (z_near - z_far);
    m.m[3][2] = -1.f;
    return m;
}

// Viewport matrix: maps the [-1,1] cube to pixels. y is flipped because
// pixel rows count downwards. Depth is mapped to [0,1]: 0 = near, 1 = far.
inline mat4 viewport(int width, int height) {
    mat4 m = mat4::identity();
    m.m[0][0] =  width  / 2.f;  m.m[0][3] = width  / 2.f;
    m.m[1][1] = -height / 2.f;  m.m[1][3] = height / 2.f;
    m.m[2][2] = 0.5f;           m.m[2][3] = 0.5f;
    return m;
}

constexpr float PI = 3.14159265358979323846f;
