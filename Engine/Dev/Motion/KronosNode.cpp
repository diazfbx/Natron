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

namespace {

enum MethodEnum { eMethodFrame = 0, eMethodBlend, eMethodMotion };
enum TimingEnum { eTimingOutputSpeed = 0, eTimingFrame };
enum ChannelsEnum { eChannelsAll = 0, eChannelsRGBA, eChannelsRGB, eChannelsAlpha };
enum OutputEnum { eOutputResult = 0, eOutputSource, eOutputVectors };

} // anonymous namespace

struct KronosNodePrivate
{
    KnobIntWPtr inputRange;
    KnobButtonWPtr resetRange;
    KnobChoiceWPtr channels;
    KnobChoiceWPtr method;
    KnobChoiceWPtr timing;
    KnobDoubleWPtr outputSpeed;
    KnobDoubleWPtr frame;
    KnobChoiceWPtr motion;        // Kronos
    KnobDoubleWPtr vectorDetail;
    KnobDoubleWPtr strength;      // Kronos "Strength" / OFlow "Smoothness"
    KnobChoiceWPtr filter;
    KnobIntWPtr shutterSamples;
    KnobDoubleWPtr shutterTime;
    KnobBoolWPtr autoShutter;
    KnobChoiceWPtr output;
    KnobChoiceWPtr matte;
};

KronosNode::KronosNode(NodePtr node)
    : EffectInstance(node)
    , _imp(new KronosNodePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

KronosNode::~KronosNode()
{
}

std::string
KronosNode::getPluginDescription() const
{
    return "Retimes footage with motion-estimated in-between frames (like NukeX's Kronos).\n\n"
           "Timing: Output Speed (0.5 = half speed slow motion, 2 = twice as fast, from the start of "
           "Input Range) or Frame (animate the source frame shown at each output frame). "
           "Method: Motion builds new frames along the motion vectors, Blend mixes the two nearest "
           "frames, Frame takes the nearest one.\n\n"
           "Vector Detail: density of the vectors (proportion of the image size). Strength: how "
           "strongly the vectors are smoothed. Shutter: adds motion blur over the given shutter time "
           "(Automatic Shutter Time follows the speed). Matte: foreground matte so a moving object and "
           "the background keep their own motion.";
}

std::string
OFlowNode::getPluginDescription() const
{
    return "Retimes footage with optical flow (like Nuke's OFlow, the predecessor of Kronos).\n\n"
           "Timing: Output Speed or Frame. Method: Motion, Blend or Frame. Vector Detail: density of "
           "the vectors; Smoothness: how smooth the vector field is (higher = fewer artefacts on "
           "complex motion, less detail). Shutter: optional motion blur.";
}

void
KronosNode::addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
    (void)inputNb;
}

void
KronosNode::initializeKnobs()
{
    const bool oflow = isOFlow();
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, oflow ? tr("OFlow") : tr("Kronos") );
    addComputeInfo(this, page);
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("Input Range"), 2);
        k->setName("input");
        k->setDefaultValue(1, 0);
        k->setDefaultValue(100, 1);
        k->setDimensionName(0, "first");
        k->setDimensionName(1, "last");
        k->setAddNewLine(false);
        k->setHintToolTip( tr("Source frames used for the retime. Output Speed counts from the first one.") );
        page->addKnob(k);
        _imp->inputRange = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>( this, tr("Reset") );
        k->setName("resetInputRange");
        k->setHintToolTip( tr("Set Input Range to the frame range of the source.") );
        page->addKnob(k);
        _imp->resetRange = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Channels") );
        k->setName("retimedChannels");
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("all", "all", "Retime every channel.") );
        o.push_back( ChoiceOption("rgba", "rgba", "") );
        o.push_back( ChoiceOption("rgb", "rgb", "Alpha taken from the nearest source frame.") );
        o.push_back( ChoiceOption("alpha", "alpha", "RGB taken from the nearest source frame.") );
        k->populateChoices(o);
        k->setDefaultValue(0);
        page->addKnob(k);
        _imp->channels = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Method") );
        k->setName("retimeMethod");
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("frame", "Frame", "Nearest source frame (no new images).") );
        o.push_back( ChoiceOption("blend", "Blend", "Mix of the two nearest source frames.") );
        o.push_back( ChoiceOption("motion", "Motion", "New in-between images along the motion vectors.") );
        k->populateChoices(o);
        k->setDefaultValue(eMethodMotion);
        page->addKnob(k);
        _imp->method = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Timing") );
        k->setName("timing2");
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("speed", "Output Speed", "Constant (or animated) speed factor.") );
        o.push_back( ChoiceOption("frame", "Frame", "Animate the source frame to show at each output frame.") );
        k->populateChoices(o);
        k->setDefaultValue(eTimingOutputSpeed);
        page->addKnob(k);
        _imp->timing = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Output Speed") );
        k->setName("timingOutputSpeed");
        k->setDefaultValue(0.5);
        k->setMinimum(0.0001);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(5.);
        k->setHintToolTip( tr("0.5 = half speed (twice as many frames), 2 = twice as fast.") );
        page->addKnob(k);
        _imp->outputSpeed = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Frame") );
        k->setName("timingFrame2");
        k->setDefaultValue(1.);
        k->setHintToolTip( tr("Source frame shown at the current output frame (animate it).") );
        page->addKnob(k);
        _imp->frame = k;
    }
    if (!oflow) {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Motion") );
        k->setName("motionEstimation");
        k->populateChoices( motionChoices() );
        k->setDefaultValue(1);
        page->addKnob(k);
        _imp->motion = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Vector Detail") );
        k->setName("vectorDetail");
        k->setDefaultValue(oflow ? 0.2 : 0.3);
        k->setMinimum(0.01);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.01);
        k->setDisplayMaximum(1.);
        k->setHintToolTip( tr("Density of the motion vectors, as a proportion of the image size.") );
        page->addKnob(k);
        _imp->vectorDetail = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, oflow ? tr("Smoothness") : tr("Strength") );
        k->setName(oflow ? "smoothness" : "strength");
        k->setDefaultValue(oflow ? 0.5 : 1.5);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(oflow ? 1. : 3.);
        k->setHintToolTip( oflow ? tr("Smoothness of the vector field.")
                                 : tr("How strongly the vectors are smoothed (higher: steadier vectors, less detail).") );
        page->addKnob(k);
        _imp->strength = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Filter") );
        k->setName("resampleType");
        k->populateChoices( filterChoices() );
        k->setDefaultValue(1);
        page->addKnob(k);
        _imp->filter = k;
    }
    {
        KnobGroupPtr g = AppManager::createKnob<KnobGroup>( this, tr("Shutter") );
        g->setName("shutterGroup");
        g->setDefaultValue(false);
        page->addKnob(g);
        {
            KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Shutter Samples") );
            k->setName("shutterSamples");
            k->setDefaultValue(1);
            k->setMinimum(1);
            k->setDisplayMinimum(1);
            k->setDisplayMaximum(32);
            g->addKnob(k);
            _imp->shutterSamples = k;
        }
        {
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Shutter Time") );
            k->setName("shutterTime");
            k->setDefaultValue(0.);
            k->setMinimum(0.);
            k->setDisplayMinimum(0.);
            k->setDisplayMaximum(4.);
            k->setHintToolTip( tr("Shutter length in output frames (0 = no motion blur).") );
            g->addKnob(k);
            _imp->shutterTime = k;
        }
        {
            KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Automatic Shutter Time") );
            k->setName("autoShutterTime");
            k->setDefaultValue(false);
            k->setHintToolTip( tr("Shutter time follows the speed (fast retimes get the blur of their speed).") );
            g->addKnob(k);
            _imp->autoShutter = k;
        }
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Output") );
        k->setName("output");
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("result", "Result", "The retimed image.") );
        o.push_back( ChoiceOption("source", "Source", "Nearest source frame (check the timing).") );
        o.push_back( ChoiceOption("vectors", "Vectors", "Motion vectors to the next source frame in R, G (pixels).") );
        k->populateChoices(o);
        k->setDefaultValue(eOutputResult);
        page->addKnob(k);
        _imp->output = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Matte") );
        k->setName("matteChannel");
        k->populateChoices( matteChoices() );
        k->setDefaultValue(0);
        page->addKnob(k);
        _imp->matte = k;
    }
    refreshVisibility();
}

void
KronosNode::refreshVisibility()
{
    KnobChoicePtr timing = _imp->timing.lock();
    if (!timing) {
        return;
    }
    const bool frameTiming = timing->getValue() == eTimingFrame;
    _imp->outputSpeed.lock()->setSecret(frameTiming);
    _imp->frame.lock()->setSecret(!frameTiming);
    const bool motion = _imp->method.lock()->getValue() == eMethodMotion;
    if ( _imp->motion.lock() ) {
        _imp->motion.lock()->setSecret(!motion);
    }
    _imp->vectorDetail.lock()->setSecret(!motion);
    _imp->strength.lock()->setSecret(!motion);
}

void
KronosNode::onKnobsLoaded()
{
    refreshVisibility();
}

void
KronosNode::onInputChanged(int inputNo)
{
    // A new source with Input Range still at its default: take the source's range.
    KnobIntPtr r = _imp->inputRange.lock();
    if (inputNo != 0 || !r || !getInput(0)) {
        return;
    }
    if ( r->getValue(0) == 1 && r->getValue(1) == 100 ) {
        double first, last;
        inputFrameRange(this, 0, &first, &last);
        if (last > first) {
            r->setValue( (int)first, ViewSpec::all(), 0 );
            r->setValue( (int)last, ViewSpec::all(), 1 );
        }
    }
}

bool
KronosNode::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/,
                        bool /*originatedFromMainThread*/)
{
    if ( k == _imp->resetRange.lock().get() ) {
        double first, last;
        inputFrameRange(this, 0, &first, &last);
        KnobIntPtr r = _imp->inputRange.lock();
        r->setValue( (int)first, ViewSpec::all(), 0 );
        r->setValue( (int)last, ViewSpec::all(), 1 );
        return true;
    }
    if ( k == _imp->timing.lock().get() || k == _imp->method.lock().get() ) {
        refreshVisibility();
        return true;
    }
    return false;
}

double
KronosNode::sourceTime(double t) const
{
    KnobIntPtr r = _imp->inputRange.lock();
    const double first = r->getValue(0), last = r->getValue(1);
    double s;
    if (_imp->timing.lock()->getValue() == eTimingFrame) {
        s = _imp->frame.lock()->getValueAtTime(t);
    } else {
        // integrate the (possibly animated) speed from the first frame
        KnobDoublePtr sp = _imp->outputSpeed.lock();
        if ( sp->isAnimated(0) ) {
            s = first;
            const double step = t >= first ? 1. : -1.;
            for (double f = first; step > 0 ? f < t - 1e-9 : f > t + 1e-9; f += step) {
                const double d = std::min(1., std::fabs(t - f));
                s += step * d * sp->getValueAtTime(f + 0.5 * step * d);
            }
        } else {
            s = first + (t - first) * sp->getValue();
        }
    }
    return std::max( std::min(first, last), std::min( std::max(first, last), s ) );
}

void
KronosNode::getFrameRange(double *first, double *last)
{
    KnobIntPtr r = _imp->inputRange.lock();
    const double f0 = r->getValue(0), f1 = r->getValue(1);
    if (_imp->timing.lock()->getValue() == eTimingFrame) {
        *first = f0;
        *last = f1;
        return;
    }
    KnobDoublePtr sp = _imp->outputSpeed.lock();
    const double speed = std::max(1e-4, sp->getValue());
    *first = f0;
    *last = f0 + std::ceil( (f1 - f0) / speed );
}

FramesNeededMap
KronosNode::getFramesNeeded(double time, ViewIdx view)
{
    const double s = sourceTime(time);
    const double speed = _imp->timing.lock()->getValue() == eTimingFrame ? 1. : std::fabs( _imp->outputSpeed.lock()->getValueAtTime(time) );
    double shutter = _imp->shutterTime.lock()->getValueAtTime(time);
    if ( _imp->autoShutter.lock()->getValueAtTime(time) ) {
        shutter = speed;
    }
    const double ext = 0.5 * shutter * speed;
    std::vector<int> ins(1, 0);
    ins.push_back(1);
    return framesRange(this, ins, std::floor(s - ext), std::ceil(s + ext), view);
}

StatusEnum
KronosNode::render(const RenderActionArgs& args)
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
    const double s = sourceTime(t);
    KnobIntPtr r = _imp->inputRange.lock();
    const double first = std::min( r->getValue(0), r->getValue(1) ), last = std::max( r->getValue(0), r->getValue(1) );
    const int method = _imp->method.lock()->getValueAtTime(t);
    const int channels = _imp->channels.lock()->getValueAtTime(t);
    const int output = _imp->output.lock()->getValueAtTime(t);
    const int matteMode = _imp->matte.lock()->getValueAtTime(t);
    const flow::FilterEnum filter = (flow::FilterEnum)_imp->filter.lock()->getValueAtTime(t);
    flow::Params fp;
    fp.detail = _imp->vectorDetail.lock()->getValueAtTime(t);
    fp.useGpu = useGpu(this);
    fp.local = _imp->motion.lock() ? _imp->motion.lock()->getValueAtTime(t) == 0 : false;
    fp.smoothness = isOFlow() ? _imp->strength.lock()->getValueAtTime(t)
                              : _imp->strength.lock()->getValueAtTime(t) / 3.;
    const double speed = _imp->timing.lock()->getValue() == eTimingFrame ? 1. : std::fabs( _imp->outputSpeed.lock()->getValueAtTime(t) );
    int N = std::max(1, _imp->shutterSamples.lock()->getValueAtTime(t));
    double shutter = _imp->shutterTime.lock()->getValueAtTime(t);
    if ( _imp->autoShutter.lock()->getValueAtTime(t) ) {
        shutter = speed;
    }
    if (shutter <= 0.) {
        N = 1;
    }

    std::map<int, Frame> frames;
    std::map<int, std::vector<float> > mattes;
    auto getFrame = [&](int f) -> const Frame* {
        f = (int)std::max( first, std::min(last, (double)f) );
        std::map<int, Frame>::iterator it = frames.find(f);
        if ( it != frames.end() ) {
            return &it->second;
        }
        Frame& fr = frames[f];
        if ( !fetchFrame(this, 0, f, args.mappedScale, args.view, R, 4, &fr) ) {
            frames.erase(f);
            return NULL;
        }
        if (matteMode != eMatteNone) {
            fetchMatte(this, 1, matteMode, fr, f, args.mappedScale, args.view, &mattes[f]);
        }
        return &fr;
    };
    const std::string tag = isOFlow() ? "oflow" : "kronos";

    // Image at source time st.
    Frame tmp;
    auto imageAt = [&](double st, Frame* dst) -> bool {
        st = std::max( first, std::min(last, st) );
        const int i0 = (int)std::floor(st + 1e-6);
        const double a = st - i0;
        const Frame* f0 = getFrame(i0);
        if (!f0) {
            return false;
        }
        if (a < 1e-4 || i0 + 1 > last || method == eMethodFrame || output == eOutputSource) {
            const Frame* fn = (method == eMethodFrame || output == eOutputSource) && a >= 0.5 && i0 + 1 <= last ? getFrame(i0 + 1) : f0;
            *dst = fn ? *fn : *f0;
            return true;
        }
        const Frame* f1 = getFrame(i0 + 1);
        if (!f1) {
            *dst = *f0;
            return true;
        }
        if (method == eMethodBlend) {
            *dst = *f0;
            for (size_t i = 0; i < dst->px.size(); ++i) {
                dst->px[i] = (float)( (1. - a) * f0->px[i] + a * f1->px[i] );
            }
            return true;
        }
        flow::FieldPtr fwdRaw = getFlow(this, 0, i0, i0 + 1, args.mappedScale, args.view, R, fp, tag, f0, f1);
        flow::FieldPtr bwdRaw = getFlow(this, 0, i0 + 1, i0, args.mappedScale, args.view, R, fp, tag, f1, f0);
        if (!fwdRaw || !bwdRaw) {
            return false;
        }
        // drop absurd vectors (the occlusion test of the interpolation still
        // needs the raw forward / backward disagreement, so only spikes go)
        std::shared_ptr<flow::Field> fwdC = std::make_shared<flow::Field>(), bwdC = std::make_shared<flow::Field>();
        flow::cleanFlow(*fwdRaw, NULL, fwdC.get());
        flow::cleanFlow(*bwdRaw, NULL, bwdC.get());
        flow::FieldPtr fwd = fwdC, bwd = bwdC;
        static const std::vector<float> none;
        const std::vector<float>& m0 = matteMode != eMatteNone ? mattes[i0] : none;
        const std::vector<float>& m1 = matteMode != eMatteNone ? mattes[i0 + 1] : none;
        interpolateFrames(*f0, *f1, *fwd, *bwd, m0, m1, (float)a, filter, dst);
        return true;
    };

    if (output == eOutputVectors) {
        const int i0 = (int)std::floor(s + 1e-6);
        const Frame* f0 = getFrame(i0);
        const Frame* f1 = getFrame(i0 + 1);
        if (!f0 || !f1) {
            return eStatusFailed;
        }
        flow::FieldPtr fwd = getFlow(this, 0, i0, i0 + 1, args.mappedScale, args.view, R, fp, tag, f0, f1);
        if (!fwd) {
            return eStatusFailed;
        }
        Frame v;
        v.bounds = R;
        v.nc = 4;
        v.px.assign(f0->px.size(), 0.f);
        for (size_t i = 0; i < fwd->u.size(); ++i) {
            v.px[i * 4] = fwd->u[i];
            v.px[i * 4 + 1] = fwd->v[i];
            v.px[i * 4 + 3] = 1.f;
        }
        writeFrame(v, outImg);
        return eStatusOK;
    }

    Frame result;
    if (N == 1) {
        if ( !imageAt(s, &result) ) {
            return eStatusFailed;
        }
    } else {
        // motion blur: samples over the shutter (in output frames, i.e. speed x source frames)
        for (int k = 0; k < N; ++k) {
            const double st = s + ( (double)k / (double)(N - 1) - 0.5 ) * shutter * speed;
            if ( !imageAt(st, &tmp) ) {
                return eStatusFailed;
            }
            if (k == 0) {
                result = tmp;
            } else {
                for (size_t i = 0; i < result.px.size(); ++i) {
                    result.px[i] += tmp.px[i];
                }
            }
            if ( isAborted(this) ) {
                return eStatusFailed;
            }
        }
        const float inv = 1.f / (float)N;
        for (size_t i = 0; i < result.px.size(); ++i) {
            result.px[i] *= inv;
        }
    }

    // Channels not retimed: from the nearest source frame.
    if (channels == eChannelsRGB || channels == eChannelsAlpha) {
        const Frame* nearest = getFrame( (int)std::floor(s + 0.5) );
        if (nearest) {
            for (size_t i = 0; i < result.px.size() / 4; ++i) {
                if (channels == eChannelsRGB) {
                    result.px[i * 4 + 3] = nearest->px[i * 4 + 3];
                } else {
                    for (int c = 0; c < 3; ++c) {
                        result.px[i * 4 + c] = nearest->px[i * 4 + c];
                    }
                }
            }
        }
    }
    writeFrame(result, outImg);
    return isAborted(this) ? eStatusFailed : eStatusOK;
}

NATRON_NAMESPACE_EXIT
