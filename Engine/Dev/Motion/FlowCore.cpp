/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#include "FlowCore.h"
#include "FlowGpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace flow {

// ------------------------------------------------------------------ threads

int
threadCount()
{
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) {
        n = 4;
    }
    return (int)std::min(n, 32u);
}

void
parallelFor(int n, const std::function<void(int, int)>& f)
{
    if (n <= 0) {
        return;
    }
    const int nt = std::min(threadCount(), std::max(1, n / 8));
    if (nt <= 1) {
        f(0, n);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (int t = 0; t < nt; ++t) {
        const int b = (int)((long long)n * t / nt);
        const int e = (int)((long long)n * (t + 1) / nt);
        if (b < e) {
            pool.emplace_back([&f, b, e]() { f(b, e); });
        }
    }
    for (size_t i = 0; i < pool.size(); ++i) {
        pool[i].join();
    }
}

// ------------------------------------------------------------------ helpers

namespace {

std::mutex gGpuErrLock;
std::string gGpuErr;

void
setGpuError(const std::string& e)
{
    std::lock_guard<std::mutex> l(gGpuErrLock);
    if ( !e.empty() && e != gGpuErr ) {
        std::fprintf(stderr, "[Motion] GPU not used, CPU instead: %s\n", e.c_str());
    }
    gGpuErr = e;
}

struct Plane
{
    int w = 0, h = 0;
    std::vector<float> d;
    Plane() {}
    Plane(int w_, int h_, float v = 0.f) : w(w_), h(h_), d((size_t)w_ * h_, v) {}
    float& at(int x, int y) { return d[(size_t)y * w + x]; }
    float at(int x, int y) const { return d[(size_t)y * w + x]; }
    float clampAt(int x, int y) const
    {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return d[(size_t)y * w + x];
    }
    float bilinear(float x, float y) const
    {
        const float fx = std::floor(x), fy = std::floor(y);
        const int x0 = (int)fx, y0 = (int)fy;
        const float tx = x - fx, ty = y - fy;
        const float a = clampAt(x0, y0), b = clampAt(x0 + 1, y0);
        const float c = clampAt(x0, y0 + 1), e = clampAt(x0 + 1, y0 + 1);
        return (a * (1.f - tx) + b * tx) * (1.f - ty) + (c * (1.f - tx) + e * tx) * ty;
    }
};

inline bool
aborted(const Params& p)
{
    return p.abort && p.abort();
}

// Separable gaussian blur (clamped edges).
void
gaussian(Plane& img, float sigma)
{
    if (sigma <= 0.05f || img.w == 0) {
        return;
    }
    const int r = std::max(1, (int)std::ceil(sigma * 3.f));
    std::vector<float> k(2 * r + 1);
    float s = 0.f;
    for (int i = -r; i <= r; ++i) {
        k[i + r] = std::exp(-0.5f * (float)(i * i) / (sigma * sigma));
        s += k[i + r];
    }
    for (size_t i = 0; i < k.size(); ++i) {
        k[i] /= s;
    }
    Plane tmp(img.w, img.h);
    parallelFor(img.h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < img.w; ++x) {
                float acc = 0.f;
                for (int i = -r; i <= r; ++i) {
                    acc += k[i + r] * img.clampAt(x + i, y);
                }
                tmp.at(x, y) = acc;
            }
        }
    });
    parallelFor(img.h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < img.w; ++x) {
                float acc = 0.f;
                for (int i = -r; i <= r; ++i) {
                    acc += k[i + r] * tmp.clampAt(x, y + i);
                }
                img.at(x, y) = acc;
            }
        }
    });
}

// Area resample (box filter when shrinking, bilinear when enlarging).
Plane
resample(const Plane& src, int w, int h)
{
    Plane dst(w, h);
    const float sx = (float)src.w / (float)w, sy = (float)src.h / (float)h;
    if (sx <= 1.f && sy <= 1.f) {
        parallelFor(h, [&](int b, int e) {
            for (int y = b; y < e; ++y) {
                for (int x = 0; x < w; ++x) {
                    dst.at(x, y) = src.bilinear((x + 0.5f) * sx - 0.5f, (y + 0.5f) * sy - 0.5f);
                }
            }
        });
        return dst;
    }
    parallelFor(h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            const float y0 = y * sy, y1 = (y + 1) * sy;
            for (int x = 0; x < w; ++x) {
                const float x0 = x * sx, x1 = (x + 1) * sx;
                float acc = 0.f, wsum = 0.f;
                for (int yy = (int)y0; yy < (int)std::ceil(y1) && yy < src.h; ++yy) {
                    const float wy = std::min(y1, (float)yy + 1.f) - std::max(y0, (float)yy);
                    if (wy <= 0.f) {
                        continue;
                    }
                    for (int xx = (int)x0; xx < (int)std::ceil(x1) && xx < src.w; ++xx) {
                        const float wx = std::min(x1, (float)xx + 1.f) - std::max(x0, (float)xx);
                        if (wx <= 0.f) {
                            continue;
                        }
                        acc += wx * wy * src.at(xx, yy);
                        wsum += wx * wy;
                    }
                }
                dst.at(x, y) = wsum > 0.f ? acc / wsum : 0.f;
            }
        }
    });
    return dst;
}

// Luminance, normalised for the flow (robust scale, compressed highlights).
Plane
toGray(const float* img, int w, int h, int nc, float scale)
{
    Plane g(w, h);
    parallelFor(h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < w; ++x) {
                const float* p = img + ((size_t)y * w + x) * nc;
                float l = nc >= 3 ? 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] : p[0];
                if (!(l == l)) {
                    l = 0.f;   // NaN
                }
                l = std::max(0.f, l * scale);
                g.at(x, y) = l / (1.f + l);   // [0, 1), HDR highlights compressed
            }
        }
    });
    return g;
}

float
robustScale(const float* A, const float* B, int w, int h, int nc)
{
    // 1 / (2 x mean luminance), from a sparse sample of both images.
    double s = 0.;
    long n = 0;
    const int step = std::max(1, (int)std::sqrt((double)w * h / 20000.));
    for (int y = 0; y < h; y += step) {
        for (int x = 0; x < w; x += step) {
            for (int k = 0; k < 2; ++k) {
                const float* p = (k ? B : A) + ((size_t)y * w + x) * nc;
                const float l = nc >= 3 ? 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] : p[0];
                if (l == l && l > 0.f && l < 1e6f) {
                    s += l;
                }
                ++n;
            }
        }
    }
    const double mean = n ? s / n : 0.;
    return mean > 1e-6 ? (float)(0.5 / mean) : 1.f;
}

// Median of the 3x3 or 5x5 neighbourhood (flow component).
void
medianFilter(Plane& f, int radius)
{
    if (radius <= 0) {
        return;
    }
    Plane src = f;
    const int n = (2 * radius + 1) * (2 * radius + 1);
    parallelFor(f.h, [&](int b, int e) {
        std::vector<float> buf(n);
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < f.w; ++x) {
                int k = 0;
                for (int j = -radius; j <= radius; ++j) {
                    for (int i = -radius; i <= radius; ++i) {
                        buf[k++] = src.clampAt(x + i, y + j);
                    }
                }
                std::nth_element(buf.begin(), buf.begin() + n / 2, buf.end());
                f.at(x, y) = buf[n / 2];
            }
        }
    });
}

// One pyramid level: refine (u, v) from I0 to I1 (same size).
bool
refineLevel(const Plane& I0, const Plane& I1, Plane& u, Plane& v,
            float alpha, int warps, int iters, int medianRadius, const Params& p)
{
    const int w = I0.w, h = I0.h;
    const float epsD = 1e-3f, epsS = 1e-2f;
    const float omega = 1.8f;
    Plane Ix(w, h), Iy(w, h), It(w, h), du(w, h), dv(w, h), psiD(w, h), psiS(w, h);
    std::vector<unsigned char> inside((size_t)w * h);

    for (int warp = 0; warp < warps; ++warp) {
        if (aborted(p)) {
            return false;
        }
        // Warped I1 and its gradients (central differences on the warped image,
        // averaged with I0's: more stable on large motions).
        parallelFor(h, [&](int b, int e) {
            for (int y = b; y < e; ++y) {
                for (int x = 0; x < w; ++x) {
                    const float xw = x + u.at(x, y), yw = y + v.at(x, y);
                    const bool in = xw >= 0.f && yw >= 0.f && xw <= (float)(w - 1) && yw <= (float)(h - 1);
                    inside[(size_t)y * w + x] = in ? 1 : 0;
                    const float i1 = I1.bilinear(xw, yw);
                    const float gx1 = 0.5f * (I1.bilinear(xw + 1.f, yw) - I1.bilinear(xw - 1.f, yw));
                    const float gy1 = 0.5f * (I1.bilinear(xw, yw + 1.f) - I1.bilinear(xw, yw - 1.f));
                    const float gx0 = 0.5f * (I0.clampAt(x + 1, y) - I0.clampAt(x - 1, y));
                    const float gy0 = 0.5f * (I0.clampAt(x, y + 1) - I0.clampAt(x, y - 1));
                    Ix.at(x, y) = 0.5f * (gx0 + gx1);
                    Iy.at(x, y) = 0.5f * (gy0 + gy1);
                    It.at(x, y) = in ? i1 - I0.at(x, y) : 0.f;
                }
            }
        });
        std::fill(du.d.begin(), du.d.end(), 0.f);
        std::fill(dv.d.begin(), dv.d.end(), 0.f);

        for (int it = 0; it < iters; ++it) {
            if ((it & 3) == 0) {
                if (aborted(p)) {
                    return false;
                }
                // Lagged robust weights (Charbonnier).
                parallelFor(h, [&](int b, int e) {
                    for (int y = b; y < e; ++y) {
                        for (int x = 0; x < w; ++x) {
                            const size_t i = (size_t)y * w + x;
                            const float r = It.d[i] + Ix.d[i] * du.d[i] + Iy.d[i] * dv.d[i];
                            psiD.d[i] = inside[i] ? 0.5f / std::sqrt(r * r + epsD * epsD) : 0.f;
                            const float ux = 0.5f * ((u.clampAt(x + 1, y) + du.clampAt(x + 1, y)) - (u.clampAt(x - 1, y) + du.clampAt(x - 1, y)));
                            const float uy = 0.5f * ((u.clampAt(x, y + 1) + du.clampAt(x, y + 1)) - (u.clampAt(x, y - 1) + du.clampAt(x, y - 1)));
                            const float vx = 0.5f * ((v.clampAt(x + 1, y) + dv.clampAt(x + 1, y)) - (v.clampAt(x - 1, y) + dv.clampAt(x - 1, y)));
                            const float vy = 0.5f * ((v.clampAt(x, y + 1) + dv.clampAt(x, y + 1)) - (v.clampAt(x, y - 1) + dv.clampAt(x, y - 1)));
                            psiS.d[i] = 0.5f / std::sqrt(ux * ux + uy * uy + vx * vx + vy * vy + epsS * epsS);
                        }
                    }
                });
            }
            // Red-black SOR.
            for (int color = 0; color < 2; ++color) {
                parallelFor(h, [&](int b, int e) {
                    for (int y = b; y < e; ++y) {
                        for (int x = (y + color) & 1; x < w; x += 2) {
                            const size_t i = (size_t)y * w + x;
                            const float ps = psiS.d[i];
                            float wsum = 0.f, su = 0.f, sv = 0.f;
                            const int nx[4] = { x - 1, x + 1, x, x };
                            const int ny[4] = { y, y, y - 1, y + 1 };
                            for (int k = 0; k < 4; ++k) {
                                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) {
                                    continue;
                                }
                                const size_t j = (size_t)ny[k] * w + nx[k];
                                const float wn = alpha * 0.5f * (ps + psiS.d[j]);
                                wsum += wn;
                                su += wn * (u.d[j] + du.d[j] - u.d[i]);
                                sv += wn * (v.d[j] + dv.d[j] - v.d[i]);
                            }
                            const float pd = psiD.d[i];
                            const float ix = Ix.d[i], iy = Iy.d[i], itv = It.d[i];
                            const float denU = pd * ix * ix + wsum;
                            const float denV = pd * iy * iy + wsum;
                            if (denU > 1e-12f) {
                                const float nu = (su - pd * (ix * itv + ix * iy * dv.d[i])) / denU;
                                du.d[i] = (1.f - omega) * du.d[i] + omega * nu;
                            }
                            if (denV > 1e-12f) {
                                const float nv = (sv - pd * (iy * itv + ix * iy * du.d[i])) / denV;
                                dv.d[i] = (1.f - omega) * dv.d[i] + omega * nv;
                            }
                        }
                    }
                });
            }
        }
        for (size_t i = 0; i < u.d.size(); ++i) {
            u.d[i] += du.d[i];
            v.d[i] += dv.d[i];
        }
        medianFilter(u, medianRadius);
        medianFilter(v, medianRadius);
    }
    return true;
}

// Patch matching (large displacements): for patches on a grid, search the
// best integer displacement within +-radius around the current flow (SSD on
// mean-removed patches), then keep, per pixel, the candidate (current flow or
// the matched one, interpolated between patch centres) with the lower local
// cost. Coarse-to-fine alone loses small or fast objects whose texture
// vanishes in the coarse levels; this recovers them before the variational
// refinement.
void
matchLevel(const Plane& I0, const Plane& I1, Plane& u, Plane& v, int radius, const Params& p)
{
    const int w = I0.w, h = I0.h;
    const int ps = 12, half = ps / 2, stride = 4;
    const int gw = (w + stride - 1) / stride, gh = (h + stride - 1) / stride;
    if (gw < 2 || gh < 2) {
        return;
    }
    std::vector<float> mu((size_t)gw * gh), mv((size_t)gw * gh);
    auto patchCost = [&](int cx, int cy, float dx, float dy) {
        // mean-removed SSD between the patch of I0 at (cx, cy) and I1 at +(dx, dy)
        float m0 = 0.f, m1 = 0.f;
        float a[ps * ps], b[ps * ps];
        int n = 0;
        for (int j = -half; j < half; ++j) {
            for (int i = -half; i < half; ++i) {
                a[n] = I0.clampAt(cx + i, cy + j);
                b[n] = I1.bilinear(cx + i + dx, cy + j + dy);
                m0 += a[n];
                m1 += b[n];
                ++n;
            }
        }
        m0 /= n;
        m1 /= n;
        float c = 0.f;
        for (int k = 0; k < n; ++k) {
            const float d = (a[k] - m0) - (b[k] - m1);
            c += d * d;
        }
        // keep a little of the absolute difference (flat patches)
        return c + 0.25f * n * (m0 - m1) * (m0 - m1);
    };
    parallelFor(gh, [&](int b0, int e0) {
        for (int gy = b0; gy < e0; ++gy) {
            for (int gx = 0; gx < gw; ++gx) {
                const int cx = std::min(w - 1, gx * stride + stride / 2);
                const int cy = std::min(h - 1, gy * stride + stride / 2);
                const float u0 = u.at(cx, cy), v0 = v.at(cx, cy);
                float best = patchCost(cx, cy, u0, v0), bu = u0, bv = v0;
                const float curCost = best;
                for (int dy = -radius; dy <= radius; ++dy) {
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (dx == 0 && dy == 0) {
                            continue;
                        }
                        // prefer small corrections: repetitive textures match at
                        // several offsets, the nearest one wins unless clearly worse
                        const float c = patchCost(cx, cy, std::round(u0) + dx, std::round(v0) + dy) *
                                        (1.f + 0.08f * (float)(dx * dx + dy * dy));
                        if (c < best) {
                            best = c;
                            bu = std::round(u0) + dx;
                            bv = std::round(v0) + dy;
                        }
                    }
                }
                // only clearly better matches replace the current flow
                if (best > 0.5f * curCost) {
                    bu = u0;
                    bv = v0;
                }
                mu[(size_t)gy * gw + gx] = bu;
                mv[(size_t)gy * gw + gx] = bv;
            }
        }
    });
    if (aborted(p)) {
        return;
    }
    // Median of the 3x3 patch neighbourhood (removes isolated false matches).
    {
        std::vector<float> fu = mu, fv = mv;
        for (int gy = 0; gy < gh; ++gy) {
            for (int gx = 0; gx < gw; ++gx) {
                float bu[9], bv[9];
                int n = 0;
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i) {
                        const int x = std::max(0, std::min(gw - 1, gx + i));
                        const int y = std::max(0, std::min(gh - 1, gy + j));
                        bu[n] = fu[(size_t)y * gw + x];
                        bv[n] = fv[(size_t)y * gw + x];
                        ++n;
                    }
                }
                std::nth_element(bu, bu + 4, bu + 9);
                std::nth_element(bv, bv + 4, bv + 9);
                mu[(size_t)gy * gw + gx] = bu[4];
                mv[(size_t)gy * gw + gx] = bv[4];
            }
        }
    }
    // Per pixel: matched candidate (nearest patch) vs current flow, 3x3 SAD.
    Plane nu = u, nv = v;
    parallelFor(h, [&](int b0, int e0) {
        for (int y = b0; y < e0; ++y) {
            for (int x = 0; x < w; ++x) {
                const int gx = std::min(gw - 1, x / stride), gy = std::min(gh - 1, y / stride);
                const float cu = mu[(size_t)gy * gw + gx], cv = mv[(size_t)gy * gw + gx];
                const float ou = u.at(x, y), ov = v.at(x, y);
                if (cu == ou && cv == ov) {
                    continue;
                }
                float c0 = 0.f, c1 = 0.f;
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i) {
                        const float i0 = I0.clampAt(x + i, y + j);
                        c0 += std::fabs(i0 - I1.bilinear(x + i + ou, y + j + ov));
                        c1 += std::fabs(i0 - I1.bilinear(x + i + cu, y + j + cv));
                    }
                }
                if (c1 < c0) {
                    nu.at(x, y) = cu;
                    nv.at(x, y) = cv;
                }
            }
        }
    });
    u = std::move(nu);
    v = std::move(nv);
}

} // anonymous namespace

void
Field::sample(float x, float y, float* du, float* dv) const
{
    const float fx = std::floor(x), fy = std::floor(y);
    int x0 = (int)fx, y0 = (int)fy;
    const float tx = x - fx, ty = y - fy;
    auto cx = [this](int a) { return a < 0 ? 0 : (a >= w ? w - 1 : a); };
    auto cy = [this](int a) { return a < 0 ? 0 : (a >= h ? h - 1 : a); };
    const int xa = cx(x0), xb = cx(x0 + 1), ya = cy(y0), yb = cy(y0 + 1);
    const size_t i00 = (size_t)ya * w + xa, i10 = (size_t)ya * w + xb;
    const size_t i01 = (size_t)yb * w + xa, i11 = (size_t)yb * w + xb;
    *du = (u[i00] * (1.f - tx) + u[i10] * tx) * (1.f - ty) + (u[i01] * (1.f - tx) + u[i11] * tx) * ty;
    *dv = (v[i00] * (1.f - tx) + v[i10] * tx) * (1.f - ty) + (v[i01] * (1.f - tx) + v[i11] * tx) * ty;
}

bool
computeFlow(const float* A, const float* B, int w, int h, int nComp, const Params& p, Field* out)
{
    if (!A || !B || w <= 0 || h <= 0 || nComp <= 0 || !out) {
        return false;
    }
    // Working resolution.
    double detail = std::max(0.01, std::min(1.0, p.detail));
    int ww = std::max(8, (int)std::lround(w * detail));
    int wh = std::max(8, (int)std::lround(h * detail));
    const int longest = std::max(ww, wh);
    if (longest > p.maxWorkingSize) {
        const double s = (double)p.maxWorkingSize / longest;
        ww = std::max(8, (int)(ww * s));
        wh = std::max(8, (int)(wh * s));
    }
    ww = std::min(ww, w);
    wh = std::min(wh, h);

    const float sc = robustScale(A, B, w, h, nComp);
    Plane g0 = toGray(A, w, h, nComp, sc), g1 = toGray(B, w, h, nComp, sc);
    if (ww != w || wh != h) {
        g0 = resample(g0, ww, wh);
        g1 = resample(g1, ww, wh);
    }
    gaussian(g0, 0.7f);
    gaussian(g1, 0.7f);

    // Pyramid (factor 0.5 down to ~16 px).
    std::vector<Plane> P0(1, g0), P1(1, g1);
    while (std::min(P0.back().w, P0.back().h) > 40 && P0.size() < 12) {
        const int nw = std::max(8, (P0.back().w + 1) / 2), nh = std::max(8, (P0.back().h + 1) / 2);
        Plane a = P0.back(), b = P1.back();
        gaussian(a, 1.0f);   // anti-aliasing before halving
        gaussian(b, 1.0f);
        P0.push_back(resample(a, nw, nh));
        P1.push_back(resample(b, nw, nh));
    }

    // Smoothness: Nuke's "Smoothness" 0..1 (OFlow), more for "Regularized".
    const double sm = std::max(0.0, std::min(4.0, p.smoothness));
    const float alpha = (float)((p.local ? 0.004 : 0.012) * (0.25 + 1.5 * sm));
    const int medianRadius = p.local ? 1 : 2;

    Plane u(P0.back().w, P0.back().h), v(P0.back().w, P0.back().h);
    for (int lvl = (int)P0.size() - 1; lvl >= 0; --lvl) {
        const Plane& I0 = P0[lvl];
        const Plane& I1 = P1[lvl];
        if (u.w != I0.w || u.h != I0.h) {
            const float sx = (float)I0.w / u.w, sy = (float)I0.h / u.h;
            Plane nu = resample(u, I0.w, I0.h), nv = resample(v, I0.w, I0.h);
            for (size_t i = 0; i < nu.d.size(); ++i) {
                nu.d[i] *= sx;
                nv.d[i] *= sy;
            }
            u = std::move(nu);
            v = std::move(nv);
        }
        const bool coarse = lvl > 0;
        if (lvl >= 2 || lvl == (int)P0.size() - 1) {   // fine levels: variational only (repetitive textures)
            matchLevel(I0, I1, u, v, lvl == (int)P0.size() - 1 ? 6 : 3, p);
        }
        if (aborted(p)) {
            return false;
        }
        const int warps = coarse ? 3 : 4, iters = coarse ? 20 : 30;
        bool done = false;
        if (p.useGpu && (size_t)I0.w * I0.h >= 96 * 96) {   // tiny levels: the CPU is quicker
            std::string err;
            done = gpu::refineLevel(I0.d, I1.d, I0.w, I0.h, u.d, v.d, alpha, warps, iters, medianRadius, p.abort, &err);
            if (!done && aborted(p)) {
                return false;
            }
            setGpuError(done ? std::string() : err);
        }
        if ( !done && !refineLevel(I0, I1, u, v, alpha, warps, iters, medianRadius, p) ) {
            return false;
        }
    }

    // Back to the image resolution.
    out->w = w;
    out->h = h;
    if (u.w == w && u.h == h) {
        out->u = std::move(u.d);
        out->v = std::move(v.d);
    } else {
        const float sx = (float)w / u.w, sy = (float)h / u.h;
        Plane fu = resample(u, w, h), fv = resample(v, w, h);
        for (size_t i = 0; i < fu.d.size(); ++i) {
            fu.d[i] *= sx;
            fv.d[i] *= sy;
        }
        out->u = std::move(fu.d);
        out->v = std::move(fv.d);
    }
    return !aborted(p);
}

// ------------------------------------------------------------------ sampling

namespace {

inline float
cubicW(float t)
{
    // Keys, a = -0.5
    t = std::fabs(t);
    if (t < 1.f) {
        return (1.5f * t - 2.5f) * t * t + 1.f;
    }
    if (t < 2.f) {
        return ((-0.5f * t + 2.5f) * t - 4.f) * t + 2.f;
    }
    return 0.f;
}

} // anonymous namespace

void
sampleImage(const float* img, int w, int h, int nc, float x, float y, FilterEnum filter, float* out)
{
    auto px = [&](int xx, int yy) -> const float* {
        xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
        yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
        return img + ((size_t)yy * w + xx) * nc;
    };
    if (filter == eFilterNearest) {
        const float* p = px((int)std::lround(x), (int)std::lround(y));
        for (int c = 0; c < nc; ++c) {
            out[c] = p[c];
        }
        return;
    }
    const float fx = std::floor(x), fy = std::floor(y);
    const int x0 = (int)fx, y0 = (int)fy;
    const float tx = x - fx, ty = y - fy;
    if (filter == eFilterBilinear) {
        const float* a = px(x0, y0);
        const float* b = px(x0 + 1, y0);
        const float* c = px(x0, y0 + 1);
        const float* d = px(x0 + 1, y0 + 1);
        for (int k = 0; k < nc; ++k) {
            out[k] = (a[k] * (1.f - tx) + b[k] * tx) * (1.f - ty) + (c[k] * (1.f - tx) + d[k] * tx) * ty;
        }
        return;
    }
    float wx[4], wy[4];
    for (int i = 0; i < 4; ++i) {
        wx[i] = cubicW(tx - (float)(i - 1));
        wy[i] = cubicW(ty - (float)(i - 1));
    }
    float acc[16] = { 0.f };
    const int n = std::min(nc, 16);
    for (int j = 0; j < 4; ++j) {
        for (int i = 0; i < 4; ++i) {
            const float wgt = wx[i] * wy[j];
            const float* p = px(x0 + i - 1, y0 + j - 1);
            for (int k = 0; k < n; ++k) {
                acc[k] += wgt * p[k];
            }
        }
    }
    for (int k = 0; k < n; ++k) {
        out[k] = acc[k];
    }
}

void
warpByField(const float* img, int w, int h, int nc, const Field& F, float scale, FilterEnum filter, float* out)
{
    parallelFor(h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                sampleImage(img, w, h, nc, x + scale * F.u[i], y + scale * F.v[i], filter, out + i * nc);
            }
        }
    });
}

void
projectFlow(const Field& fAB, const Field& fBA, float a, Field* out)
{
    const int w = fAB.w, h = fAB.h;
    out->w = w;
    out->h = h;
    out->u.assign((size_t)w * h, 0.f);
    out->v.assign((size_t)w * h, 0.f);
    std::vector<float> wsum((size_t)w * h, 0.f);

    // Splat with bilinear weights: from A (A->B vectors, moved by a) and from
    // B (-(B->A) vectors, moved by (1 - a)). Sequential (concurrent writes).
    auto splat = [&](float x, float y, float du, float dv, float weight) {
        const float fx = std::floor(x), fy = std::floor(y);
        const int x0 = (int)fx, y0 = (int)fy;
        const float tx = x - fx, ty = y - fy;
        const float ws[4] = { (1.f - tx) * (1.f - ty), tx * (1.f - ty), (1.f - tx) * ty, tx * ty };
        const int xs[4] = { x0, x0 + 1, x0, x0 + 1 };
        const int ys[4] = { y0, y0, y0 + 1, y0 + 1 };
        for (int k = 0; k < 4; ++k) {
            if (xs[k] < 0 || ys[k] < 0 || xs[k] >= w || ys[k] >= h) {
                continue;
            }
            const size_t i = (size_t)ys[k] * w + xs[k];
            const float wk = ws[k] * weight;
            out->u[i] += wk * du;
            out->v[i] += wk * dv;
            wsum[i] += wk;
        }
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            if (fAB.valid()) {
                // consistency: A->B then back with B->A should return to x
                float bu, bv;
                fBA.valid() ? fBA.sample(x + fAB.u[i], y + fAB.v[i], &bu, &bv) : (void)(bu = -fAB.u[i], bv = -fAB.v[i]);
                const float err = (fAB.u[i] + bu) * (fAB.u[i] + bu) + (fAB.v[i] + bv) * (fAB.v[i] + bv);
                splat(x + a * fAB.u[i], y + a * fAB.v[i], fAB.u[i], fAB.v[i], (1.f - a + 1e-3f) / (1.f + err));
            }
            if (fBA.valid()) {
                float au, av;
                fAB.valid() ? fAB.sample(x + fBA.u[i], y + fBA.v[i], &au, &av) : (void)(au = -fBA.u[i], av = -fBA.v[i]);
                const float err = (fBA.u[i] + au) * (fBA.u[i] + au) + (fBA.v[i] + av) * (fBA.v[i] + av);
                splat(x + (1.f - a) * fBA.u[i], y + (1.f - a) * fBA.v[i], -fBA.u[i], -fBA.v[i], (a + 1e-3f) / (1.f + err));
            }
        }
    }
    // Normalise; fill holes by repeated averaging of the filled neighbours.
    std::vector<unsigned char> filled((size_t)w * h, 0);
    size_t holes = 0;
    for (size_t i = 0; i < wsum.size(); ++i) {
        if (wsum[i] > 1e-4f) {
            out->u[i] /= wsum[i];
            out->v[i] /= wsum[i];
            filled[i] = 1;
        } else {
            ++holes;
        }
    }
    for (int pass = 0; holes > 0 && pass < 256; ++pass) {
        std::vector<unsigned char> next = filled;
        size_t fixedNow = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                if (filled[i]) {
                    continue;
                }
                float su = 0.f, sv = 0.f;
                int n = 0;
                for (int j = -1; j <= 1; ++j) {
                    for (int k = -1; k <= 1; ++k) {
                        const int xx = x + k, yy = y + j;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h) {
                            continue;
                        }
                        const size_t q = (size_t)yy * w + xx;
                        if (filled[q]) {
                            su += out->u[q];
                            sv += out->v[q];
                            ++n;
                        }
                    }
                }
                if (n) {
                    out->u[i] = su / n;
                    out->v[i] = sv / n;
                    next[i] = 1;
                    ++fixedNow;
                }
            }
        }
        filled.swap(next);
        if (fixedNow == 0) {
            break;
        }
        holes -= std::min(holes, fixedNow);
    }
}

void
interpolate(const float* A, const float* B, int w, int h, int nc,
            const Field& fAB, const Field& fBA, float a, FilterEnum filter, float* out)
{
    a = std::max(0.f, std::min(1.f, a));
    Field Vt;
    projectFlow(fAB, fBA, a, &Vt);
    parallelFor(h, [&](int b, int e) {
        std::vector<float> ca(nc), cb(nc);
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                const float vu = Vt.u[i], vv = Vt.v[i];
                const float ax = x - a * vu, ay = y - a * vv;           // position in A
                const float bx = x + (1.f - a) * vu, by = y + (1.f - a) * vv; // position in B
                // visibility from the flows' agreement at the source points
                float fu, fv, gu, gv;
                fAB.sample(ax, ay, &fu, &fv);
                fBA.sample(bx, by, &gu, &gv);
                const float eA = (fu - vu) * (fu - vu) + (fv - vv) * (fv - vv);
                const float eB = (gu + vu) * (gu + vu) + (gv + vv) * (gv + vv);
                float wa = (1.f - a) / (1.f + eA * eA);
                float wb = a / (1.f + eB * eB);
                const bool outA = ax < -0.5f || ay < -0.5f || ax > w - 0.5f || ay > h - 0.5f;
                const bool outB = bx < -0.5f || by < -0.5f || bx > w - 0.5f || by > h - 0.5f;
                if (outA && !outB) {
                    wa *= 0.02f;
                }
                if (outB && !outA) {
                    wb *= 0.02f;
                }
                float ws = wa + wb;
                if (ws < 1e-6f) {
                    wa = 1.f - a;
                    wb = a;
                    ws = 1.f;
                }
                sampleImage(A, w, h, nc, ax, ay, filter, ca.data());
                sampleImage(B, w, h, nc, bx, by, filter, cb.data());
                float* o = out + i * nc;
                for (int c = 0; c < nc; ++c) {
                    o[c] = (wa * ca[c] + wb * cb[c]) / ws;
                }
            }
        }
    });
}

// ------------------------------------------------------------------ robust vectors

void
cleanFlow(const Field& f, const Field* back, Field* out, std::vector<float>* confidence)
{
    *out = f;
    if ( !f.valid() ) {
        return;
    }
    const int w = f.w, h = f.h;
    const size_t n = (size_t)w * h;
    const float s = std::max(1.f, (float)std::max(w, h) / 1920.f);   // resolution scale (1 = HD)
    const bool haveBack = back && back->valid() && back->w == w && back->h == h;

    // Neighbourhood medians on blocks (subsampled), then 3x3 median of blocks.
    const int Bm = std::max(16, (int)std::lround(32.f * s));
    const int mgw = (w + Bm - 1) / Bm, mgh = (h + Bm - 1) / Bm;
    std::vector<float> mu((size_t)mgw * mgh), mv((size_t)mgw * mgh);
    parallelFor(mgh, [&](int b, int e) {
        std::vector<float> bu, bv;
        const int st = std::max(1, Bm / 8);
        for (int gy = b; gy < e; ++gy) {
            for (int gx = 0; gx < mgw; ++gx) {
                bu.clear();
                bv.clear();
                for (int y = gy * Bm; y < std::min(h, (gy + 1) * Bm); y += st) {
                    for (int x = gx * Bm; x < std::min(w, (gx + 1) * Bm); x += st) {
                        bu.push_back(f.uAt(x, y));
                        bv.push_back(f.vAt(x, y));
                    }
                }
                if ( bu.empty() ) {
                    continue;
                }
                std::nth_element(bu.begin(), bu.begin() + bu.size() / 2, bu.end());
                std::nth_element(bv.begin(), bv.begin() + bv.size() / 2, bv.end());
                mu[(size_t)gy * mgw + gx] = bu[bu.size() / 2];
                mv[(size_t)gy * mgw + gx] = bv[bv.size() / 2];
            }
        }
    });
    {
        std::vector<float> su = mu, sv = mv;
        for (int gy = 0; gy < mgh; ++gy) {
            for (int gx = 0; gx < mgw; ++gx) {
                float a[9], c[9];
                int k = 0;
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i) {
                        const int xx = std::max(0, std::min(mgw - 1, gx + i)), yy = std::max(0, std::min(mgh - 1, gy + j));
                        a[k] = su[(size_t)yy * mgw + xx];
                        c[k] = sv[(size_t)yy * mgw + xx];
                        ++k;
                    }
                }
                std::nth_element(a, a + 4, a + 9);
                std::nth_element(c, c + 4, c + 9);
                mu[(size_t)gy * mgw + gx] = a[4];
                mv[(size_t)gy * mgw + gx] = c[4];
            }
        }
    }

    // Reliability per pixel.
    std::vector<unsigned char> ok(n, 1);
    const float tolAbs = (1.5f * s) * (1.5f * s);
    parallelFor(h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                const float fu = f.u[i], fv = f.v[i];
                if ( !(fu == fu) || !(fv == fv) ) {   // NaN
                    ok[i] = 0;
                    continue;
                }
                // forward / backward check (only when the target is in the frame)
                const float tx = x + fu, ty = y + fv;
                if ( haveBack && tx >= 0.f && ty >= 0.f && tx <= (float)(w - 1) && ty <= (float)(h - 1) ) {
                    float bu, bv;
                    back->sample(tx, ty, &bu, &bv);
                    const float eu = fu + bu, ev = fv + bv;
                    const float tol = 0.03f * (fu * fu + fv * fv + bu * bu + bv * bv) + tolAbs;
                    if (eu * eu + ev * ev > tol) {
                        ok[i] = 0;
                        continue;
                    }
                }
                // wild spikes against the neighbourhood
                const size_t g = (size_t)std::min(mgh - 1, y / Bm) * mgw + std::min(mgw - 1, x / Bm);
                const float du = fu - mu[g], dv = fv - mv[g];
                const float ml = std::sqrt(mu[g] * mu[g] + mv[g] * mv[g]);
                const float lim = std::max(64.f * s, 3.f * ml + 32.f * s);   // absurd vectors only
                if (du * du + dv * dv > lim * lim) {
                    ok[i] = 0;
                }
            }
        }
    });
    if (confidence) {
        confidence->resize(n);
        for (size_t i = 0; i < n; ++i) {
            (*confidence)[i] = ok[i] ? 1.f : 0.f;
        }
    }

    // Fill: mean of the reliable vectors per small block, holes in the block
    // grid filled from their neighbours, then bilinear at the bad pixels.
    const int Bf = std::max(4, (int)std::lround(8.f * s));
    const int gw = (w + Bf - 1) / Bf, gh = (h + Bf - 1) / Bf;
    std::vector<float> gu((size_t)gw * gh, 0.f), gv((size_t)gw * gh, 0.f);
    std::vector<unsigned char> gok((size_t)gw * gh, 0);
    parallelFor(gh, [&](int b, int e) {
        for (int gy = b; gy < e; ++gy) {
            for (int gx = 0; gx < gw; ++gx) {
                double su = 0., sv = 0.;
                int cnt = 0, tot = 0;
                for (int y = gy * Bf; y < std::min(h, (gy + 1) * Bf); ++y) {
                    for (int x = gx * Bf; x < std::min(w, (gx + 1) * Bf); ++x) {
                        const size_t i = (size_t)y * w + x;
                        ++tot;
                        if (ok[i]) {
                            su += f.u[i];
                            sv += f.v[i];
                            ++cnt;
                        }
                    }
                }
                const size_t g = (size_t)gy * gw + gx;
                if ( cnt > 0 && cnt * 4 >= tot ) {
                    gu[g] = (float)(su / cnt);
                    gv[g] = (float)(sv / cnt);
                    gok[g] = 1;
                }
            }
        }
    });
    for (int pass = 0; pass < 4096; ++pass) {
        std::vector<unsigned char> next = gok;
        size_t missing = 0, fixedNow = 0;
        for (int gy = 0; gy < gh; ++gy) {
            for (int gx = 0; gx < gw; ++gx) {
                const size_t g = (size_t)gy * gw + gx;
                if (gok[g]) {
                    continue;
                }
                ++missing;
                float su = 0.f, sv = 0.f;
                int cnt = 0;
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i) {
                        const int xx = gx + i, yy = gy + j;
                        if (xx < 0 || yy < 0 || xx >= gw || yy >= gh) {
                            continue;
                        }
                        const size_t q = (size_t)yy * gw + xx;
                        if (gok[q]) {
                            su += gu[q];
                            sv += gv[q];
                            ++cnt;
                        }
                    }
                }
                if (cnt) {
                    gu[g] = su / cnt;
                    gv[g] = sv / cnt;
                    next[g] = 1;
                    ++fixedNow;
                }
            }
        }
        gok.swap(next);
        if (missing == 0 || fixedNow == 0) {
            break;
        }
    }
    parallelFor(h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                if (ok[i]) {
                    continue;
                }
                const float fx = std::max(0.f, std::min( (float)(gw - 1), ( (float)x + 0.5f ) / Bf - 0.5f ));
                const float fy = std::max(0.f, std::min( (float)(gh - 1), ( (float)y + 0.5f ) / Bf - 0.5f ));
                const int x0 = (int)fx, y0 = (int)fy;
                const int x1 = std::min(gw - 1, x0 + 1), y1 = std::min(gh - 1, y0 + 1);
                const float tx = fx - x0, ty = fy - y0;
                auto lerp2 = [&](const std::vector<float>& G) {
                    return (G[(size_t)y0 * gw + x0] * (1.f - tx) + G[(size_t)y0 * gw + x1] * tx) * (1.f - ty) +
                           (G[(size_t)y1 * gw + x0] * (1.f - tx) + G[(size_t)y1 * gw + x1] * tx) * ty;
                };
                out->u[i] = lerp2(gu);
                out->v[i] = lerp2(gv);
            }
        }
    });
}

// ------------------------------------------------------------------ motion blur

void
motionBlurGather(const float* img, int w, int h, int nc, const Field* fwd, const Field* bwd,
                 const float* matte, const std::vector<float>* confF, const std::vector<float>* confB,
                 float shutter, int samples, FilterEnum filter, float* out, bool useGpu)
{
    const size_t n = (size_t)w * h;
    const Field* F = (fwd && fwd->valid() && fwd->w == w && fwd->h == h) ? fwd : nullptr;
    const Field* B = (bwd && bwd->valid() && bwd->w == w && bwd->h == h) ? bwd : nullptr;
    if ( (!F && !B) || shutter <= 0.f || w <= 0 || h <= 0 ) {
        std::copy(img, img + n * nc, out);
        return;
    }
    if ( confF && confF->size() != n ) {
        confF = nullptr;
    }
    if ( confB && confB->size() != n ) {
        confB = nullptr;
    }
    const float half = 0.5f * shutter;
    // per-frame motion forward / backward at a pixel index or a position
    auto vecF = [&](size_t i, float* u, float* v) {
        if (F) { *u = F->u[i]; *v = F->v[i]; } else { *u = -B->u[i]; *v = -B->v[i]; }
    };
    auto vecB = [&](size_t i, float* u, float* v) {
        if (B) { *u = B->u[i]; *v = B->v[i]; } else { *u = -F->u[i]; *v = -F->v[i]; }
    };
    auto sampleF = [&](float x, float y, float* u, float* v) {
        if (F) { F->sample(x, y, u, v); } else { B->sample(x, y, u, v); *u = -*u; *v = -*v; }
    };
    auto sampleB = [&](float x, float y, float* u, float* v) {
        if (B) { B->sample(x, y, u, v); } else { F->sample(x, y, u, v); *u = -*u; *v = -*v; }
    };
    auto nearestIdx = [&](float x, float y) {
        const int xi = std::max(0, std::min(w - 1, (int)std::lround(x)));
        const int yi = std::max(0, std::min(h - 1, (int)std::lround(y)));
        return (size_t)yi * w + xi;
    };

    // Longest displacement over half the shutter -> tile size of the
    // neighbourhood-max motion (any path reaching a pixel starts within one tile).
    float M = 0.f;
    for (size_t i = 0; i < n; ++i) {
        float u, v, a, b;
        vecF(i, &u, &v);
        vecB(i, &a, &b);
        const float m = half * std::sqrt( std::max(u * u + v * v, a * a + b * b) );
        if (m == m) {
            M = std::max(M, m);
        }
    }
    const int T = std::max(8, std::min(256, (int)std::ceil(M) + 1));
    const int tw = (w + T - 1) / T, th = (h + T - 1) / T;
    std::vector<float> tFu((size_t)tw * th, 0.f), tFv(tFu), tBu(tFu), tBv(tFu);
    parallelFor(th, [&](int b0, int e0) {
        for (int ty = b0; ty < e0; ++ty) {
            for (int tx = 0; tx < tw; ++tx) {
                float bf = -1.f, bb = -1.f, fu = 0.f, fv = 0.f, bu = 0.f, bv = 0.f;
                for (int y = ty * T; y < std::min(h, (ty + 1) * T); ++y) {
                    for (int x = tx * T; x < std::min(w, (tx + 1) * T); ++x) {
                        const size_t i = (size_t)y * w + x;
                        float u, v;
                        vecF(i, &u, &v);
                        if (u * u + v * v > bf) { bf = u * u + v * v; fu = u; fv = v; }
                        vecB(i, &u, &v);
                        if (u * u + v * v > bb) { bb = u * u + v * v; bu = u; bv = v; }
                    }
                }
                const size_t t = (size_t)ty * tw + tx;
                tFu[t] = fu; tFv[t] = fv; tBu[t] = bu; tBv[t] = bv;
            }
        }
    });
    std::vector<float> nFu(tFu.size()), nFv(tFu.size()), nBu(tFu.size()), nBv(tFu.size());
    for (int ty = 0; ty < th; ++ty) {
        for (int tx = 0; tx < tw; ++tx) {
            float bf = -1.f, bb = -1.f;
            const size_t t = (size_t)ty * tw + tx;
            for (int j = -1; j <= 1; ++j) {
                for (int i = -1; i <= 1; ++i) {
                    const int xx = tx + i, yy = ty + j;
                    if (xx < 0 || yy < 0 || xx >= tw || yy >= th) {
                        continue;
                    }
                    const size_t q = (size_t)yy * tw + xx;
                    const float mf = tFu[q] * tFu[q] + tFv[q] * tFv[q];
                    if (mf > bf) { bf = mf; nFu[t] = tFu[q]; nFv[t] = tFv[q]; }
                    const float mb = tBu[q] * tBu[q] + tBv[q] * tBv[q];
                    if (mb > bb) { bb = mb; nBu[t] = tBu[q]; nBv[t] = tBv[q]; }
                }
            }
        }
    }

    // Which surface is in front: the matte, or else the flows' reliability
    // (a surface that fails the forward or backward check gets hidden or
    // revealed: it is behind). Smoothed, 1 = front.
    std::vector<float> D(n, 0.5f);
    const bool haveDepth = matte || confF || confB;
    if (haveDepth) {
        for (size_t i = 0; i < n; ++i) {
            if (matte) {
                D[i] = matte[i];
            } else {
                D[i] = std::min( confF ? (*confF)[i] : 1.f, confB ? (*confB)[i] : 1.f );
            }
        }
        const int R = std::max(1, (int)std::lround( 2.f * std::max(1.f, (float)std::max(w, h) / 1920.f) ));
        std::vector<float> tmp(n);
        parallelFor(h, [&](int b0, int e0) {
            for (int y = b0; y < e0; ++y) {
                for (int x = 0; x < w; ++x) {
                    float sacc = 0.f;
                    int cnt = 0;
                    for (int k = -R; k <= R; ++k) {
                        const int xx = x + k;
                        if (xx >= 0 && xx < w) { sacc += D[(size_t)y * w + xx]; ++cnt; }
                    }
                    tmp[(size_t)y * w + x] = sacc / cnt;
                }
            }
        });
        parallelFor(h, [&](int b0, int e0) {
            for (int y = b0; y < e0; ++y) {
                for (int x = 0; x < w; ++x) {
                    float sacc = 0.f;
                    int cnt = 0;
                    for (int k = -R; k <= R; ++k) {
                        const int yy = y + k;
                        if (yy >= 0 && yy < h) { sacc += tmp[(size_t)yy * w + x]; ++cnt; }
                    }
                    D[(size_t)y * w + x] = sacc / cnt;
                }
            }
        });
    }

    // Enough samples for ~2 px steps along the longest path.
    const int Ns = std::max( std::max(2, samples), std::min( 32, (int)std::ceil(2.f * M / 2.f) ) );

    if (useGpu && nc == 4) {
        std::vector<float> fa(n * 2), ba(n * 2), tl( (size_t)tw * th * 4 );
        for (size_t i = 0; i < n; ++i) {
            vecF(i, &fa[i * 2], &fa[i * 2 + 1]);
            vecB(i, &ba[i * 2], &ba[i * 2 + 1]);
        }
        for (size_t q = 0; q < nFu.size(); ++q) {
            tl[q * 4] = nFu[q];
            tl[q * 4 + 1] = nFv[q];
            tl[q * 4 + 2] = nBu[q];
            tl[q * 4 + 3] = nBv[q];
        }
        gpu::GatherJob j;
        j.w = w; j.h = h; j.tw = tw; j.th = th; j.T = T; j.Ns = Ns; j.filter = (int)filter;
        j.haveDepth = haveDepth;
        j.shutter = shutter;
        j.img = img; j.F = fa.data(); j.B = ba.data(); j.D = D.data(); j.tiles = tl.data();
        std::string err;
        if ( gpu::motionBlurGather(j, out, &err) ) {
            setGpuError( std::string() );
            return;
        }
        setGpuError(err);
    }
    parallelFor(h, [&](int b0, int e0) {
        std::vector<float> c2(nc), own( (size_t)Ns * nc ), acc(nc);
        std::vector<signed char> state(Ns);   // 1 lands, 0 revealed background (keep), -1 replace
        for (int y = b0; y < e0; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                const size_t t = (size_t)std::min(th - 1, y / T) * tw + std::min(tw - 1, x / T);
                float fu, fv, bu, bv;
                vecF(i, &fu, &fv);
                vecB(i, &bu, &bv);
                const float dX = D[i];
                // 1) the pixel's own path
                int nLand = 0;
                std::fill(acc.begin(), acc.end(), 0.f);
                for (int k = 0; k < Ns; ++k) {
                    const float tau = ( (k + 0.5f) / Ns ) * shutter - half;
                    const bool fwdSide = tau >= 0.f;
                    const float a = std::fabs(tau);
                    const float dx = a * (fwdSide ? fu : bu), dy = a * (fwdSide ? fv : bv);
                    const float sx = x - dx, sy = y - dy;
                    float su, sv;
                    fwdSide ? sampleF(sx, sy, &su, &sv) : sampleB(sx, sy, &su, &sv);
                    const float ex = a * su - dx, ey = a * sv - dy;
                    const float dl = std::sqrt(dx * dx + dy * dy);
                    const float tol = std::max(1.f, 0.3f * dl);
                    float* o = &own[(size_t)k * nc];
                    sampleImage(img, w, h, nc, sx, sy, filter, o);
                    if (ex * ex + ey * ey <= tol * tol) {
                        state[k] = 1;
                        for (int c = 0; c < nc; ++c) {
                            acc[c] += o[c];
                        }
                        ++nLand;
                    } else {
                        // The source is another surface. If this pixel is in
                        // front and moves away, the surface behind shows up:
                        // that source is a good guess of it. Otherwise (this
                        // pixel is the background) use its own visible samples.
                        state[k] = ( haveDepth && dX > D[nearestIdx(sx, sy)] + 0.25f ) ? 0 : -1;
                    }
                }
                for (int c = 0; c < nc; ++c) {
                    acc[c] = nLand ? acc[c] / nLand : img[i * nc + c];
                }
                for (int k = 0; k < Ns; ++k) {
                    if (state[k] < 0) {
                        std::copy(acc.begin(), acc.end(), own.begin() + (size_t)k * nc);
                    }
                }
                // 2) faster neighbouring motion passing over this pixel
                float* o = out + i * nc;
                std::fill(o, o + nc, 0.f);
                for (int k = 0; k < Ns; ++k) {
                    const float tau = ( (k + 0.5f) / Ns ) * shutter - half;
                    const bool fwdSide = tau >= 0.f;
                    const float a = std::fabs(tau);
                    const float dx = a * (fwdSide ? fu : bu), dy = a * (fwdSide ? fv : bv);
                    const float ndx = a * (fwdSide ? nFu[t] : nBu[t]), ndy = a * (fwdSide ? nFv[t] : nBv[t]);
                    const float* src = &own[(size_t)k * nc];
                    float w2 = 0.f;
                    if ( (ndx - dx) * (ndx - dx) + (ndy - dy) * (ndy - dy) > 1.f ) {
                        const float sx = x - ndx, sy = y - ndy;
                        float su, sv;
                        fwdSide ? sampleF(sx, sy, &su, &sv) : sampleB(sx, sy, &su, &sv);
                        const float ex = a * su - ndx, ey = a * sv - ndy;
                        const float nl = std::sqrt(ndx * ndx + ndy * ndy);
                        const float tol = std::max(1.f, 0.3f * nl);
                        if (ex * ex + ey * ey <= tol * tol) {
                            // both surfaces are here at this time: the front one wins
                            w2 = 0.5f + ( D[nearestIdx(sx, sy)] - dX );
                            w2 = std::max(0.f, std::min(1.f, w2));
                            if (w2 > 0.f) {
                                sampleImage(img, w, h, nc, sx, sy, filter, c2.data());
                            }
                        }
                    }
                    for (int c = 0; c < nc; ++c) {
                        o[c] += (w2 > 0.f) ? (1.f - w2) * src[c] + w2 * c2[c] : src[c];
                    }
                }
                for (int c = 0; c < nc; ++c) {
                    o[c] /= (float)Ns;
                }
            }
        }
    });
}

std::string
lastGpuError()
{
    std::lock_guard<std::mutex> l(gGpuErrLock);
    return gGpuErr;
}

// ------------------------------------------------------------------ cache

namespace {

struct Cache
{
    std::mutex m;
    std::list<std::pair<std::string, FieldPtr> > lru;   // front = most recent
    size_t bytes = 0;
    static const size_t kMaxBytes = (size_t)768 << 20;   // 768 MB
};

Cache&
cache()
{
    static Cache c;
    return c;
}

} // anonymous namespace

FieldPtr
cacheGet(const std::string& key)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> l(c.m);
    for (auto it = c.lru.begin(); it != c.lru.end(); ++it) {
        if (it->first == key) {
            c.lru.splice(c.lru.begin(), c.lru, it);
            return c.lru.front().second;
        }
    }
    return FieldPtr();
}

void
cachePut(const std::string& key, const FieldPtr& f)
{
    if (!f) {
        return;
    }
    Cache& c = cache();
    std::lock_guard<std::mutex> l(c.m);
    for (auto it = c.lru.begin(); it != c.lru.end(); ++it) {
        if (it->first == key) {
            c.bytes -= it->second->u.size() * 2 * sizeof(float);
            c.lru.erase(it);
            break;
        }
    }
    c.lru.push_front(std::make_pair(key, f));
    c.bytes += f->u.size() * 2 * sizeof(float);
    while (c.bytes > Cache::kMaxBytes && c.lru.size() > 2) {
        c.bytes -= c.lru.back().second->u.size() * 2 * sizeof(float);
        c.lru.pop_back();
    }
}

void
cacheClear()
{
    Cache& c = cache();
    std::lock_guard<std::mutex> l(c.m);
    c.lru.clear();
    c.bytes = 0;
}

} // namespace flow
