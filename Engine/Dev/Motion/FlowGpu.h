/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_DEV_MOTION_FLOWGPU_H
#define NATRON_ENGINE_DEV_MOTION_FLOWGPU_H

// GPU versions (Dev/Gpu/GpuCompute, wgpu) of FlowCore's two heavy loops: the
// variational refinement of one pyramid level and the motion-blur gather.
// Same maths as the CPU code; they return false (err set) when the GPU can't
// run them, and the caller runs the CPU code instead.

#include <functional>
#include <string>
#include <vector>

namespace flow {
namespace gpu {

// FlowCore's refineLevel: I0, I1, u, v are w x h; u, v refined in place.
bool refineLevel(const std::vector<float>& I0, const std::vector<float>& I1, int w, int h,
                 std::vector<float>& u, std::vector<float>& v, float alpha, int warps, int iters,
                 int medianRadius, const std::function<bool()>& aborted, std::string* err);

// The per-pixel part of motionBlurGather (RGBA only). F, B: w*h*2 (per-frame
// motion forward / backward, interleaved), D: w*h front-ness, tiles:
// tw*th*4 (neighbourhood-max forward u, v, backward u, v).
struct GatherJob
{
    int w = 0, h = 0, tw = 0, th = 0, T = 1, Ns = 1, filter = 1;
    bool haveDepth = false;
    float shutter = 0.f;
    const float* img = nullptr;
    const float* F = nullptr;
    const float* B = nullptr;
    const float* D = nullptr;
    const float* tiles = nullptr;
};
bool motionBlurGather(const GatherJob& job, float* out, std::string* err);

} // namespace gpu
} // namespace flow

#endif // NATRON_ENGINE_DEV_MOTION_FLOWGPU_H
