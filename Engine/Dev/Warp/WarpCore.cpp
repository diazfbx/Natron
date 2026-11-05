/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#include "WarpCore.h"

#include <algorithm>
#include <cmath>

#include "../Motion/FlowCore.h"   // parallelFor, sampleImage

namespace warp {

namespace {

inline Vec2
catmull(const Vec2& p0, const Vec2& p1, const Vec2& p2, const Vec2& p3, double t)
{
    const double t2 = t * t, t3 = t2 * t;
    const double a = -0.5 * t3 + t2 - 0.5 * t;
    const double b = 1.5 * t3 - 2.5 * t2 + 1.;
    const double c = -1.5 * t3 + 2. * t2 + 0.5 * t;
    const double d = 0.5 * t3 - 0.5 * t2;
    return Vec2(a * p0.x + b * p1.x + c * p2.x + d * p3.x, a * p0.y + b * p1.y + c * p2.y + d * p3.y);
}

// Grid point with linear extrapolation beyond the borders (keeps the
// boundary curves natural).
inline Vec2
gridAt(const std::vector<Vec2>& p, int cols, int rows, int i, int j)
{
    const int ci = std::max(0, std::min(cols - 1, i)), cj = std::max(0, std::min(rows - 1, j));
    Vec2 v = p[(size_t)cj * cols + ci];
    if (i != ci && cols >= 2) {
        const int in = i < 0 ? 1 : cols - 2;
        const Vec2 q = p[(size_t)cj * cols + in];
        v = Vec2(2. * v.x - q.x, 2. * v.y - q.y);
    }
    if (j != cj && rows >= 2) {
        const int jn = j < 0 ? 1 : rows - 2;
        const int ii = std::max(0, std::min(cols - 1, i));
        const Vec2 q = p[(size_t)jn * cols + ii];
        const Vec2 b = p[(size_t)cj * cols + ii];
        v = Vec2(v.x + (b.x - q.x), v.y + (b.y - q.y));
    }
    return v;
}

} // anonymous namespace

Vec2
gridEval(const std::vector<Vec2>& pts, int cols, int rows, double u, double v)
{
    if (cols < 1 || rows < 1 || pts.size() < (size_t)cols * rows) {
        return Vec2(u, v);
    }
    u = std::max(0., std::min( (double)(cols - 1), u ));
    v = std::max(0., std::min( (double)(rows - 1), v ));
    int i = std::min(cols - 2, (int)std::floor(u));
    int j = std::min(rows - 2, (int)std::floor(v));
    i = std::max(0, i);
    j = std::max(0, j);
    const double tu = cols >= 2 ? u - i : 0., tv = rows >= 2 ? v - j : 0.;
    Vec2 r[4];
    for (int k = -1; k <= 2; ++k) {
        r[k + 1] = catmull( gridAt(pts, cols, rows, i - 1, j + k), gridAt(pts, cols, rows, i, j + k),
                            gridAt(pts, cols, rows, i + 1, j + k), gridAt(pts, cols, rows, i + 2, j + k), tu );
    }
    return catmull(r[0], r[1], r[2], r[3], tv);
}

void
gridWarpMap(const std::vector<Vec2>& srcGrid, const std::vector<Vec2>& dstGrid, int cols, int rows,
            int subdiv, int w, int h, double ox, double oy, std::vector<float>* map, std::vector<float>* coverage)
{
    map->resize( (size_t)w * h * 2 );
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            (*map)[( (size_t)y * w + x ) * 2] = (float)(ox + x);
            (*map)[( (size_t)y * w + x ) * 2 + 1] = (float)(oy + y);
        }
    }
    if (coverage) {
        coverage->assign( (size_t)w * h, 0.f );
    }
    if (cols < 2 || rows < 2) {
        return;
    }
    subdiv = std::max(1, subdiv);
    const int nu = (cols - 1) * subdiv + 1, nv = (rows - 1) * subdiv + 1;
    std::vector<Vec2> D( (size_t)nu * nv ), S( (size_t)nu * nv );
    flow::parallelFor(nv, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            for (int i = 0; i < nu; ++i) {
                const double u = (double)i / subdiv, v = (double)j / subdiv;
                D[(size_t)j * nu + i] = gridEval(dstGrid, cols, rows, u, v);
                S[(size_t)j * nu + i] = gridEval(srcGrid, cols, rows, u, v);
            }
        }
    });
    // Rasterise the destination triangles; each thread owns a band of rows.
    flow::parallelFor(h, [&](int yb, int ye) {
        auto tri = [&](size_t a, size_t b, size_t c) {
            const Vec2 &A = D[a], &B = D[b], &C = D[c];
            const double area = (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
            if (std::fabs(area) < 1e-12) {
                return;
            }
            const double minx = std::min( A.x, std::min(B.x, C.x) ) - ox, maxx = std::max( A.x, std::max(B.x, C.x) ) - ox;
            const double miny = std::min( A.y, std::min(B.y, C.y) ) - oy, maxy = std::max( A.y, std::max(B.y, C.y) ) - oy;
            const int x0 = std::max(0, (int)std::ceil(minx - 1e-9)), x1 = std::min(w - 1, (int)std::floor(maxx + 1e-9));
            const int y0 = std::max(yb, (int)std::ceil(miny - 1e-9)), y1 = std::min(ye - 1, (int)std::floor(maxy + 1e-9));
            for (int y = y0; y <= y1; ++y) {
                const double py = oy + y;
                for (int x = x0; x <= x1; ++x) {
                    const double px = ox + x;
                    const double w0 = ( (B.x - px) * (C.y - py) - (C.x - px) * (B.y - py) ) / area;
                    const double w1 = ( (C.x - px) * (A.y - py) - (A.x - px) * (C.y - py) ) / area;
                    const double w2 = 1. - w0 - w1;
                    if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) {
                        continue;
                    }
                    const size_t o = ( (size_t)y * w + x ) * 2;
                    (*map)[o] = (float)(w0 * S[a].x + w1 * S[b].x + w2 * S[c].x);
                    (*map)[o + 1] = (float)(w0 * S[a].y + w1 * S[b].y + w2 * S[c].y);
                    if (coverage) {
                        (*coverage)[(size_t)y * w + x] = 1.f;
                    }
                }
            }
        };
        for (int j = 0; j + 1 < nv; ++j) {
            for (int i = 0; i + 1 < nu; ++i) {
                const size_t p00 = (size_t)j * nu + i, p10 = p00 + 1, p01 = p00 + nu, p11 = p01 + 1;
                tri(p00, p10, p11);
                tri(p00, p11, p01);
            }
        }
    });
}

void
mlsWarpMap(const std::vector<Vec2>& src, const std::vector<Vec2>& dst, double alpha, int step,
           int w, int h, double ox, double oy, std::vector<float>* map)
{
    map->resize( (size_t)w * h * 2 );
    const size_t n = std::min( src.size(), dst.size() );
    step = std::max(1, step);
    const int gw = (w + step - 1) / step + 1, gh = (h + step - 1) / step + 1;
    std::vector<float> gx( (size_t)gw * gh ), gy( (size_t)gw * gh );
    flow::parallelFor(gh, [&](int b, int e) {
        for (int j = b; j < e; ++j) {
            for (int i = 0; i < gw; ++i) {
                const double vx = ox + i * step, vy = oy + j * step;
                double sw = 0., psx = 0., psy = 0., qsx = 0., qsy = 0.;
                std::vector<double> wts(n);
                bool exact = false;
                for (size_t k = 0; k < n; ++k) {
                    const double dx = dst[k].x - vx, dy = dst[k].y - vy;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < 1e-10) {
                        gx[(size_t)j * gw + i] = (float)src[k].x;
                        gy[(size_t)j * gw + i] = (float)src[k].y;
                        exact = true;
                        break;
                    }
                    const double wk = 1. / std::pow(d2, alpha);
                    wts[k] = wk;
                    sw += wk;
                    psx += wk * dst[k].x;
                    psy += wk * dst[k].y;
                    qsx += wk * src[k].x;
                    qsy += wk * src[k].y;
                }
                if (exact) {
                    continue;
                }
                if (n == 0 || sw <= 0.) {
                    gx[(size_t)j * gw + i] = (float)vx;
                    gy[(size_t)j * gw + i] = (float)vy;
                    continue;
                }
                psx /= sw; psy /= sw; qsx /= sw; qsy /= sw;
                const double dx = vx - psx, dy = vy - psy;
                // rigid MLS: fr = sum q^_k A_k, A_k = w_k [p^; -p^perp][d; -d perp]^T
                double frx = 0., fry = 0.;
                for (size_t k = 0; k < n; ++k) {
                    const double px = dst[k].x - psx, py = dst[k].y - psy;
                    const double qx = src[k].x - qsx, qy = src[k].y - qsy;
                    // [p^; -p^perp] = [[px, py], [py, -px]], [d; -d perp] = [[dx, dy], [dy, -dx]]
                    const double a11 = px * dx + py * dy, a12 = px * dy - py * dx;
                    const double a21 = py * dx - px * dy, a22 = py * dy + px * dx;
                    frx += wts[k] * (qx * a11 + qy * a21);
                    fry += wts[k] * (qx * a12 + qy * a22);
                }
                const double fl = std::sqrt(frx * frx + fry * fry), dl = std::sqrt(dx * dx + dy * dy);
                if (fl > 1e-12) {
                    gx[(size_t)j * gw + i] = (float)(dl * frx / fl + qsx);
                    gy[(size_t)j * gw + i] = (float)(dl * fry / fl + qsy);
                } else {
                    gx[(size_t)j * gw + i] = (float)(dx + qsx);
                    gy[(size_t)j * gw + i] = (float)(dy + qsy);
                }
            }
        }
    });
    flow::parallelFor(h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            const int j = y / step;
            const double ty = (double)(y - j * step) / step;
            for (int x = 0; x < w; ++x) {
                const int i = x / step;
                const double tx = (double)(x - i * step) / step;
                const size_t a = (size_t)j * gw + i, bb = a + 1, c = a + gw, d = c + 1;
                const double mx = (gx[a] * (1 - tx) + gx[bb] * tx) * (1 - ty) + (gx[c] * (1 - tx) + gx[d] * tx) * ty;
                const double my = (gy[a] * (1 - tx) + gy[bb] * tx) * (1 - ty) + (gy[c] * (1 - tx) + gy[d] * tx) * ty;
                (*map)[( (size_t)y * w + x ) * 2] = (float)mx;
                (*map)[( (size_t)y * w + x ) * 2 + 1] = (float)my;
            }
        }
    });
}

void
applyMap(const float* img, int iw, int ih, double ix, double iy, int nComp,
         const std::vector<float>& map, int w, int h, int filter, bool clampEdges, float* out)
{
    flow::parallelFor(h, [&](int b, int e) {
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                const float sx = (float)(map[i * 2] - ix), sy = (float)(map[i * 2 + 1] - iy);
                float* o = out + i * nComp;
                if ( !clampEdges && (sx < -0.5f || sy < -0.5f || sx > iw - 0.5f || sy > ih - 0.5f) ) {
                    for (int c = 0; c < nComp; ++c) {
                        o[c] = 0.f;
                    }
                    continue;
                }
                flow::sampleImage(img, iw, ih, nComp, sx, sy, (flow::FilterEnum)filter, o);
            }
        }
    });
}

} // namespace warp
