/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_DEV_MOTION_MOTIONCOMMON_H
#define NATRON_ENGINE_DEV_MOTION_MOTIONCOMMON_H

// Natron side of the Motion nodes: knobs shared by MotionBlur / Kronos /
// OFlow / SmartVector (Vector Detail, Strength / Smoothness, Motion, Filter,
// Matte), whole-frame fetching of inputs at any time, and cached flows.

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <string>
#include <vector>

#include "../../ChoiceOption.h"
#include "../../EffectInstance.h"
#include "../../ImagePlaneDesc.h"
#include "../../RectI.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

#include "FlowCore.h"

NATRON_NAMESPACE_ENTER

namespace motion {

// A whole frame as an interleaved float buffer over `bounds` (pixel coords).
struct Frame
{
    RectI bounds;
    int nc = 4;
    std::vector<float> px;

    int w() const { return bounds.x2 - bounds.x1; }
    int h() const { return bounds.y2 - bounds.y1; }
    bool valid() const { return w() > 0 && h() > 0 && px.size() == (size_t)w() * h() * nc; }
    float* at(int x, int y) { return &px[((size_t)(y - bounds.y1) * w() + (x - bounds.x1)) * nc]; }
    const float* at(int x, int y) const { return &px[((size_t)(y - bounds.y1) * w() + (x - bounds.x1)) * nc]; }
};

// Fetch input `inputNb` at `time` over `bounds` (edges clamped where the
// input is smaller), `nc` channels (RGBA = 4; missing alpha = 1). `layer`:
// plane to fetch (default: the colour plane). Must run inside render().
bool fetchFrame(EffectInstance* fx, int inputNb, double time, const RenderScale& scale, ViewIdx view,
                const RectI& bounds, int nc, Frame* out, const ImagePlaneDesc* layer = NULL);

// Frame range of an input (or 1..1).
void inputFrameRange(EffectInstance* fx, int inputNb, double* first, double* last);

// Flow from input frame t0 to frame t1 (both fetched over `bounds`), cached
// process-wide. `tag` distinguishes callers (node + options).
flow::FieldPtr getFlow(EffectInstance* fx, int inputNb, double t0, double t1,
                       const RenderScale& scale, ViewIdx view, const RectI& bounds,
                       const flow::Params& params, const std::string& tag,
                       const Frame* f0 = NULL, const Frame* f1 = NULL);

// ---------------------------------------------------------------- matte
enum MatteEnum
{
    eMatteNone = 0,
    eMatteSourceAlpha,
    eMatteSourceInvertedAlpha,
    eMatteMatteLuminance,
    eMatteMatteInvertedLuminance,
    eMatteMatteAlpha,
    eMatteMatteInvertedAlpha,
};
std::vector<ChoiceOption> matteChoices();
// Matte (one value per pixel, 0..1) of frame `time`, from the source frame
// (already fetched) or the Matte input. Empty when None or not connected.
bool fetchMatte(EffectInstance* fx, int matteInput, int mode, const Frame& src, double time,
                const RenderScale& scale, ViewIdx view, std::vector<float>* matte);

// Interpolated image at fraction a between A and B, with an optional matte
// per frame (foreground and background moved with their own vectors, so
// they don't drag each other at the matte edge).
void interpolateFrames(const Frame& A, const Frame& B, const flow::Field& fAB, const flow::Field& fBA,
                       const std::vector<float>& mA, const std::vector<float>& mB,
                       float a, flow::FilterEnum filter, Frame* out);

std::vector<ChoiceOption> filterChoices();
std::vector<ChoiceOption> motionChoices();

// Shared "Local GPU" line of the Nuke nodes: this build computes on the CPU.
void addComputeInfo(EffectInstance* fx, const KnobPagePtr& page);

std::string scaleTag(const RenderScale& scale, const RectI& bounds);

// Copy a frame (nc channels) into an output image (its own bounds and
// channel count; 1-channel images get the alpha).
void writeFrame(const Frame& f, const ImagePtr& out);

// getFramesNeeded helper: inputs `inputs` need [first, last] at `view`.
FramesNeededMap framesRange(EffectInstance* fx, const std::vector<int>& inputs, double first, double last, ViewIdx view);

// Natron's abort test for the render thread.
inline bool isAborted(EffectInstance* fx) { return fx->aborted(); }

} // namespace motion

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MOTION_MOTIONCOMMON_H
