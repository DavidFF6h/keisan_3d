// bench/main.cpp
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "parser.h"
#include "simd_eval.h"

// Runs fn() several times and returns the fastest run in milliseconds. The
// minimum is the least noisy statistic: slower runs only ever mean the OS got in the way.
template <typename F>
static double best_of(int runs, F &&fn) {
    double best = 1e30;
    for (int r = 0; r < runs; r++) {
        auto t0 = std::chrono::steady_clock::now();
        fn();
        auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

int main() {
    std::printf("SIMD backend: %s\n\n", simd_backend_name());

    const char *formulas[] = {
        "x*x - y*y",              // pure arithmetic: fully vectorised
        "sqrt(x*x + y*y) / 2",    // arithmetic + a native sqrt instruction
        "sin(x) * cos(y)",        // sin/cos fall back to per-lane scalar calls
    };
    const int sizes[] = { 100, 316, 1000 };   // grid side; points = side^2 (10k, 100k, 1M)

    std::printf("%-22s %10s %12s %12s %9s %12s\n",
                "formula", "points", "scalar ms", "simd ms", "speedup", "max |diff|");

    for (const char *formula : formulas) {
        node_ptr ast = parse_expression(formula);

        for (int side : sizes) {
            std::size_t n = (std::size_t)side * side;
            std::vector<float> xs(n), ys(n), z_scalar(n), z_simd(n);
            for (int j = 0; j < side; j++)
                for (int i = 0; i < side; i++) {
                    xs[(std::size_t)j * side + i] = -3.f + 6.f * i / (side - 1);
                    ys[(std::size_t)j * side + i] = -3.f + 6.f * j / (side - 1);
                }

            double t_scalar = best_of(5, [&] { evaluate_grid_scalar(ast.get(), xs.data(), ys.data(), z_scalar.data(), n); });
            double t_simd   = best_of(5, [&] { evaluate_grid_simd(ast.get(), xs.data(), ys.data(), z_simd.data(), n); });

            // The two paths must agree (to float rounding), or the speed-up means nothing.
            double max_diff = 0;
            for (std::size_t i = 0; i < n; i++) {
                if (std::isnan(z_scalar[i]) && std::isnan(z_simd[i])) continue;
                max_diff = std::max(max_diff, (double)std::fabs(z_scalar[i] - z_simd[i]));
            }

            std::printf("%-22s %10zu %12.3f %12.3f %8.2fx %12.2e\n",
                        formula, n, t_scalar, t_simd, t_scalar / t_simd, max_diff);
        }
    }
    return 0;
}
