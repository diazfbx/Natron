/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "MotionCommon.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

NATRON_NAMESPACE_ENTER

namespace motion {

bool
fetchFrame(EffectInstance* fx, int inputNb, double time, const RenderScale& scale, ViewIdx view,
           const RectI& bounds, int nc, Frame* out, const ImagePlaneDesc* layer)
{
    out->bounds = bounds;
    out->nc = nc;
    out->px.assign((size_t)std::max(0, bounds.width()) * std::max(0, bounds.height()) * nc, 0.f);
    if (bounds.isNull()) {
        return false;
    }
    EffectInstancePtr input = fx->getInput(inputNb);
    if (!input) {
        return false;
    }
    RectD rod;
    bool isProject = false;
    StatusEnum st = input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &rod, &isProject);
    if (st == eStatusFailed || rod.isNull()) {
        return false;
    }
    RectI roiPx;
    ImagePtr img = fx->getImage(inputNb, time, scale, view, &rod, layer, false, false, eStorageModeRAM, NULL, &roiPx);
    if (!img) {
        return false;
    }
    const RectI ib = img->getBounds();
    if (ib.isNull()) {
        return false;
    }
    const int inc = img->getComponents().getNumComponents();
    Image::ReadAccess ra( img.get() );
    const int w = bounds.width();
    flow::parallelFor(bounds.height(), [&](int b, int e) {
        for (int yy = b; yy < e; ++yy) {
            const int y = std::max(ib.y1, std::min(ib.y2 - 1, bounds.y1 + yy));
            for (int xx = 0; xx < w; ++xx) {
                const int x = std::max(ib.x1, std::min(ib.x2 - 1, bounds.x1 + xx));
                const float* p = (const float*)ra.pixelAt(x, y);
                float* d = &out->px[((size_t)yy * w + xx) * nc];
                if (!p) {
                    continue;
                }
                if (inc == 1) {   // alpha only
                    for (int c = 0; c < nc; ++c) {
                        d[c] = (c == 3 || nc == 1) ? p[0] : 0.f;
                    }
                    continue;
                }
                for (int c = 0; c < nc; ++c) {
                    d[c] = c < inc ? p[c] : (c == 3 ? 1.f : 0.f);
                }
            }
        }
    });
    return true;
}

bool
inputBounds(EffectInstance* fx, int inputNb, double time, const RenderScale& scale, ViewIdx view, RectI* out)
{
    EffectInstancePtr input = fx->getInput(inputNb);
    if (!input) {
        return false;
    }
    RectD rod;
    bool isProject = false;
    StatusEnum st = input->getRegionOfDefinition_public(input->getRenderHash(), time, scale, view, &rod, &isProject);
    if (st == eStatusFailed || rod.isNull()) {
        return false;
    }
    *out = rod.toPixelEnclosing( scale, fx->getAspectRatio(inputNb) );
    return !out->isNull();
}

void
inputFrameRange(EffectInstance* fx, int inputNb, double* first, double* last)
{
    *first = 1;
    *last = 1;
    EffectInstancePtr input = fx->getInput(inputNb);
    if (input) {
        input->getFrameRange_public(input->getHash(), first, last);
        if (*first == std::numeric_limits<int>::min() || *last == std::numeric_limits<int>::max()) {
            *first = 1;
            *last = 1;
        }
    }
}

std::string
scaleTag(const RenderScale& scale, const RectI& bounds)
{
    std::ostringstream s;
    s << "m" << scale.toMipmapLevel() << "_" << bounds.x1 << "_" << bounds.y1 << "_" << bounds.x2 << "_" << bounds.y2;
    return s.str();
}

flow::FieldPtr
getFlow(EffectInstance* fx, int inputNb, double t0, double t1, const RenderScale& scale, ViewIdx view,
        const RectI& bounds, const flow::Params& params, const std::string& tag,
        const Frame* f0, const Frame* f1)
{
    EffectInstancePtr input = fx->getInput(inputNb);
    if (!input) {
        return flow::FieldPtr();
    }
    std::ostringstream key;
    key << (const void*)input.get() << ":" << input->getRenderHash() << ":" << tag << ":" << t0 << ">" << t1
        << ":d" << params.detail << ":s" << params.smoothness << ":l" << params.local << ":" << scaleTag(scale, bounds);
    flow::FieldPtr cached = flow::cacheGet( key.str() );
    if (cached) {
        return cached;
    }
    Frame a, b;
    if (!f0) {
        if (!fetchFrame(fx, inputNb, t0, scale, view, bounds, 4, &a)) {
            return flow::FieldPtr();
        }
        f0 = &a;
    }
    if (!f1) {
        if (!fetchFrame(fx, inputNb, t1, scale, view, bounds, 4, &b)) {
            return flow::FieldPtr();
        }
        f1 = &b;
    }
    std::shared_ptr<flow::Field> f = std::make_shared<flow::Field>();
    flow::Params p = params;
    p.abort = [fx]() { return fx->aborted(); };
    if (!flow::computeFlow(f0->px.data(), f1->px.data(), f0->w(), f0->h(), f0->nc, p, f.get())) {
        return flow::FieldPtr();
    }
    flow::cachePut(key.str(), f);
    return f;
}

std::vector<ChoiceOption>
matteChoices()
{
    std::vector<ChoiceOption> o;
    o.push_back(ChoiceOption("none", "None", "No matte: one motion for the whole image."));
    o.push_back(ChoiceOption("srcAlpha", "Source Alpha", "The source alpha separates the foreground."));
    o.push_back(ChoiceOption("srcInvAlpha", "Source Inverted Alpha", ""));
    o.push_back(ChoiceOption("matteLum", "Matte Luminance", "Luminance of the Matte input."));
    o.push_back(ChoiceOption("matteInvLum", "Matte Inverted Luminance", ""));
    o.push_back(ChoiceOption("matteAlpha", "Matte Alpha", "Alpha of the Matte input."));
    o.push_back(ChoiceOption("matteInvAlpha", "Matte Inverted Alpha", ""));
    return o;
}

bool
fetchMatte(EffectInstance* fx, int matteInput, int mode, const Frame& src, double time,
           const RenderScale& scale, ViewIdx view, std::vector<float>* matte)
{
    matte->clear();
    if (mode == eMatteNone || !src.valid()) {
        return false;
    }
    const size_t n = (size_t)src.w() * src.h();
    matte->resize(n);
    if (mode == eMatteSourceAlpha || mode == eMatteSourceInvertedAlpha) {
        const bool inv = mode == eMatteSourceInvertedAlpha;
        for (size_t i = 0; i < n; ++i) {
            const float a = src.nc >= 4 ? src.px[i * src.nc + 3] : 1.f;
            (*matte)[i] = std::max(0.f, std::min(1.f, inv ? 1.f - a : a));
        }
        return true;
    }
    Frame m;
    if (!fx->getInput(matteInput) || !fetchFrame(fx, matteInput, time, scale, view, src.bounds, 4, &m)) {
        matte->clear();
        return false;
    }
    const bool lum = mode == eMatteMatteLuminance || mode == eMatteMatteInvertedLuminance;
    const bool inv = mode == eMatteMatteInvertedLuminance || mode == eMatteMatteInvertedAlpha;
    for (size_t i = 0; i < n; ++i) {
        const float* p = &m.px[i * 4];
        float v = lum ? 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2] : p[3];
        v = std::max(0.f, std::min(1.f, v));
        (*matte)[i] = inv ? 1.f - v : v;
    }
    return true;
}

namespace {

// Field restricted to the pixels where keep[] is set, filled elsewhere from
// those pixels (the foreground's own motion carried over the background
// and vice versa).
void
restrictField(const flow::Field& f, const std::vector<float>& m, bool fg, flow::Field* out)
{
    *out = f;
    const int w = f.w, h = f.h;
    std::vector<unsigned char> ok((size_t)w * h);
    size_t holes = 0;
    for (size_t i = 0; i < ok.size(); ++i) {
        ok[i] = (fg ? m[i] >= 0.5f : m[i] < 0.5f) ? 1 : 0;
        holes += ok[i] ? 0 : 1;
    }
    if (holes == ok.size()) {
        return;   // nothing of this layer: keep the global motion
    }
    for (int pass = 0; holes > 0 && pass < 512; ++pass) {
        std::vector<unsigned char> next = ok;
        size_t fixedNow = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                if (ok[i]) {
                    continue;
                }
                float su = 0.f, sv = 0.f;
                int n = 0;
                const int nx[4] = { x - 1, x + 1, x, x };
                const int ny[4] = { y, y, y - 1, y + 1 };
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) {
                        continue;
                    }
                    const size_t q = (size_t)ny[k] * w + nx[k];
                    if (ok[q]) {
                        su += out->u[q];
                        sv += out->v[q];
                        ++n;
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
        ok.swap(next);
        if (!fixedNow) {
            break;
        }
        holes -= std::min(holes, fixedNow);
    }
}

} // anonymous namespace

void
interpolateFrames(const Frame& A, const Frame& B, const flow::Field& fAB, const flow::Field& fBA,
                  const std::vector<float>& mA, const std::vector<float>& mB,
                  float a, flow::FilterEnum filter, Frame* out)
{
    out->bounds = A.bounds;
    out->nc = A.nc;
    out->px.assign(A.px.size(), 0.f);
    const int w = A.w(), h = A.h();
    if (mA.empty() || mB.empty() || mA.size() != (size_t)w * h || mB.size() != mA.size()) {
        flow::interpolate(A.px.data(), B.px.data(), w, h, A.nc, fAB, fBA, a, filter, out->px.data());
        return;
    }
    flow::Field fgAB, fgBA, bgAB, bgBA;
    restrictField(fAB, mA, true, &fgAB);
    restrictField(fBA, mB, true, &fgBA);
    restrictField(fAB, mA, false, &bgAB);
    restrictField(fBA, mB, false, &bgBA);
    std::vector<float> fg(A.px.size()), bg(A.px.size()), m((size_t)w * h);
    flow::interpolate(A.px.data(), B.px.data(), w, h, A.nc, fgAB, fgBA, a, filter, fg.data());
    flow::interpolate(A.px.data(), B.px.data(), w, h, A.nc, bgAB, bgBA, a, filter, bg.data());
    flow::interpolate(mA.data(), mB.data(), w, h, 1, fgAB, fgBA, a, flow::eFilterBilinear, m.data());
    const int nc = A.nc;
    for (size_t i = 0; i < m.size(); ++i) {
        const float t = std::max(0.f, std::min(1.f, m[i]));
        for (int c = 0; c < nc; ++c) {
            out->px[i * nc + c] = fg[i * nc + c] * t + bg[i * nc + c] * (1.f - t);
        }
    }
}

std::vector<ChoiceOption>
filterChoices()
{
    std::vector<ChoiceOption> o;
    o.push_back(ChoiceOption("nearest", "Nearest", "Fastest, blocky."));
    o.push_back(ChoiceOption("bilinear", "Bilinear", "Default: smooth, slightly soft."));
    o.push_back(ChoiceOption("cubic", "Cubic", "Sharper (Keys cubic), may ring on hard edges."));
    return o;
}

std::vector<ChoiceOption>
motionChoices()
{
    std::vector<ChoiceOption> o;
    o.push_back(ChoiceOption("local", "Local", "Vectors follow small local motions (less smoothing), "
                                               "may break up on smooth areas."));
    o.push_back(ChoiceOption("regularized", "Regularized", "Smoother, more coherent vectors (default)."));
    return o;
}

void
addComputeInfo(EffectInstance* fx, const KnobPagePtr& page)
{
    KnobStringPtr k = AppManager::createKnob<KnobString>( fx, fx->tr("Local GPU") );
    k->setName("localGPU");
    k->setAsLabel();
    k->setDefaultValue("CPU (multi-threaded) - no GPU path in this build");
    k->setEvaluateOnChange(false);
    k->setIsPersistent(false);
    page->addKnob(k);
    KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>( fx, std::string() );
    page->addKnob(sep);
}

void
writeFrame(const Frame& f, const ImagePtr& out)
{
    if (!out || !f.valid()) {
        return;
    }
    const RectI b = out->getBounds();
    const int onc = out->getComponents().getNumComponents();
    Image::WriteAccess wa( out.get() );
    for (int y = b.y1; y < b.y2; ++y) {
        for (int x = b.x1; x < b.x2; ++x) {
            float* d = (float*)wa.pixelAt(x, y);
            if (!d) {
                continue;
            }
            const int xx = std::max(f.bounds.x1, std::min(f.bounds.x2 - 1, x));
            const int yy = std::max(f.bounds.y1, std::min(f.bounds.y2 - 1, y));
            const bool inside = x == xx && y == yy;
            const float* s = f.at(xx, yy);
            if (onc == 1) {
                d[0] = inside ? (f.nc >= 4 ? s[3] : s[0]) : 0.f;
                continue;
            }
            for (int c = 0; c < onc; ++c) {
                d[c] = inside && c < f.nc ? s[c] : 0.f;
            }
        }
    }
}

FramesNeededMap
framesRange(EffectInstance* fx, const std::vector<int>& inputs, double first, double last, ViewIdx view)
{
    FramesNeededMap ret;
    RangeD r;
    r.min = std::min(first, last);
    r.max = std::max(first, last);
    std::vector<RangeD> ranges(1, r);
    FrameRangesMap m;
    m.insert( std::make_pair(view, ranges) );
    for (size_t i = 0; i < inputs.size(); ++i) {
        if ( fx->getInput(inputs[i]) ) {
            ret.insert( std::make_pair(inputs[i], m) );
        }
    }
    return ret;
}

} // namespace motion

NATRON_NAMESPACE_EXIT
