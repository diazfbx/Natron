/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_DEV_MOTION_FLOWCORE_H
#define NATRON_ENGINE_DEV_MOTION_FLOWCORE_H

// Dense optical flow and flow-based image warping for the Motion nodes
// (MotionBlur, Kronos, OFlow, SmartVector, VectorDistort).
//
// Plain C++ (no Natron / Qt types), so it can be tested on its own.
//
// Flow model: coarse-to-fine variational flow (Brox et al. 2004 without the
// gradient-constancy term, i.e. Horn-Schunck with robust Charbonnier data and
// smoothness terms, linearised by warping), solved with red-black SOR, with a
// median filter on the flow after every warp (Sun, Roth & Black 2010,
// "Secrets of optical flow"). Computed at a working resolution set by
// "Vector Detail" (Nuke's knob: proportion of the image size), upsampled.
//
// Conventions: images are interleaved float buffers, row-major, nComp
// channels, w x h. A flow field F from image A to image B gives for every
// pixel x of A the displacement d such that A(x) ~ B(x + d), in pixels of
// these buffers (x to the right, y along the rows).

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flow {

struct Field
{
    int w = 0, h = 0;
    std::vector<float> u, v;   // w*h each

    bool valid() const { return w > 0 && h > 0 && u.size() == (size_t)w * h && v.size() == u.size(); }
    float uAt(int x, int y) const { return u[(size_t)y * w + x]; }
    float vAt(int x, int y) const { return v[(size_t)y * w + x]; }
    // bilinear, clamped to the edges
    void sample(float x, float y, float* du, float* dv) const;
};
typedef std::shared_ptr<const Field> FieldPtr;

struct Params
{
    double detail = 0.3;       // working resolution / image resolution (Nuke "Vector Detail")
    double smoothness = 0.5;   // regularisation (OFlow "Smoothness"; Kronos "Strength" / 3)
    bool local = false;        // Nuke "Motion: Local" (less smoothing, sharper motion edges)
    int maxWorkingSize = 4096; // clamp of the working resolution (longest side)
    std::function<bool()> abort;   // polled from the calling thread only; true = stop
};

// Number of worker threads used by parallelFor (hardware concurrency).
int threadCount();
// Splits [0, n) into chunks run on threadCount() threads. f(begin, end).
void parallelFor(int n, const std::function<void(int, int)>& f);

// Optical flow from A to B (both w x h x nComp). Returns false if aborted
// or on bad input.
bool computeFlow(const float* A, const float* B, int w, int h, int nComp,
                 const Params& p, Field* out);

enum FilterEnum
{
    eFilterNearest = 0,
    eFilterBilinear,
    eFilterCubic,
};

// Sample nComp channels of img at (x, y) (pixel centers at integer
// coordinates), clamped to the edges.
void sampleImage(const float* img, int w, int h, int nComp, float x, float y,
                 FilterEnum filter, float* out);

// Image at fraction a (0 = A, 1 = B) of the way from A to B, from the two
// flows fAB and fBA: the A->B flow is projected to time a, occlusions are
// resolved with the forward / backward consistency. out: w*h*nComp.
void interpolate(const float* A, const float* B, int w, int h, int nComp,
                 const Field& fAB, const Field& fBA, float a, FilterEnum filter,
                 float* out);

// Flow at time a between A and B (A->B displacement of the point that is at
// y at time a): projection of fAB (and -fBA) with hole filling.
void projectFlow(const Field& fAB, const Field& fBA, float a, Field* out);

// Robust vectors: vectors of fAB that fail the forward / backward check
// against fBA, or that are wild spikes compared with their neighbourhood,
// are replaced by a smooth fill from the reliable vectors around them.
// confidence (optional): 1 = kept, 0 = replaced.
void cleanFlow(const Field& fAB, const Field* fBA, Field* out, std::vector<float>* confidence = nullptr);

// Motion blur of one image along its own vectors (gather): fwd = motion to
// the next frame, bwd = motion to the previous frame (either may be null:
// the other one, reversed, is used). The shutter covers [-shutter/2,
// +shutter/2] frames. Samples along the pixel's own path and along the
// largest neighbouring motion (so fast objects also smear over static
// pixels). matte (optional, w*h, 1 = foreground) decides which surface wins
// where two motions overlap; without it they are blended.
// confF / confB (optional, from cleanFlow): where a motion fails the
// forward / backward check the surface gets hidden — used, without a matte,
// to decide which surface is in front.
void motionBlurGather(const float* img, int w, int h, int nComp, const Field* fwd, const Field* bwd,
                      const float* matte, const std::vector<float>* confF, const std::vector<float>* confB,
                      float shutter, int samples, FilterEnum filter, float* out);

// Warp: out(x) = img(x + scale * F(x)).
void warpByField(const float* img, int w, int h, int nComp, const Field& F, float scale,
                 FilterEnum filter, float* out);

// ------------------------------------------------------------------- cache
// Small process-wide LRU of computed flows, keyed by a string the caller
// builds from everything the flow depends on (input hash, times, params,
// render scale).
FieldPtr cacheGet(const std::string& key);
void cachePut(const std::string& key, const FieldPtr& f);
void cacheClear();

} // namespace flow

#endif // NATRON_ENGINE_DEV_MOTION_FLOWCORE_H
