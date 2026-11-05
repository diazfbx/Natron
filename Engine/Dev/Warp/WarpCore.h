/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_DEV_WARP_WARPCORE_H
#define NATRON_ENGINE_DEV_WARP_WARPCORE_H

// Image warps for GridWarp and SplineWarp. Plain C++, no Natron types.
//
// Coordinates: pixel space of the buffers (x right, y along the rows), pixel
// centres at integer + 0.5... images are sampled with centres at integer
// coordinates (as flow::sampleImage) — callers pass positions in that space.

#include <vector>

namespace warp {

struct Vec2
{
    double x = 0., y = 0.;
    Vec2() {}
    Vec2(double x_, double y_) : x(x_), y(y_) {}
};

// Smooth surface through a cols x rows grid of points (row-major, row 0
// first): Catmull-Rom in both directions, so it passes through every point.
// (u, v) in [0, cols - 1] x [0, rows - 1].
Vec2 gridEval(const std::vector<Vec2>& pts, int cols, int rows, double u, double v);

// Source position for every output pixel of a grid warp: the destination
// grid is tessellated (subdiv x subdiv per cell) and rasterised; inside it,
// map(x, y) = the matching point of the source grid. Outside the grid: the
// identity (the image is unchanged there). map: w*h*2 floats (x, y), output
// pixel (i, j) has centre (ox + i, oy + j) in the grids' coordinate space.
void gridWarpMap(const std::vector<Vec2>& srcGrid, const std::vector<Vec2>& dstGrid, int cols, int rows,
                 int subdiv, int w, int h, double ox, double oy, std::vector<float>* map,
                 std::vector<float>* coverage = nullptr);

// Moving-least-squares warp (Schaefer et al. 2006, rigid): for every output
// pixel v, the source position f(v) given point correspondences dst[i] ->
// src[i] (the shape at dst in the output comes from src in the input).
// Computed on a grid every `step` pixels and interpolated. alpha: weight
// falloff exponent (1 = 1/d^2).
void mlsWarpMap(const std::vector<Vec2>& src, const std::vector<Vec2>& dst, double alpha, int step,
                int w, int h, double ox, double oy, std::vector<float>* map);

// out(i, j) = img(map(i, j)), nComp channels, bilinear (filter 1) or cubic
// (2) or nearest (0). img covers [ix, ix + iw) x [iy, iy + ih) in the map's
// coordinate space; outside: edge clamped (clampEdges) or 0.
void applyMap(const float* img, int iw, int ih, double ix, double iy, int nComp,
              const std::vector<float>& map, int w, int h, int filter, bool clampEdges, float* out);

} // namespace warp

#endif // NATRON_ENGINE_DEV_WARP_WARPCORE_H
