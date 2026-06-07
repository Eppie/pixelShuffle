#include "fmath.hpp"
#include "pngReadWrite.h"
#include <atomic>
#include <cstring>
#include <functional>
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

using std::atomic;
using std::cerr;
using std::cout;
using std::endl;
using std::fixed;
using std::lock_guard;
using std::min;
using std::mt19937;
using std::mutex;
using std::numeric_limits;
using std::ostringstream;
using std::setfill;
using std::setprecision;
using std::setw;
using std::string;
using std::swap;
using std::uniform_int_distribution;
using std::vector;

struct Color {
  float L; // Can represent L*, or R
  float a; // Can represent a*, or G
  float b; // Can represent b*, or B
};

INLINE_OPTIONAL vector<int> commonDivisors(const int a, const int b) {
  vector<int> out;
  const int lim = min(a, b);
  for (int d = 1; d <= lim; ++d)
    if (a % d == 0 && b % d == 0)
      out.push_back(d);
  return out;
}

struct PatchGrid {
  int numPatchesX{};
  int numPatchesY{};
  vector<int> targetX_cuts;
  vector<int> targetY_cuts;
  vector<int> paletteX_cuts;
  vector<int> paletteY_cuts;
};

INLINE_OPTIONAL vector<int> calculateCuts(const int totalDimension,
                                          const int numDivisions) {
  vector<int> cuts(numDivisions + 1);
  const int baseSize = totalDimension / numDivisions;
  const int extraPixels = totalDimension % numDivisions;
  cuts[0] = 0;
  for (int i = 0; i < numDivisions; ++i) {
    cuts[i + 1] = cuts[i] + baseSize + (i < extraPixels ? 1 : 0);
  }
  return cuts;
}

INLINE_OPTIONAL PatchGrid
determineOptimalPatchGrid(const int sW, const int sH,   // palette
                          const int dW, const int dH,   // target
                          const int targetArea          // N × N
                          , const double maxAspect = 2.0,
                          const double aspectPenalty = 1'000.0)
{
    if (sW * sH != dW * dH)
        throw std::invalid_argument("Palette and target must have equal areas");

    const int maxArea = targetArea;               // <= N²
    struct Candidate {
        int rows, cols;
        int pW, pH;                               // palette patch W/H
        int tW, tH;                               // target  patch W/H
        int areaAvg;                              // mean of the two areas
        double aspSum;                            // (ar1-1)+(ar2-1)
    };

    Candidate best{};                             // will stay rows=0 if none
    double bestScore = -std::numeric_limits<double>::infinity();

    // rows = vertical splits, cols = horizontal splits
    for (int rows = 1; rows <= std::min(sH, dH); ++rows) {
        // Ceiling division, guarantees full coverage
        const int pH = (sH + rows - 1) / rows;
        const int tH = (dH + rows - 1) / rows;

        // If even a 1-pixel-wide patch would exceed N², skip these rows
        if (std::min(pH, tH) > maxArea) continue;

        for (int cols = 1; cols <= std::min(sW, dW); ++cols) {
            const int pW = (sW + cols - 1) / cols;
            const int tW = (dW + cols - 1) / cols;

            const int areaP = pW * pH;
            const int areaT = tW * tH;
            if (areaP > maxArea || areaT > maxArea) continue;

            const double arP = static_cast<double>(std::max(pW, pH)) /
                               std::min(pW, pH);
            const double arT = static_cast<double>(std::max(tW, tH)) /
                               std::min(tW, tH);
            if (arP > maxAspect || arT > maxAspect) continue;

            const int   areaAvg = (areaP + areaT) / 2;
            const double aspSum = (arP - 1.0) + (arT - 1.0);
            const double score  = areaAvg - aspectPenalty * aspSum;

            if (score > bestScore) {
                bestScore = score;
                best      = {rows, cols, pW, pH, tW, tH, areaAvg, aspSum};
            }
        }
    }

    if (best.rows == 0)
        throw std::runtime_error(
            "Could not find any admissible patch grid for the given images");

    PatchGrid grid;
    grid.numPatchesX   = best.cols;
    grid.numPatchesY   = best.rows;
    grid.targetX_cuts  = calculateCuts(dW, grid.numPatchesX);
    grid.targetY_cuts  = calculateCuts(dH, grid.numPatchesY);
    grid.paletteX_cuts = calculateCuts(sW, grid.numPatchesX);
    grid.paletteY_cuts = calculateCuts(sH, grid.numPatchesY);
    return grid;
}

INLINE_OPTIONAL Color neighborhoodAverage(const vector<Color> &buf,
                                          const int dW, const int dH,
                                          const int R, const int idx) {
  const int y = idx / dW;
  const int x = idx % dW;
  float sumL = 0, suma = 0, sumb = 0;
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
  float R = px[0] / 255.0f, G = px[1] / 255.0f, B = px[2] / 255.0f;
  R = R > 0.04045f ? pow24.get((R + 0.055f) / 1.055f) : R / 12.92f;
  G = G > 0.04045f ? pow24.get((G + 0.055f) / 1.055f) : G / 12.92f;
  B = B > 0.04045f ? pow24.get((B + 0.055f) / 1.055f) : B / 12.92f;
  R *= 100.0f; G *= 100.0f; B *= 100.0f;
  float X = (R * 0.4124f) + (G * 0.3576f) + (B * 0.1805f);
  float Y = (R * 0.2126f) + (G * 0.7152f) + (B * 0.0722f);
  float Z = (R * 0.0193f) + (G * 0.1192f) + (B * 0.9505f);
  static PowGenerator cbrt(1.0 / 3.0);
  X /= 95.047f; Y /= 100.0f; Z /= 108.883f;
  X = X > 0.008856f ? cbrt.get(X) : (X * 7.787f) + (16.0f / 116.0f);
  Y = Y > 0.008856f ? cbrt.get(Y) : (Y * 7.787f) + (16.0f / 116.0f);
  Z = Z > 0.008856f ? cbrt.get(Z) : (Z * 7.787f) + (16.0f / 116.0f);
  return Color{(116.0f * Y) - 16.0f, 500.0f * (X - Y), 200.0f * (Y - Z)};
}

INLINE_OPTIONAL Color RGBToRGB(png_bytep px) {
  return Color{px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
}

INLINE_OPTIONAL float deltaE2_Lab(const Color &c1, const Color &c2) {
  float dL = c1.L - c2.L;
  float da = c1.a - c2.a;
  float db = c1.b - c2.b;
  return dL * dL + da * da + db * db;
}

INLINE_OPTIONAL float deltaE2_RGB(const Color &c1, const Color &c2) {
  float dR = c1.L - c2.L;
  float dG = c1.a - c2.a;
  float dB = c1.b - c2.b;
  return dR * dR + dG * dG + dB * dB;
}

using ColorConverterFunc = std::function<Color(png_bytep)>;
using ColorDistanceFunc = std::function<float(const Color &, const Color &)>;

INLINE_OPTIONAL void lapjv(const vector<float> &cost, int n,
                           vector<int> &assign) {
  constexpr float INF = numeric_limits<float>::infinity();
  vector<float> u(n, 0.0f), v(n, 0.0f);
  vector<int> colOfRow(n, -1), rowOfCol(n, -1);
  vector<int> pred(n);
  vector<float> dist(n);
  vector<char> scanned(n);

  for (int j = 0; j < n; ++j) {
    float minVal = cost[j];
    int minRow = 0;
    for (int i = 1; i < n; ++i) {
      if (cost[i * n + j] < minVal) {
        minVal = cost[i * n + j];
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

  vector<int> freeRows;
  for (int i = 0; i < n; ++i)
    if (colOfRow[i] == -1) freeRows.push_back(i);

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
      for (int jj = 0; jj < n; ++jj) {
        if (scanned[jj]) {
          v[jj] += delta;
          u[rowOfCol[jj]] -= delta;
        } else {
          dist[jj] -= delta;
        }
      }
      for (int jj = 0; jj < n; ++jj) {
        if (scanned[jj]) continue;
        const float cur = cost[i * n + jj] - u[i] - v[jj];
        if (cur < dist[jj]) {
          dist[jj] = cur;
          pred[jj] = i;
        }
      }
    }
    while (jStar != -1) {
      const int i = pred[jStar];
      const int nextJ = colOfRow[i];
      rowOfCol[jStar] = i;
      colOfRow[i] = jStar;
      jStar = nextJ;
    }
  }
  assign.resize(n);
  for (int j = 0; j < n; ++j) assign[j] = rowOfCol[j];
}

double solveAndApplyPatch(
    const int px, const int py, const PatchGrid &patchGrid,
    const vector<Color> &paletteLab, const vector<Color> &targetLab,
    const png_bytep *paletteRows, const vector<png_bytep> &outRows,
    const int sW, const int dW, atomic<int> &patchCounter,
    const int totalPatches, mutex &ioMutex,
    const ColorDistanceFunc &colorDistance, const bool outputIntermediate,
    const std::function<void(int)> &dump_partial) {

  const int y0 = patchGrid.targetY_cuts[py], y1 = patchGrid.targetY_cuts[py + 1];
  const int tPatchH = y1 - y0;
  const int tx0 = patchGrid.targetX_cuts[px], tx1 = patchGrid.targetX_cuts[px + 1];
  const int tPatchW = tx1 - tx0;
  const int sy0 = patchGrid.paletteY_cuts[py], sy1 = patchGrid.paletteY_cuts[py + 1];
  const int pPatchH = sy1 - sy0;
  const int sx0 = patchGrid.paletteX_cuts[px], sx1 = patchGrid.paletteX_cuts[px + 1];
  const int pPatchW = sx1 - sx0;
  const int nPatch = tPatchW * tPatchH;

  vector<int> idx(nPatch), pIdx(nPatch);
  vector<float> cost(static_cast<size_t>(nPatch) * nPatch);

  for (int y = 0; y < tPatchH; ++y)
    for (int x = 0; x < tPatchW; ++x)
      idx[y * tPatchW + x] = (y0 + y) * dW + (tx0 + x);

  for (int y = 0; y < pPatchH; ++y)
    for (int x = 0; x < pPatchW; ++x)
      pIdx[y * pPatchW + x] = (sy0 + y) * sW + (sx0 + x);

  for (int i = 0; i < nPatch; ++i)
    for (int j = 0; j < nPatch; ++j)
      cost[static_cast<size_t>(i) * nPatch + j] =
          colorDistance(paletteLab[pIdx[i]], targetLab[idx[j]]);

  vector<int> assignment;
  lapjv(cost, nPatch, assignment);

  for (int t = 0; t < nPatch; ++t) {
    int src = pIdx[assignment[t]], tgt = idx[t];
    int srcY = src / sW, srcX = src % sW;
    int tgtY = tgt / dW, tgtX = tgt % dW;
    memcpy(&outRows[tgtY][tgtX * 4], &paletteRows[srcY][srcX * 4], 4);
  }

  double patchCost = 0.0;
  for (int t = 0; t < nPatch; ++t)
    patchCost += cost[static_cast<size_t>(assignment[t]) * nPatch + t];

  int curIndex = ++patchCounter;
  {
    lock_guard<mutex> lk(ioMutex);
    cout << "Patch " << curIndex << "/" << totalPatches
         << " done, patch ΔE² = " << fixed << setprecision(2) << patchCost
         << endl;
  }

  if (outputIntermediate) {
    dump_partial(curIndex);
  }

  return patchCost;
}

// ============================================================================
// CLI Parsing and Configuration
// ============================================================================

enum class ColorSpace { LAB, RGB };

struct ProgramOptions {
  string palettePath;
  string targetPath;
  string outputPath;

  bool ditheringEnabled = true;
  int numDitherVariants = 11;
  int patchSize = 128;
  ColorSpace colorSpace = ColorSpace::LAB;
  bool outputIntermediate = true;
};

void print_usage(const char *progName) {
  cerr << "Usage: " << progName
       << " [options] <palette.png> <target.png> <output.png>\n";
  cerr << "Options:\n"
       << "  --help                       Show this help message.\n"
       << "  --no-dither                  Disable the final dithering step.\n"
       << "  --dither-variants <N>        Generate N dithered images "
          "(default: 11).\n"
       << "  --patch-size <S>             Set target patch side length "
          "(default: 128).\n"
       << "  --color-space <space>        Use 'LAB' or 'RGB' for color "
          "difference (default: LAB).\n"
       << "  --no-intermediate            Do not save intermediate patch "
          "images.\n";
}

bool parse_arguments(int argc, char *argv[], ProgramOptions &opts) {
  vector<string> args(argv + 1, argv + argc);
  vector<string> positional_args;

  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--help") {
      print_usage(argv[0]);
      return false;
    } else if (args[i] == "--no-dither") {
      opts.ditheringEnabled = false;
    } else if (args[i] == "--no-intermediate") {
      opts.outputIntermediate = false;
    } else if (args[i] == "--dither-variants") {
      if (++i >= args.size()) {
        cerr << "Error: --dither-variants needs a value.\n";
        return false;
      }
      try {
        opts.numDitherVariants = std::stoi(args[i]);
      } catch (...) {
        cerr << "Error: Invalid number for --dither-variants.\n";
        return false;
      }
    } else if (args[i] == "--patch-size") {
      if (++i >= args.size()) {
        cerr << "Error: --patch-size needs a value.\n";
        return false;
      }
      try {
        opts.patchSize = std::stoi(args[i]);
      } catch (...) {
        cerr << "Error: Invalid number for --patch-size.\n";
        return false;
      }
    } else if (args[i] == "--color-space") {
      if (++i >= args.size()) {
        cerr << "Error: --color-space needs a value.\n";
        return false;
      }
      if (args[i] == "RGB")
        opts.colorSpace = ColorSpace::RGB;
      else if (args[i] == "LAB")
        opts.colorSpace = ColorSpace::LAB;
      else {
        cerr << "Error: unknown color space '" << args[i]
             << "'. Use 'LAB' or 'RGB'.\n";
        return false;
      }
    } else if (args[i][0] == '-') {
      cerr << "Error: unknown option '" << args[i] << "'.\n";
      return false;
    } else {
      positional_args.push_back(args[i]);
    }
  }

  if (positional_args.size() != 3) {
    print_usage(argv[0]);
    return false;
  }
  opts.palettePath = positional_args[0];
  opts.targetPath = positional_args[1];
  opts.outputPath = positional_args[2];
  return true;
}

// ============================================================================
// Main Execution
// ============================================================================

int main(int argc, char *argv[]) {
  ProgramOptions opts;
  if (!parse_arguments(argc, argv, opts)) {
    return 1;
  }

#ifdef _OPENMP
  omp_set_dynamic(0);
  omp_set_num_threads(omp_get_max_threads());
#endif

  int sW, sH, dW, dH;
  png_bytep *paletteRows = nullptr;
  png_bytep *targetRows = nullptr;

  readPNGFile(opts.palettePath.c_str(), &paletteRows, &sW, &sH);
  readPNGFile(opts.targetPath.c_str(), &targetRows, &dW, &dH);

  const int N = dW * dH;
  if (sW * sH != N) {
    cerr << "Error: Palette and target must have identical pixel counts!\n";
    return 1;
  }

  ColorConverterFunc colorConverter;
  ColorDistanceFunc colorDistance;
  if (opts.colorSpace == ColorSpace::LAB) {
    cout << "Using CIELAB color space for calculations.\n";
    colorConverter = RGBToLab;
    colorDistance = deltaE2_Lab;
  } else { // RGB
    cout << "Using sRGB color space for calculations.\n";
    colorConverter = RGBToRGB;
    colorDistance = deltaE2_RGB;
  }

  cout << "Building color arrays (" << N << " pixels)..." << endl;
  vector<Color> paletteLab(N), targetLab(N);
#pragma omp parallel for
  for (int y = 0; y < sH; ++y) {
    for (int x = 0; x < sW; ++x) {
      paletteLab[y * sW + x] = colorConverter(&paletteRows[y][x * 4]);
    }
  }
#pragma omp parallel for
  for (int y = 0; y < dH; ++y) {
    for (int x = 0; x < dW; ++x) {
      targetLab[y * dW + x] = colorConverter(&targetRows[y][x * 4]);
    }
  }

  cout << "Determining optimal patch layout..." << endl;
  const int targetArea = opts.patchSize * opts.patchSize;
  PatchGrid patchGrid = determineOptimalPatchGrid(sW, sH, dW, dH, targetArea);
  const int totalPatches = patchGrid.numPatchesX * patchGrid.numPatchesY;
  cout << "Patch grid: " << patchGrid.numPatchesX << " × "
       << patchGrid.numPatchesY << " (" << totalPatches << " patches total)…"
       << endl;

  const size_t stride = static_cast<size_t>(dW) * 4;
  auto outputBuffer = static_cast<png_bytep>(malloc(stride * dH));
  vector<png_bytep> outRows(dH);
  for (int y = 0; y < dH; ++y) {
    outRows[y] = outputBuffer + y * stride;
    memset(outRows[y], 0, stride);
  }

  string outPrefix;
  string outName = opts.outputPath;
  if (auto slash = outName.find_last_of('/'); slash != string::npos)
    outName = outName.substr(slash + 1);
  if (auto dot = outName.find_last_of('.'); dot != string::npos)
    outName = outName.substr(0, dot);
  outPrefix = "out" + outName + "_";

  auto dump_partial = [&](const int iter) {
    ostringstream oss;
    oss << outPrefix << setw(5) << setfill('0') << iter << ".png";
    writePNGFile(oss.str().c_str(), outRows.data(), dW, dH);
  };

  cout << "Solving patches (target " << opts.patchSize << "x"
       << opts.patchSize << ")..." << endl;

  atomic<int> patchCounter{0};
  mutex ioMutex;
  double globalCost = 0.0;

#pragma omp parallel for collapse(2) schedule(guided, 1) reduction(+ : globalCost)
  for (int py = 0; py < patchGrid.numPatchesY; ++py) {
    for (int px = 0; px < patchGrid.numPatchesX; ++px) {
      globalCost += solveAndApplyPatch(
          px, py, patchGrid, paletteLab, targetLab, paletteRows, outRows, sW,
          dW, patchCounter, totalPatches, ioMutex, colorDistance,
          opts.outputIntermediate, dump_partial);
    }
  }

  cout << "All patches complete. Final total ΔE² = " << fixed << setprecision(2)
       << globalCost << endl;
  writePNGFile(opts.outputPath.c_str(), outRows.data(), dW, dH);
  cout << "Final image written to " << opts.outputPath << endl;

  if (opts.ditheringEnabled) {
    const int maxItersPerVariant = min<int>(N * 10, 200000000);
    cout << "\nGenerating " << opts.numDitherVariants
         << " neighbourhood-aware dithered variants (window 5x5)..." << endl;

    vector<Color> baseLab(N);
#pragma omp parallel for
    for (int i = 0; i < N; ++i) {
        int y = i / dW;
        int x = i % dW;
        baseLab[i] = colorConverter(&outRows[y][x * 4]);
    }

    constexpr int R = 2;
    auto weightedErr = [&](const vector<Color> &buf, const int idx,
                           const float detail) -> float {
      const Color nAvg = neighborhoodAverage(buf, dW, dH, R, idx);
      const float neighErr = colorDistance(nAvg, targetLab[idx]);
      const float centErr = colorDistance(buf[idx], targetLab[idx]);
      return detail * centErr + (1.0f - detail) * neighErr;
    };

#pragma omp parallel for schedule(dynamic)
    for (int dv = 0; dv < opts.numDitherVariants; ++dv) {
      const float detail = (opts.numDitherVariants > 1)
                               ? static_cast<float>(dv) / (opts.numDitherVariants - 1)
                               : 1.0f;

      unsigned seed = 987654u ^ static_cast<unsigned>(detail * 1000.0f) ^
                      static_cast<unsigned>(omp_get_thread_num() * 1234);
      mt19937 rng(seed);
      uniform_int_distribution pick(0, N - 1);

      vector<Color> workLab = baseLab;
      vector<png_byte> scratchRGBA(static_cast<size_t>(dH) * stride);
      vector<png_bytep> scratchRows(dH);
      for (int y = 0; y < dH; ++y) {
        scratchRows[y] = scratchRGBA.data() + y * stride;
        memcpy(scratchRows[y], outRows[y], stride);
      }

      for (int it = 0; it < maxItersPerVariant; ++it) {
        int i = pick(rng);
        int j = pick(rng);
        if (i == j) continue;
        float before = weightedErr(workLab, i, detail) + weightedErr(workLab, j, detail);
        swap(workLab[i], workLab[j]);
        float after = weightedErr(workLab, i, detail) + weightedErr(workLab, j, detail);
        if (after < before) {
          int iy = i / dW, ix = i % dW, jy = j / dW, jx = j % dW;
          png_bytep p1 = &scratchRows[iy][ix * 4], p2 = &scratchRows[jy][jx * 4];
          for (int k = 0; k < 4; ++k) swap(p1[k], p2[k]);
        } else {
          swap(workLab[i], workLab[j]);
        }
      }

      string dithered_path;
      {
        lock_guard<mutex> lk(ioMutex); // Protects filename creation and cout
        ostringstream fname;
        string basePath = opts.outputPath;
        if (auto dot = basePath.find_last_of('.'); dot != string::npos)
            basePath = basePath.substr(0, dot);

        fname << basePath << "_dither_" << fixed << setprecision(3) << detail
              << ".png";
        dithered_path = fname.str();
        writePNGFile(dithered_path.c_str(), scratchRows.data(), dW, dH);
        cout << "  • detail " << fixed << setprecision(3) << detail
             << " done. Written to " << dithered_path << endl;
      }
    }
  }

  for (int y = 0; y < sH; ++y) free(paletteRows[y]);
  free(paletteRows);
  for (int y = 0; y < dH; ++y) free(targetRows[y]);
  free(targetRows);
  free(outputBuffer);

  return 0;
}