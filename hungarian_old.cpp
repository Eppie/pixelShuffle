#include <iostream>
#include <vector>
#include <limits>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include "pngReadWrite.h"   // supplied elsewhere
#include "fmath.hpp"        // fast pow helper used in original code

// -----------------------------------------------------------------------------
// Basic colour structures and helpers (copied from pngLAB.cpp, trimmed)
// -----------------------------------------------------------------------------
struct Color {
    float L;
    float a;
    float b;
};

static inline Color RGBToLab(png_bytep px)
{
    // --- RGB -> XYZ ---
    static PowGenerator pow24(2.4);
    float R = px[0] / 255.0f;
    float G = px[1] / 255.0f;
    float B = px[2] / 255.0f;

    R = R > 0.04045f ? pow24.get((R + 0.055f) / 1.055f) : R / 12.92f;
    G = G > 0.04045f ? pow24.get((G + 0.055f) / 1.055f) : G / 12.92f;
    B = B > 0.04045f ? pow24.get((B + 0.055f) / 1.055f) : B / 12.92f;

    R *= 100.0f;
    G *= 100.0f;
    B *= 100.0f;

    float X = (R * 0.4124f) + (G * 0.3576f) + (B * 0.1805f);
    float Y = (R * 0.2126f) + (G * 0.7152f) + (B * 0.0722f);
    float Z = (R * 0.0193f) + (G * 0.1192f) + (B * 0.9505f);

    // --- XYZ -> Lab ---
    static PowGenerator cbrt(1.0/3.0);

    X /= 95.047f;
    Y /= 100.0f;
    Z /= 108.883f;

    X = X > 0.008856f ? cbrt.get(X)               : (X * 7.787f) + (16.0f/116.0f);
    Y = Y > 0.008856f ? cbrt.get(Y)               : (Y * 7.787f) + (16.0f/116.0f);
    Z = Z > 0.008856f ? cbrt.get(Z)               : (Z * 7.787f) + (16.0f/116.0f);

    Color out;
    out.L = (116.0f * Y) - 16.0f;
    out.a = 500.0f * (X - Y);
    out.b = 200.0f * (Y - Z);
    return out;
}

static inline float deltaE2(const Color &c1, const Color &c2)
{
    float dL = c1.L - c2.L;
    float da = c1.a - c2.a;
    float db = c1.b - c2.b;
    return dL*dL + da*da + db*db;
}

// -----------------------------------------------------------------------------
// O(n^3) Hungarian implementation with periodic progress printing
// -----------------------------------------------------------------------------
static void hungarian(const std::vector<float>& cost,
                      int                      n,
                      std::vector<int>&        assign,
                      png_bytep*               paletteRows,
                      int                      width,
                      int                      height,
                      const std::string&       filePrefix)
{
    const float INF = std::numeric_limits<float>::infinity();
    std::vector<float> u(n+1, 0.0f), v(n+1, 0.0f), minv(n+1);
    std::vector<int> p(n+1), way(n+1);

    const int step = std::max(1, n/100);   // print every 1% by default

    auto dump_partial = [&](const std::vector<int>& col2row,
                            int                    iter) {
        // Build a temporary RGBA buffer
        const size_t stride = static_cast<size_t>(width) * 4;
        png_bytep tmpBuf = static_cast<png_bytep>(std::malloc(stride * height));
        std::vector<png_bytep> tmpRows(height);
        for (int y = 0; y < height; ++y) tmpRows[y] = tmpBuf + y * stride;

        // Fill with black initially
        std::memset(tmpBuf, 0, stride * height);

        // Copy assigned pixels
        for (int col = 1; col <= n; ++col) {
            if (col2row[col] == 0) continue;           // not yet matched
            int src = col2row[col] - 1;                // palette index
            int tgt = col - 1;                         // target index

            int srcY = src / width, srcX = src % width;
            int tgtY = tgt / width, tgtX = tgt % width;
            std::memcpy(&tmpRows[tgtY][tgtX * 4],
                        &paletteRows[srcY][srcX * 4],
                        4);
        }

        // Filename: prefix + five‑digit counter + .png
        std::ostringstream oss;
        oss << filePrefix << std::setw(5) << std::setfill('0') << iter << ".png";
        writePNGFile(oss.str().c_str(), tmpRows.data(), false);

        std::free(tmpBuf);
    };

    for (int i=1;i<=n;++i) {
        p[0] = i;
        int j0 = 0;                  // column 0 acts as root
        std::fill(minv.begin(), minv.end(), INF);
        std::vector<char> used(n+1, 0);
        do {
            used[j0] = 1;
            int i0 = p[j0];          // current row to match
            float delta = INF;
            int j1 = 0;
            for (int j=1;j<=n;++j) if (!used[j]) {
                float cur = cost[static_cast<size_t>(i0-1)*n + (j-1)] - u[i0] - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = j; }
            }
            for (int j=0;j<=n;++j) {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else { minv[j] -= delta; }
            }
            j0 = j1;
        } while (p[j0] != 0);
        //  augmenting path
        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);

        // ---------------- progress print ----------------
        if (i % step == 0 || i==n) {
            double curCost = 0.0;
            for (int col=1; col<=n; ++col) {
                if (p[col]) {
                    curCost += cost[static_cast<size_t>(p[col]-1)*n + (col-1)];
                }
            }
            double pct = 100.0 * i / n;
            std::cout << std::fixed << std::setprecision(1)
                      << pct << "% complete | current total ΔE² = " << curCost << std::endl;
            dump_partial(p, i);           // write intermediate PNG
        }
    }

    assign.resize(n);
    for (int j=1;j<=n;++j) {
        if (p[j] != 0) assign[j-1] = p[j]-1;   // column j gets row p[j]
    }
}

// -----------------------------------------------------------------------------
// Main application logic: read palette & target, run Hungarian, write output
// -----------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <palette.png> <target.png> <output.png>\n";
        return 1;
    }

    // -------------------- load PNGs --------------------
    int sW, sH, dW, dH;
    png_bytep *paletteRows   = nullptr;
    png_bytep *targetRows    = nullptr;

    readPNGFile(argv[1], &paletteRows, &sW, &sH);
    readPNGFile(argv[2], &targetRows,  &dW, &dH);

    const int N = dW * dH;
    dWidth  = dW;
    dHeight = dH;
    if (sW * sH != N) {
        std::cerr << "Palette and target must have identical pixel counts for Hungarian run\n";
        return 1;
    }

    // Build filename prefix like "out" + original basename without extension, e.g. outmona_
    std::string outPrefix;
    {
        std::string outName = argv[3];            // requested output file
        auto slash = outName.find_last_of('/');
        if (slash != std::string::npos) outName = outName.substr(slash + 1);
        auto dot   = outName.find_last_of('.');
        if (dot   != std::string::npos) outName = outName.substr(0, dot);
        outPrefix = "out" + outName + "_";
    }

    std::cout << "Building colour arrays (" << N << " pixels)…" << std::endl;

    // -------------------- convert to Lab --------------------
    std::vector<Color> paletteLab(N);
    std::vector<Color> targetLab(N);

    for (int y=0;y<dH;++y) {
        for (int x=0;x<dW;++x) {
            int idx = y*dW + x;
            paletteLab[idx] = RGBToLab(&paletteRows[y][x*4]);
            targetLab[idx]  = RGBToLab(&targetRows [y][x*4]);
        }
    }

    std::cout << "Constructing " << N << " × " << N << " cost matrix…" << std::endl;

    // -------------------- build cost matrix --------------------
    std::vector<float> cost(static_cast<size_t>(N)*N);
    for (int i=0;i<N;++i) {
        for (int j=0;j<N;++j) {
            cost[static_cast<size_t>(i)*N + j] = deltaE2(paletteLab[i], targetLab[j]);
        }
    }

    std::cout << "Running Hungarian algorithm…" << std::endl;

    // -------------------- Hungarian assignment --------------------
    std::vector<int> assignment;               // index by target pixel, value = palette index
    hungarian(cost, N, assignment,
              paletteRows,
              dW, dH,
              outPrefix);

    // compute final cost
    double finalCost=0.0;
    for (int t=0;t<N;++t) finalCost += cost[static_cast<size_t>(assignment[t])*N + t];
    std::cout << "Hungarian complete. Final total ΔE² = " << std::fixed << std::setprecision(2) << finalCost << std::endl;

    // -------------------- build output image --------------------
    const size_t stride = static_cast<size_t>(dW) * 4;
    png_bytep outputBuffer = static_cast<png_bytep>(std::malloc(stride * dH));
    std::vector<png_bytep> outRows(dH);
    for (int y=0;y<dH;++y) outRows[y] = outputBuffer + y*stride;

    for (int tgt=0;tgt<N;++tgt) {
        int src = assignment[tgt];
        int srcY = src / dW, srcX = src % dW;
        int tgtY = tgt / dW, tgtX = tgt % dW;
        std::memcpy(&outRows[tgtY][tgtX*4], &paletteRows[srcY][srcX*4], 4);
    }

    // -------------------- write & cleanup --------------------
    writePNGFile(argv[3], outRows.data(), false);

    for (int y=0;y<sH;++y) std::free(paletteRows[y]);
    std::free(paletteRows);
    for (int y=0;y<dH;++y) std::free(targetRows[y]);
    std::free(targetRows);
    std::free(outputBuffer);
    return 0;
}
