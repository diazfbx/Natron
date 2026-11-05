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

#include "MotionNodes.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"

#include "MotionCommon.h"

NATRON_NAMESPACE_ENTER

using namespace motion;

enum MBMethodEnum
{
    eMBMethodVectorBlur = 0,
    eMBMethodInterpolation,
};

struct MotionBlurNodePrivate
{
    KnobChoiceWPtr method;
    KnobIntWPtr shutterSamples;
    KnobDoubleWPtr shutterTime;
    KnobChoiceWPtr motion;
    KnobDoubleWPtr vectorDetail;
    KnobChoiceWPtr filter;
    KnobChoiceWPtr matte;
};

MotionBlurNode::MotionBlurNode(NodePtr node)
    : EffectInstance(node)
    , _imp(new MotionBlurNodePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

MotionBlurNode::~MotionBlurNode()
{
}

std::string
MotionBlurNode::getPluginDescription() const
{
    return "Adds motion blur to moving footage, from the image's own motion (like NukeX's MotionBlur).\n\n"
           "Motion vectors are computed between the frame and its neighbours (optical flow), "
           "the image is then blurred along them over the shutter.\n\n"
           "Method: Vector Blur (default) smears the current frame along its own cleaned vectors and "
           "decides which surface passes in front from the occlusions (or the Matte): robust on noisy, "
           "dark or flat areas. Frame Interpolation builds in-between images from the neighbouring "
           "frames (Kronos) and averages them: sharper occlusions, but wrong vectors show as ghosts.\n\n"
           "Shutter Samples: number of in-between images (raise it if the blur shows steps). "
           "Shutter Time: shutter length in frames (0.5 = 180 degrees), centred on the frame. "
           "Vector Detail: resolution of the vector field (proportion of the image size: 1 = one vector "
           "per pixel, slower). Matte: separates a foreground whose motion differs from the background "
           "(from the source alpha or the Matte input) so neither drags the other.";
}

void
MotionBlurNode::addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    if (inputNb == 1) {
        comps->push_back( ImagePlaneDesc::getAlphaComponents() );
    }
}

void
MotionBlurNode::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, tr("MotionBlur") );
    addComputeInfo(this, page);
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Method") );
        k->setName("mbMethod");
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("vectorBlur", "Vector Blur", "Blur the current frame along its own (cleaned) motion vectors. Robust, no ghosts.") );
        o.push_back( ChoiceOption("interpolation", "Frame Interpolation", "Average in-between images built from the neighbouring frames (Kronos).") );
        k->populateChoices(o);
        k->setDefaultValue(0);
        page->addKnob(k);
        _imp->method = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Shutter Samples") );
        k->setName("shutterSamples");
        k->setDefaultValue(3);
        k->setMinimum(1);
        k->setDisplayMinimum(1);
        k->setDisplayMaximum(32);
        k->setHintToolTip( tr("Number of in-between images averaged over the shutter. "
                              "Raise it when the blur shows distinct copies.") );
        page->addKnob(k);
        _imp->shutterSamples = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Shutter Time") );
        k->setName("shutterTime");
        k->setDefaultValue(0.5);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(10.);
        k->setHintToolTip( tr("Shutter length in frames, centred on the current frame "
                              "(0.5 = 180 degree shutter, 1 = a whole frame).") );
        page->addKnob(k);
        _imp->shutterTime = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Motion") );
        k->setName("motion");
        k->populateChoices( motionChoices() );
        k->setDefaultValue(1);
        k->setHintToolTip( tr("Regularized: smooth, coherent vectors. Local: follows small local motions more closely.") );
        page->addKnob(k);
        _imp->motion = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Vector Detail") );
        k->setName("vectorDetail");
        k->setDefaultValue(0.3);
        k->setMinimum(0.01);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.01);
        k->setDisplayMaximum(1.);
        k->setHintToolTip( tr("Density of the motion vectors, as a proportion of the image size "
                              "(1 = one vector per pixel: most accurate, slowest).") );
        page->addKnob(k);
        _imp->vectorDetail = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Filter") );
        k->setName("filter");
        k->populateChoices( filterChoices() );
        k->setDefaultValue(1);
        page->addKnob(k);
        _imp->filter = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Matte") );
        k->setName("matteChannel");
        k->populateChoices( matteChoices() );
        k->setDefaultValue(0);
        k->setHintToolTip( tr("Foreground matte: the foreground and the background get their own motion, "
                              "so a moving object doesn't drag the background with it.") );
        page->addKnob(k);
        _imp->matte = k;
    }
}

FramesNeededMap
MotionBlurNode::getFramesNeeded(double time, ViewIdx view)
{
    std::vector<int> ins(1, 0);
    ins.push_back(1);
    return framesRange(this, ins, std::floor(time) - 1, std::floor(time) + 1, view);
}

StatusEnum
MotionBlurNode::render(const RenderActionArgs& args)
{
    if ( args.outputPlanes.empty() ) {
        return eStatusFailed;
    }
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) {
        return eStatusFailed;
    }
    const RectI R = outImg->getBounds();
    const double t = args.time;
    const int N = std::max(1, _imp->shutterSamples.lock()->getValueAtTime(t));
    const double S = std::max(0., _imp->shutterTime.lock()->getValueAtTime(t));
    flow::Params fp;
    fp.detail = _imp->vectorDetail.lock()->getValueAtTime(t);
    fp.local = _imp->motion.lock()->getValueAtTime(t) == 0;
    fp.smoothness = 0.5;
    const flow::FilterEnum filter = (flow::FilterEnum)_imp->filter.lock()->getValueAtTime(t);
    const int matteMode = _imp->matte.lock()->getValueAtTime(t);

    Frame cur;
    if ( !fetchFrame(this, 0, t, args.mappedScale, args.view, R, 4, &cur) ) {
        return eStatusFailed;
    }
    if (N == 1 || S <= 0.) {
        writeFrame(cur, outImg);
        return eStatusOK;
    }
    double first, last;
    inputFrameRange(this, 0, &first, &last);
    const double tPrev = t - 1, tNext = t + 1;
    const bool havePrev = tPrev >= first, haveNext = tNext <= last;

    Frame prev, next;
    if ( havePrev && !fetchFrame(this, 0, tPrev, args.mappedScale, args.view, R, 4, &prev) ) {
        return eStatusFailed;
    }
    if ( haveNext && !fetchFrame(this, 0, tNext, args.mappedScale, args.view, R, 4, &next) ) {
        return eStatusFailed;
    }
    std::vector<float> mCur, mPrev, mNext;
    if (matteMode != eMatteNone) {
        fetchMatte(this, 1, matteMode, cur, t, args.mappedScale, args.view, &mCur);
        if (havePrev) {
            fetchMatte(this, 1, matteMode, prev, tPrev, args.mappedScale, args.view, &mPrev);
        }
        if (haveNext) {
            fetchMatte(this, 1, matteMode, next, tNext, args.mappedScale, args.view, &mNext);
        }
    }
    const std::string tag = "src";
    flow::FieldPtr fCN, fNC, fPC, fCP;   // cur->next, next->cur, prev->cur, cur->prev
    if (haveNext) {
        fCN = getFlow(this, 0, t, tNext, args.mappedScale, args.view, R, fp, tag, &cur, &next);
        fNC = getFlow(this, 0, tNext, t, args.mappedScale, args.view, R, fp, tag, &next, &cur);
    }
    if ( isAborted(this) ) {
        return eStatusFailed;
    }
    if (havePrev) {
        fPC = getFlow(this, 0, tPrev, t, args.mappedScale, args.view, R, fp, tag, &prev, &cur);
        fCP = getFlow(this, 0, t, tPrev, args.mappedScale, args.view, R, fp, tag, &cur, &prev);
    }
    if ( isAborted(this) ) {
        return eStatusFailed;
    }

    const int method = _imp->method.lock()->getValueAtTime(t);
    if (method == eMBMethodVectorBlur) {
        // cleaned vectors of the current frame + where they fail (occlusions)
        flow::Field cF, cB;
        std::vector<float> kF, kB;
        if (fCN) {
            flow::cleanFlow(*fCN, fNC.get(), &cF, &kF);
        }
        if (fCP) {
            flow::cleanFlow(*fCP, fPC.get(), &cB, &kB);
        }
        if ( !cF.valid() && !cB.valid() ) {
            writeFrame(cur, outImg);
            return eStatusOK;
        }
        if ( isAborted(this) ) {
            return eStatusFailed;
        }
        Frame res;
        res.bounds = R;
        res.nc = 4;
        res.px.assign(cur.px.size(), 0.f);
        const bool useMatte = !mCur.empty() && mCur.size() == (size_t)cur.w() * cur.h();
        flow::motionBlurGather(cur.px.data(), cur.w(), cur.h(), 4,
                               cF.valid() ? &cF : NULL, cB.valid() ? &cB : NULL,
                               useMatte ? mCur.data() : NULL,
                               kF.empty() ? NULL : &kF, kB.empty() ? NULL : &kB,
                               (float)S, N, filter, res.px.data());
        writeFrame(res, outImg);
        return eStatusOK;
    }

    // Frame Interpolation: absurd vectors removed first
    auto spikeFree = [](const flow::FieldPtr& f) -> flow::FieldPtr {
        if (!f) {
            return f;
        }
        std::shared_ptr<flow::Field> c = std::make_shared<flow::Field>();
        flow::cleanFlow(*f, NULL, c.get());
        return c;
    };
    fCN = spikeFree(fCN);
    fNC = spikeFree(fNC);
    fPC = spikeFree(fPC);
    fCP = spikeFree(fCP);

    Frame acc;
    acc.bounds = R;
    acc.nc = 4;
    acc.px.assign(cur.px.size(), 0.f);
    int used = 0;
    Frame tmp;
    for (int k = 0; k < N; ++k) {
        const double tau = ( (double)k / (double)(N - 1) - 0.5 ) * S;
        const Frame* img = &cur;
        if (tau > 1e-6 && fCN && fNC) {
            interpolateFrames(cur, next, *fCN, *fNC, mCur, mNext, (float)std::min(1., tau), filter, &tmp);
            img = &tmp;
        } else if (tau < -1e-6 && fPC && fCP) {
            interpolateFrames(prev, cur, *fPC, *fCP, mPrev, mCur, (float)std::max(0., 1. + tau), filter, &tmp);
            img = &tmp;
        }
        for (size_t i = 0; i < acc.px.size(); ++i) {
            acc.px[i] += img->px[i];
        }
        ++used;
        if ( isAborted(this) ) {
            return eStatusFailed;
        }
    }
    const float inv = 1.f / (float)std::max(1, used);
    for (size_t i = 0; i < acc.px.size(); ++i) {
        acc.px[i] *= inv;
    }
    writeFrame(acc, outImg);
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
