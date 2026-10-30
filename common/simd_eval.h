// common/simd_eval.h
#pragma once
#include <cstddef>
#include "parser.h"

// Which instruction set this build uses, e.g. "AVX2 (8 floats at a time)".
const char *simd_backend_name();

// Evaluate the expression at `count` points: zs[i] = f(xs[i], ys[i]).
// Both functions give the same answers (to float precision); the second is faster.
void evaluate_grid_scalar(const expr_node *root, const float *xs, const float *ys,
                          float *zs, std::size_t count);
void evaluate_grid_simd(const expr_node *root, const float *xs, const float *ys,
                        float *zs, std::size_t count);
