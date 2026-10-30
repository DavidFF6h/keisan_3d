// common/simd_eval.cpp
#include "simd_eval.h"
#include <cmath>

// ---------------------------------------------------------------------------
// A tiny layer over the CPU's vector instructions. Everything below this block
// is written against these nine operations and doesn't care which CPU it is on.
//
//   vf          a "vector of floats": one register holding LANES numbers
//   vf_set1     fill every lane with the same number
//   vf_load     read LANES consecutive floats from memory
//   vf_store    write LANES consecutive floats to memory
//   vf_add ...  do the operation on every lane at the same time
// ---------------------------------------------------------------------------
#if defined(__AVX2__)                            // x86-64 with AVX2

#include <immintrin.h>
namespace {
using vf = __m256;
constexpr int LANES = 8;
constexpr const char *BACKEND = "AVX2 (8 floats at a time)";
inline vf   vf_set1(float a)             { return _mm256_set1_ps(a); }
inline vf   vf_load(const float *p)      { return _mm256_loadu_ps(p); }
inline void vf_store(float *p, vf v)     { _mm256_storeu_ps(p, v); }
inline vf   vf_add(vf a, vf b)           { return _mm256_add_ps(a, b); }
inline vf   vf_sub(vf a, vf b)           { return _mm256_sub_ps(a, b); }
inline vf   vf_mul(vf a, vf b)           { return _mm256_mul_ps(a, b); }
inline vf   vf_div(vf a, vf b)           { return _mm256_div_ps(a, b); }
inline vf   vf_sqrt(vf a)                { return _mm256_sqrt_ps(a); }
inline vf   vf_abs(vf a)                 { return _mm256_andnot_ps(_mm256_set1_ps(-0.f), a); } // clear the sign bit
}

#elif defined(__ARM_NEON) && defined(__aarch64__) // Apple Silicon, Raspberry Pi 4/5, ...

#include <arm_neon.h>
namespace {
using vf = float32x4_t;
constexpr int LANES = 4;
constexpr const char *BACKEND = "NEON (4 floats at a time)";
inline vf   vf_set1(float a)             { return vdupq_n_f32(a); }
inline vf   vf_load(const float *p)      { return vld1q_f32(p); }
inline void vf_store(float *p, vf v)     { vst1q_f32(p, v); }
inline vf   vf_add(vf a, vf b)           { return vaddq_f32(a, b); }
inline vf   vf_sub(vf a, vf b)           { return vsubq_f32(a, b); }
inline vf   vf_mul(vf a, vf b)           { return vmulq_f32(a, b); }
inline vf   vf_div(vf a, vf b)           { return vdivq_f32(a, b); }
inline vf   vf_sqrt(vf a)                { return vsqrtq_f32(a); }
inline vf   vf_abs(vf a)                 { return vabsq_f32(a); }
}

#else                                            // anything else: one "lane", plain floats

namespace {
using vf = float;
constexpr int LANES = 1;
constexpr const char *BACKEND = "none (scalar fallback, 1 float at a time)";
inline vf   vf_set1(float a)             { return a; }
inline vf   vf_load(const float *p)      { return *p; }
inline void vf_store(float *p, vf v)     { *p = v; }
inline vf   vf_add(vf a, vf b)           { return a + b; }
inline vf   vf_sub(vf a, vf b)           { return a - b; }
inline vf   vf_mul(vf a, vf b)           { return a * b; }
inline vf   vf_div(vf a, vf b)           { return a / b; }
inline vf   vf_sqrt(vf a)                { return std::sqrt(a); }
inline vf   vf_abs(vf a)                 { return std::fabs(a); }
}

#endif

const char *simd_backend_name() { return BACKEND; }

namespace {

// Functions like sin and pow have no single CPU instruction. The honest fallback:
// spill the register to memory, run the normal scalar function on each lane, and
// load the results back. Correct, but those nodes don't get the speed-up.
template <typename F>
vf map_lanes(vf v, F f) {
    float lanes[LANES];
    vf_store(lanes, v);
    for (int i = 0; i < LANES; i++) lanes[i] = f(lanes[i]);
    return vf_load(lanes);
}

template <typename F>
vf map_lanes(vf a, vf b, F f) {
    float la[LANES], lb[LANES];
    vf_store(la, a);
    vf_store(lb, b);
    for (int i = 0; i < LANES; i++) la[i] = f(la[i], lb[i]);
    return vf_load(la);
}

// The same tree walk as eval() in parser.cpp, except every "number" is now a
// whole register of LANES numbers: one walk computes LANES grid points.
vf eval_vec(const expr_node *n, vf x, vf y) {
    switch (n->kind) {
        case node_kind::num: return vf_set1((float)n->value);
        case node_kind::var: return n->name == 'x' ? x : y;
        case node_kind::add: return vf_add(eval_vec(n->lhs.get(), x, y), eval_vec(n->rhs.get(), x, y));
        case node_kind::sub: return vf_sub(eval_vec(n->lhs.get(), x, y), eval_vec(n->rhs.get(), x, y));
        case node_kind::mul: return vf_mul(eval_vec(n->lhs.get(), x, y), eval_vec(n->rhs.get(), x, y));
        case node_kind::div: return vf_div(eval_vec(n->lhs.get(), x, y), eval_vec(n->rhs.get(), x, y));
        case node_kind::neg: return vf_mul(vf_set1(-1.f), eval_vec(n->lhs.get(), x, y));  // exact sign flip (0 - v would turn -0 into +0)
        case node_kind::pow:
            return map_lanes(eval_vec(n->lhs.get(), x, y), eval_vec(n->rhs.get(), x, y),
                             [](float a, float b) { return std::pow(a, b); });
        case node_kind::call: {
            vf v = eval_vec(n->lhs.get(), x, y);
            switch (n->fn) {
                case func_id::sqrt: return vf_sqrt(v);   // real instructions
                case func_id::abs:  return vf_abs(v);
                case func_id::sin:  return map_lanes(v, [](float a) { return std::sin(a); });
                case func_id::cos:  return map_lanes(v, [](float a) { return std::cos(a); });
                case func_id::tan:  return map_lanes(v, [](float a) { return std::tan(a); });
                case func_id::exp:  return map_lanes(v, [](float a) { return std::exp(a); });
                case func_id::log:  return map_lanes(v, [](float a) { return std::log(a); });
            }
        }
    }
    return vf_set1(0.f);
}

} // namespace

void evaluate_grid_scalar(const expr_node *root, const float *xs, const float *ys,
                          float *zs, std::size_t count)
{
    for (std::size_t i = 0; i < count; i++)
        zs[i] = (float)eval(root, xs[i], ys[i]);
}

void evaluate_grid_simd(const expr_node *root, const float *xs, const float *ys,
                        float *zs, std::size_t count)
{
    std::size_t i = 0;

    // Main loop: LANES points per iteration.
    for (; i + LANES <= count; i += LANES)
        vf_store(zs + i, eval_vec(root, vf_load(xs + i), vf_load(ys + i)));

    // Leftovers: when count isn't a multiple of LANES, finish one at a time.
    for (; i < count; i++)
        zs[i] = (float)eval(root, xs[i], ys[i]);
}
