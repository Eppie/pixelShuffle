#include "fmath.hpp"
#include "pngReadWrite.h"
#include <atomic>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <omp.h>
#include <random>
#include <sstream>
#include <vector>

#if 1
#define INLINE_OPTIONAL __attribute__((noinline))
#else
#define INLINE_OPTIONAL inline
#endif

using std::vector;
using std::min;
using std::cerr;
using std::endl;
using std::cout;
using std:: string;
using std::setw;
using std::atomic;
using std::mutex;
using std::setfill;
using std::ostringstream;
using std::lock_guard;
using std::fixed;
using std::setprecision;
using std::uniform_int_distribution;
using std::mt19937;
using std::swap;
using std::numeric_limits;

struct Color {
    float L;
    float a;
    float b;
};

INLINE_OPTIONAL vector<int> commonDivisors(const int a, const int b) {
    vector<int> out;
    const int lim = min(a, b);
    for (int d = 1; d <= lim; ++d)
        if (a % d == 0 && b % d == 0)
            out.push_back(d);
    return out;
}

INLINE_OPTIONAL Color neighborhoodAverage(const vector<Color> &buf, const int dW, const int dH, const int R,
                                          const int idx) {
    const int y = idx / dW;
    const int x = idx % dW;
    float sumL = 0;
    float suma = 0;
    float sumb = 0;
    int cnt = 0;
    for (int dy = -R; dy <= R; ++dy) {
        const int yy = y + dy;
        if (yy < 0 || yy >= dH) continue;
        for (int dx = -R; dx <= R; ++dx) {
            const int xx = x + dx;
            if (xx < 0 || xx >= dW) continue;
            const int id = yy * dW + xx;
            const Color &c = buf[id];
            sumL += c.L;
            suma += c.a;
            sumb += c.b;
            ++cnt;
        }
    }
    return Color{sumL / cnt, suma / cnt, sumb / cnt};
}

INLINE_OPTIONAL Color RGBToLab(png_bytep px) {
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
    static PowGenerator cbrt(1.0 / 3.0);

    X /= 95.047f;
    Y /= 100.0f;
    Z /= 108.883f;

    X = X > 0.008856f ? cbrt.get(X) : (X * 7.787f) + (16.0f / 116.0f);
    Y = Y > 0.008856f ? cbrt.get(Y) : (Y * 7.787f) + (16.0f / 116.0f);
    Z = Z > 0.008856f ? cbrt.get(Z) : (Z * 7.787f) + (16.0f / 116.0f);

    Color out;
    out.L = (116.0f * Y) - 16.0f;
    out.a = 500.0f * (X - Y);
    out.b = 200.0f * (Y - Z);
    return out;
}

INLINE_OPTIONAL float deltaE2(const Color &c1, const Color &c2) {
    float dL = c1.L - c2.L;
    float da = c1.a - c2.a;
    float db = c1.b - c2.b;
    return dL * dL + da * da + db * db;
}

/* -----------------------------------------------------------------------------
   Jonker–Volgenant Linear Assignment (LAPJV) algorithm
   Replaces the previous Hungarian implementation.
   Complexity O(n³) with a lower constant factor.

   Input  : cost   — flattened row‑major n×n cost matrix
                     (row = palette pixel, col = target pixel)
            n      — dimension of the problem
   Output : assign — size‑n vector with assign[col] = palette row chosen
----------------------------------------------------------------------------- */
INLINE_OPTIONAL void lapjv(const vector<float> &cost, int n,
                           vector<int> &assign) {
    constexpr float INF = numeric_limits<float>::infinity();

    /* Dual variables and matchings */
    vector<float> u(n, 0.0f), v(n, 0.0f);
    vector<int> colOfRow(n, -1), rowOfCol(n, -1);
    vector<int> pred(n);
    vector<float> dist(n);
    vector<char> scanned(n);

    /* ---------- Phase 1: column reduction & trivial assignments ---------- */
    for (int j = 0; j < n; ++j) {
        float minVal = cost[j];
        int minRow = 0;
        for (int i = 1; i < n; ++i) {
            float c = cost[i * n + j];
            if (c < minVal) {
                minVal = c;
                minRow = i;
            }
        }
        v[j] = minVal;
        if (colOfRow[minRow] == -1) {
            colOfRow[minRow] = j;
            rowOfCol[j] = minRow;
        } else if (cost[minRow * n + j] - minVal <
                   cost[minRow * n + colOfRow[minRow]] - v[colOfRow[minRow]]) {
            rowOfCol[colOfRow[minRow]] = -1;
            colOfRow[minRow] = j;
            rowOfCol[j] = minRow;
        }
    }

    /* ---------- Phase 2: augmenting‑path search (shortest‑path with potentials) */
    vector<int> freeRows;
    for (int i = 0; i < n; ++i)
        if (colOfRow[i] == -1)
            freeRows.push_back(i);

    while (!freeRows.empty()) {
        int i0 = freeRows.back();
        freeRows.pop_back();

        fill(dist.begin(), dist.end(), INF);
        fill(pred.begin(), pred.end(), -1);
        fill(scanned.begin(), scanned.end(), 0);

        for (int j = 0; j < n; ++j) {
            dist[j] = cost[i0 * n + j] - u[i0] - v[j];
            pred[j] = i0;
        }

        int jStar = -1;
        while (true) {
            /* choose closest unscanned column */
            float delta = INF;
            int j = -1;
            for (int jj = 0; jj < n; ++jj)
                if (!scanned[jj] && dist[jj] < delta) {
                    delta = dist[jj];
                    j = jj;
                }

            scanned[j] = 1;
            int i = rowOfCol[j];
            if (i == -1) {
                jStar = j;
                break;
            }

            /* update dual variables */
            for (int jj = 0; jj < n; ++jj) {
                if (scanned[jj]) {
                    v[jj] += delta;
                    u[rowOfCol[jj]] -= delta;
                } else {
                    dist[jj] -= delta;
                }
            }

            /* relax edges leaving newly scanned row i */
            for (int jj = 0; jj < n; ++jj) {
                if (scanned[jj])
                    continue;
                const float cur = cost[i * n + jj] - u[i] - v[jj];
                if (cur < dist[jj]) {
                    dist[jj] = cur;
                    pred[jj] = i;
                }
            }
        }

        /* ---------- Augment along the shortest path found ---------- */
        while (jStar != -1) {
            const int i = pred[jStar];
            const int nextJ = colOfRow[i];
            rowOfCol[jStar] = i;
            colOfRow[i] = jStar;
            jStar = nextJ;
        }
    }

    /* ---------- Output conversion: target‑indexed assignment ---------- */
    assign.resize(n);
    for (int j = 0; j < n; ++j)
        assign[j] = rowOfCol[j];
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        cerr << "Usage: " << argv[0]
                << " <palette.png> <target.png> <output.png>\n";
        return 1;
    }

#ifdef _OPENMP
  omp_set_dynamic(0);
  omp_set_num_threads(omp_get_max_threads());
#endif
    // -------------------- load PNGs --------------------
    int sW, sH, dW, dH;
    png_bytep *paletteRows = nullptr;
    png_bytep *targetRows = nullptr;

    readPNGFile(argv[1], &paletteRows, &sW, &sH);
    readPNGFile(argv[2], &targetRows, &dW, &dH);

    const int N = dW * dH;
    dWidth = dW;
    dHeight = dH;
    if (sW * sH != N) {
        cerr << "Palette and target must have identical pixel counts for "
                "Hungarian run\n";
        return 1;
    }

    cout << "Building colour arrays (" << N << " pixels)…" << endl;

    // -------------------- convert to Lab --------------------
    vector<Color> paletteLab(N);
    vector<Color> targetLab(N);

    /* palette image: row‑major by sW */
    for (int y = 0; y < sH; ++y) {
        for (int x = 0; x < sW; ++x) {
            int idx = y * sW + x;
            paletteLab[idx] = RGBToLab(&paletteRows[y][x * 4]);
        }
    }

    /* target image: row‑major by dW */
    for (int y = 0; y < dH; ++y) {
        for (int x = 0; x < dW; ++x) {
            int idx = y * dW + x;
            targetLab[idx] = RGBToLab(&targetRows[y][x * 4]);
        }
    }
    // -------------------- patch-based assignment --------------------
    //
    // Goal: choose a patch grid whose patches are roughly square and whose
    // area is close to 128x128 = 16384 pixels, while still covering the
    // image exactly (patch boundaries fall on integer coordinates).
    // We allow *variable* patch widths/heights so the dimensions need not
    // divide the image perfectly; each patch fits between two cut‑lines.
    //
    constexpr int targetArea = 128 * 128;

    // 2) Enumerate candidate grid counts that divide *both* images so that
    //    each patch has the exact same pixel count in palette and target.

    const vector<int> wDiv = commonDivisors(sW, dW);
    const vector<int> hDiv = commonDivisors(sH, dH);

    int bestPx = 1, bestPy = 1;
    double bestScore = numeric_limits<double>::infinity();

    for (int px: wDiv) {
        for (int py: hDiv) {
            double patchWf = static_cast<double>(dW) / px; // integer by construction
            double patchHf = static_cast<double>(dH) / py;
            double area = patchWf * patchHf;
            double score = abs(area - targetArea) // area near 128×128
                           + 0.5 * abs(patchWf - patchHf); // prefer square
            if (score < bestScore) {
                bestScore = score;
                bestPx = px;
                bestPy = py;
            }
        }
    }

    // Fall back to the single‑patch case if no divisor was found (shouldn’t
    // happen because 1 divides everything).
    const int pxCnt = bestPx;
    const int pyCnt = bestPy;
    const int totalPatches = pxCnt * pyCnt;

    // 3) Pre‑compute cut positions so that the first <extra> stripes get +1
    // pixel.
    vector<int> xCut(pxCnt + 1);
    vector<int> yCut(pyCnt + 1);

    const int baseW = dW / pxCnt;
    const int extraW = dW % pxCnt;
    xCut[0] = 0;
    for (int i = 0; i < pxCnt; ++i)
        xCut[i + 1] = xCut[i] + baseW + (i < extraW ? 1 : 0);

    const int baseH = dH / pyCnt;
    const int extraH = dH % pyCnt;
    yCut[0] = 0;
    for (int i = 0; i < pyCnt; ++i)
        yCut[i + 1] = yCut[i] + baseH + (i < extraH ? 1 : 0);

    // --- build palette cut‑lines so that palette and target use the same
    // (pxCnt,pyCnt) grid ---
    vector<int> sxCut(pxCnt + 1);
    vector<int> syCut(pyCnt + 1);

    const int sBaseW = sW / pxCnt;
    const int sExtraW = sW % pxCnt;
    sxCut[0] = 0;
    for (int i = 0; i < pxCnt; ++i)
        sxCut[i + 1] = sxCut[i] + sBaseW + (i < sExtraW ? 1 : 0);

    const int sBaseH = sH / pyCnt;
    const int sExtraH = sH % pyCnt;
    syCut[0] = 0;
    for (int i = 0; i < pyCnt; ++i)
        syCut[i + 1] = syCut[i] + sBaseH + (i < sExtraH ? 1 : 0);

    cout << "Patch grid: " << pxCnt << " × " << pyCnt << " (variable sizes, "
            << totalPatches << " patches total)…" << endl;

    // Allocate the output buffer now (cleared to transparent black).
    const size_t stride = static_cast<size_t>(dW) * 4;
    png_bytep outputBuffer = static_cast<png_bytep>(malloc(stride * dH));
    vector<png_bytep> outRows(dH);
    for (int y = 0; y < dH; ++y) {
        outRows[y] = outputBuffer + y * stride;
        memset(outRows[y], 0, stride);
    }

    // Build a filename prefix like "outTARGET_"
    string outPrefix; {
        string outName = argv[3];
        auto slash = outName.find_last_of('/');
        if (slash != string::npos)
            outName = outName.substr(slash + 1);
        auto dot = outName.find_last_of('.');
        if (dot != string::npos)
            outName = outName.substr(0, dot);
        outPrefix = "out" + outName + "_";
    }

    // Helper: dump current output PNG to disk
    auto dump_partial = [&](int iter) {
        ostringstream oss;
        oss << outPrefix << setw(5) << setfill('0') << iter << ".png";
        writePNGFile(oss.str().c_str(), outRows.data(), false);
    };

    cout << "Solving patches (target 128×128)..." << endl;

    atomic<int> patchCounter{0}; // global progress
    mutex ioMutex; // protects PNG writes & cout
    double globalCost = 0.0; // accumulated in critical+reduction

    /* OpenMP: parallelize over the 2‑D grid with guided scheduling ---------- */
#pragma omp parallel for collapse(2) schedule(guided, 1)
    for (int py = 0; py < pyCnt; ++py) {
        for (int px = 0; px < pxCnt; ++px) {
            /* --------------- compute patch bounds ---------------- */
            const int y0 = yCut[py];
            const int y1 = yCut[py + 1];
            const int pPatchH = syCut[py + 1] - syCut[py];
            const int tPatchH = y1 - y0;

            /* target patch bounds */
            const int tx0 = xCut[px];
            const int tx1 = xCut[px + 1];
            const int tPatchW = tx1 - tx0;

            /* palette patch bounds */
            const int px0 = sxCut[px];
            const int px1 = sxCut[px + 1];
            const int pPatchW = px1 - px0;

            const int nPatch = tPatchW * tPatchH;

            /* --------------- local containers -------------------- */
            vector<int> idx(nPatch);
            vector<int> pIdx(nPatch);
            vector<float> cost(static_cast<size_t>(nPatch) * nPatch);

            // gather *target* indices
            for (int y = 0; y < tPatchH; ++y)
                for (int x = 0; x < tPatchW; ++x)
                    idx[y * tPatchW + x] = (y0 + y) * dW + (tx0 + x);

            // gather *palette* indices
            for (int y = 0; y < pPatchH; ++y)
                for (int x = 0; x < pPatchW; ++x)
                    pIdx[y * pPatchW + x] = (syCut[py] + y) * sW + (px0 + x);

            // build cost matrix
            for (int i = 0; i < nPatch; ++i)
                for (int j = 0; j < nPatch; ++j)
                    cost[static_cast<size_t>(i) * nPatch + j] =
                            deltaE2(paletteLab[pIdx[i]], targetLab[idx[j]]);

            // solve LAP
            vector<int> assignment;
            lapjv(cost, nPatch, assignment);

            /* copy pixels into shared output buffer ------------------------- */
            for (int t = 0; t < nPatch; ++t) {
                int src = pIdx[assignment[t]];
                int tgt = idx[t];

                int srcY = src / sW, srcX = src % sW;
                int tgtY = tgt / dW, tgtX = tgt % dW;

                memcpy(&outRows[tgtY][tgtX * 4], &paletteRows[srcY][srcX * 4], 4);
            }

            /* accumulate patch cost locally */
            double patchCost = 0.0;
            for (int t = 0; t < nPatch; ++t)
                patchCost += cost[static_cast<size_t>(assignment[t]) * nPatch + t];

            /* ---------------- progress reporting --------------------------- */
            int curIndex = ++patchCounter; // atomic fetch‑add

            {
                lock_guard<mutex> lk(ioMutex);
                globalCost += patchCost;
                cout << "Patch " << curIndex << "/" << totalPatches
                        << " done, patch ΔE² = " << fixed << setprecision(2)
                        << patchCost << endl;
            }

            dump_partial(curIndex);
        }
    }

    cout << "All patches complete. Final total ΔE² = " << fixed
            << setprecision(2) << globalCost << endl;

    // -------------------- write & cleanup --------------------

    writePNGFile(argv[3], outRows.data(), false);

    // -------------------- 3×3‑neighbourhood dithering variants --------------
    //
    // Produce 11 variants with detail ∈ {0.0, 0.1, …, 1.0}.  Each variant
    // starts from the *same* baseline (the LAPJV result) so they are
    // independent.  We keep maxIters modest per variant to bound runtime.
    //
    {
        const int maxItersPerVariant = min<int>(N * 10, 200000000);
        cout << "Generating neighbourhood‑aware variants (detail 0.0‑1.0, "
                << "window 5×5)…" << endl;

        // Pre‑compute the original LAP output’s Lab colours once.
        vector<Color> baseLab(N);
        for (int y = 0; y < dH; ++y)
            for (int x = 0; x < dW; ++x)
                baseLab[y * dW + x] = RGBToLab(&outRows[y][x * 4]);

        constexpr int R = 2; // neighbourhood radius ⇒ 2R+1×2R+1 window

        auto weightedErr = [&](const vector<Color> &buf, const int idx,
                               const float detail) -> float {
            const Color nAvg = neighborhoodAverage(buf, dW, dH, R, idx);
            const float neighErr = deltaE2(nAvg, targetLab[idx]);
            const float centErr = deltaE2(buf[idx], targetLab[idx]);
            return detail * centErr + (1.0f - detail) * neighErr;
        };

        /* parallel over detail variants – each iteration independent */
#pragma omp parallel for schedule(dynamic)
        for (int dv = 0; dv <= 100; ++dv) {
            float detail = dv * 0.01f;

            /* thread‑local RNG seeded uniquely per variant + thread */
            unsigned seed = 987654u ^ static_cast<unsigned>(detail * 1000.0f) ^
                            static_cast<unsigned>(omp_get_thread_num() * 1234);
            mt19937 rng(seed);
            uniform_int_distribution<int> pick(0, N - 1);

            /* working Lab + RGBA buffers local to this variant */
            vector<Color> workLab = baseLab; // start fresh

            vector<png_byte> scratchRGBA(static_cast<size_t>(dH) * stride);
            for (int y = 0; y < dH; ++y)
                memcpy(&scratchRGBA[y * stride], outRows[y], stride);

            vector<png_bytep> scratchRows(dH);
            for (int y = 0; y < dH; ++y)
                scratchRows[y] = scratchRGBA.data() + y * stride;

            for (int it = 0; it < maxItersPerVariant; ++it) {
                int i = pick(rng), j = pick(rng);
                if (i == j)
                    continue;

                float before =
                        weightedErr(workLab, i, detail) + weightedErr(workLab, j, detail);

                swap(workLab[i], workLab[j]);

                float after =
                        weightedErr(workLab, i, detail) + weightedErr(workLab, j, detail);

                if (after < before) {
                    int iy = i / dW, ix = i % dW;
                    int jy = j / dW, jx = j % dW;
                    png_bytep p1 = &scratchRows[iy][ix * 4];
                    png_bytep p2 = &scratchRows[jy][jx * 4];
                    for (int k = 0; k < 4; ++k)
                        swap(p1[k], p2[k]);
                } else {
                    swap(workLab[i], workLab[j]);
                }
            }

            /* filename generation + write guarded to avoid libpng race */
            {
                ostringstream fname;
                fname << argv[3] << "_dither_" << fixed << setprecision(3)
                        << detail << ".png"; {
                    writePNGFile(fname.str().c_str(), scratchRows.data(), false);
                    cout << "  • detail " << fixed << setprecision(3)
                            << detail << " done (" << maxItersPerVariant << " swaps)\n";
                }
            }
        }
    }

    for (int y = 0; y < sH; ++y)
        free(paletteRows[y]);
    free(paletteRows);
    for (int y = 0; y < dH; ++y)
        free(targetRows[y]);
    free(targetRows);
    free(outputBuffer);
    return 0;
}
