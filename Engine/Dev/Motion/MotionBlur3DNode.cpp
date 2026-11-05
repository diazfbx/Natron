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
#include <cctype>
#include <cmath>
#include <mutex>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"

#include "../DotUtils.h"
#include "../Scene3D/CameraMath.h"
#include "../Scene3D/CameraProvider.h"
#include "../Scene3D/RotationConventions.h"
#include "MotionCommon.h"

NATRON_NAMESPACE_ENTER

using namespace motion;

namespace {

enum OutputUVEnum { eOutUVForward = 0, eOutUVBackward, eOutUVRGB };
enum DepthConvEnum { eDepthFarZero = 0, eDepthFarInfinity };   // far=0: 1/z (Nuke), far=infinity: z
enum ShutterOffsetEnum { eShutterStart = 0, eShutterCentred, eShutterEnd, eShutterCustom };

struct DepthOption
{
    ImagePlaneDesc plane;
    int channel = 0;
    std::string id;
};

struct Cam
{
    double R[3][3];     // camera -> world rotation
    double T[3];        // camera position
    double tanH, tanV;  // tan(fov / 2)
};

bool
getCam(CameraProvider* cp, double time, Cam* c)
{
    double tx, ty, tz, rx, ry, rz;
    cp->getCameraPosition(time, tx, ty, tz, rx, ry, rz);
    RotationConventions::compose(rx, ry, rz, c->R);
    c->T[0] = tx;
    c->T[1] = ty;
    c->T[2] = tz;
    double fh, fv;
    CameraMath::computeFovs(cp->getCameraFocalLength(time), cp->getCameraHAperture(time),
                            cp->getCameraVAperture(time), fh, fv);
    c->tanH = std::tan(0.5 * fh);
    c->tanV = std::tan(0.5 * fv);
    return c->tanH > 0. && c->tanV > 0.;
}

// NDC (-1..1) + distance along the view axis -> world
void
unproject(const Cam& c, double nx, double ny, double z, double P[3])
{
    const double pc[3] = { nx * c.tanH * z, ny * c.tanV * z, -z };
    for (int i = 0; i < 3; ++i) {
        P[i] = c.R[i][0] * pc[0] + c.R[i][1] * pc[1] + c.R[i][2] * pc[2] + c.T[i];
    }
}

// world -> NDC; false behind the camera
bool
project(const Cam& c, const double P[3], double* nx, double* ny)
{
    const double d[3] = { P[0] - c.T[0], P[1] - c.T[1], P[2] - c.T[2] };
    // camera space = R^T d
    const double x = c.R[0][0] * d[0] + c.R[1][0] * d[1] + c.R[2][0] * d[2];
    const double y = c.R[0][1] * d[0] + c.R[1][1] * d[1] + c.R[2][1] * d[2];
    const double z = c.R[0][2] * d[0] + c.R[1][2] * d[1] + c.R[2][2] * d[2];
    if (z > -1e-9) {
        return false;
    }
    *nx = x / (-z * c.tanH);
    *ny = y / (-z * c.tanV);
    return true;
}

} // anonymous namespace

struct MotionBlur3DNodePrivate
{
    KnobChoiceWPtr outputUV;
    KnobBoolWPtr useZ;
    KnobChoiceWPtr zChannel;
    KnobChoiceWPtr depthConv;
    KnobDoubleWPtr distance;
    KnobDoubleWPtr shutterLength;
    KnobChoiceWPtr shutterOffset;
    KnobDoubleWPtr shutterCustom;

    std::mutex optionsMutex;
    std::vector<DepthOption> depthOptions;
};

MotionBlur3DNode::MotionBlur3DNode(NodePtr node)
    : EffectInstance(node)
    , _imp(new MotionBlur3DNodePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

MotionBlur3DNode::~MotionBlur3DNode()
{
}

std::string
MotionBlur3DNode::getPluginDescription() const
{
    return "Motion vectors from the movement of a 3D camera (like Nuke's MotionBlur3D).\n\n"
           "Each pixel is placed in 3D with the Cam input and its depth (Z input, or a constant "
           "distance), then projected at the shutter open and close times: the difference is its "
           "motion over the shutter, in pixels. The vectors go to the forward (or backward) motion "
           "layer, or to R, G: plug a VectorBlur after it to blur (its UV Channels = the layer, or "
           "the Vectors input for R, G).\n\n"
           "Depth: far=0 for 1/z depth (Nuke, ScanlineRender), far=infinity for distance (most "
           "renderers' Z). Static objects in a camera move get the right blur; objects that move "
           "by themselves need their own vectors (render or MotionBlur).";
}

void
MotionBlur3DNode::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
}

void
MotionBlur3DNode::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, tr("MotionBlur3D") );
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Output UV") );
        k->setName("uv");
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("forward", "motion (forward)", "Natron's forward motion layer (u, v).") );
        o.push_back( ChoiceOption("backward", "motion (backward)", "Natron's backward motion layer (u, v).") );
        o.push_back( ChoiceOption("rgb", "rgb (u -> red, v -> green)", "In the colour channels, for VectorBlur's Vectors input.") );
        k->populateChoices(o);
        k->setDefaultValue(eOutUVForward);
        page->addKnob(k);
        _imp->outputUV = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>( this, tr("Z input") );
        k->setName("useZ");
        k->setDefaultValue(false);
        k->setAddNewLine(false);
        k->setHintToolTip( tr("Take each pixel's depth from a channel of the source.") );
        page->addKnob(k);
        _imp->useZ = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Z channel") );
        k->setName("Zchan");
        k->setLabel(std::string());
        std::vector<ChoiceOption> o(1, ChoiceOption("none", "none", ""));
        k->populateChoices(o);
        k->setAddNewLine(false);
        page->addKnob(k);
        _imp->zChannel = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Depth") );
        k->setName("depthConvention");
        k->setLabel(std::string());
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("far0", "far=0", "The channel holds 1/z (Nuke, ScanlineRender).") );
        o.push_back( ChoiceOption("farInf", "far=infinity", "The channel holds the distance z.") );
        k->populateChoices(o);
        k->setDefaultValue(eDepthFarZero);
        page->addKnob(k);
        _imp->depthConv = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("distance") );
        k->setName("distance");
        k->setDefaultValue(1.);
        k->setMinimum(1e-6);
        k->setDisplayMinimum(1.);
        k->setDisplayMaximum(1e7);
        k->setHintToolTip( tr("Distance of every pixel from the camera when Z input is off "
                              "(large = far away: only rotations blur).") );
        page->addKnob(k);
        _imp->distance = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Shutter Length") );
        k->setName("shutter");
        k->setDefaultValue(0.5);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(2.);
        k->setHintToolTip( tr("Shutter length in frames.") );
        page->addKnob(k);
        _imp->shutterLength = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Shutter Offset") );
        k->setName("shutteroffset");
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("start", "start", "The shutter opens at the frame.") );
        o.push_back( ChoiceOption("centred", "centred", "The shutter is centred on the frame.") );
        o.push_back( ChoiceOption("end", "end", "The shutter closes at the frame.") );
        o.push_back( ChoiceOption("custom", "custom", "Opens at the frame + the value next to it.") );
        k->populateChoices(o);
        k->setDefaultValue(eShutterStart);
        page->addKnob(k);
        _imp->shutterOffset = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Custom offset") );
        k->setName("shuttercustomoffset");
        k->setLabel(std::string());
        k->setDefaultValue(0.);
        k->setDisplayMinimum(-1.);
        k->setDisplayMaximum(1.);
        page->addKnob(k);
        _imp->shutterCustom = k;
    }
    refreshVisibility();
}

void
MotionBlur3DNode::refreshVisibility()
{
    if ( !_imp->useZ.lock() ) {
        return;
    }
    const bool z = _imp->useZ.lock()->getValue();
    _imp->zChannel.lock()->setAllDimensionsEnabled(z);
    _imp->depthConv.lock()->setAllDimensionsEnabled(z);
    _imp->distance.lock()->setAllDimensionsEnabled(!z);
    _imp->shutterCustom.lock()->setAllDimensionsEnabled(_imp->shutterOffset.lock()->getValue() == eShutterCustom);
}

void
MotionBlur3DNode::refreshDepthChoices()
{
    std::list<ImagePlaneDesc> layers;
    double time = 0;
    if ( getApp() && getApp()->getTimeLine() ) {
        time = getApp()->getTimeLine()->currentFrame();
    }
    if ( EffectInstancePtr in = getInput(0) ) {
        in->getAvailableLayers(time, ViewIdx(0), -1, &layers);
    }
    std::vector<DepthOption> opts;
    std::vector<ChoiceOption> entries;
    entries.push_back( ChoiceOption("none", "none", "") );
    int best = -1;
    for (std::list<ImagePlaneDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        const std::vector<std::string>& ch = it->getChannels();
        const std::string label = it->getPlaneLabel().empty() ? it->getPlaneID() : it->getPlaneLabel();
        for (size_t c = 0; c < ch.size(); ++c) {
            DepthOption d;
            d.plane = *it;
            d.channel = (int)c;
            d.id = label + "." + ch[c];
            std::string low = d.id;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            if ( best < 0 && (low.find("depth") != std::string::npos || low == "z" || low.find(".z") != std::string::npos) ) {
                best = (int)opts.size();
            }
            opts.push_back(d);
            entries.push_back( ChoiceOption(d.id, d.id, "") );
        }
    }
    KnobChoicePtr k = _imp->zChannel.lock();
    if (k) {
        const std::string prev = k->getActiveEntry().id;
        k->populateChoices(entries);
        bool restored = false;
        for (size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].id == prev && prev != "none") {
                k->setValue( (int)i );
                restored = true;
                break;
            }
        }
        if (!restored && best >= 0 && prev == "none") {
            k->setValue(best + 1);
        }
    }
    std::lock_guard<std::mutex> l(_imp->optionsMutex);
    _imp->depthOptions.swap(opts);
}

void
MotionBlur3DNode::onInputChanged(int inputNo)
{
    if (inputNo == 0) {
        refreshDepthChoices();
    }
}

void
MotionBlur3DNode::onKnobsLoaded()
{
    refreshDepthChoices();
    refreshVisibility();
}

bool
MotionBlur3DNode::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/,
                              bool /*originatedFromMainThread*/)
{
    if ( k == _imp->useZ.lock().get() || k == _imp->shutterOffset.lock().get() ) {
        refreshVisibility();
        if ( k == _imp->useZ.lock().get() ) {
            refreshDepthChoices();
        }
        return true;
    }
    return false;
}

void
MotionBlur3DNode::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                                 EffectInstance::ComponentsNeededMap* comps,
                                                 double* passThroughTime, int* passThroughView, int* passThroughInput)
{
    (*comps)[0].push_back( ImagePlaneDesc::getRGBAComponents() );
    KnobBoolPtr useZ = _imp->useZ.lock();
    if ( useZ && useZ->getValue() ) {
        const int idx = _imp->zChannel.lock()->getValue() - 1;
        std::lock_guard<std::mutex> l(_imp->optionsMutex);
        if ( idx >= 0 && idx < (int)_imp->depthOptions.size() ) {
            (*comps)[0].push_back(_imp->depthOptions[idx].plane);
        }
    }
    (*comps)[-1].push_back( ImagePlaneDesc::getRGBAComponents() );
    const int out = _imp->outputUV.lock() ? _imp->outputUV.lock()->getValue() : 0;
    if (out == eOutUVForward) {
        (*comps)[-1].push_back( ImagePlaneDesc::getForwardMotionComponents() );
    } else if (out == eOutUVBackward) {
        (*comps)[-1].push_back( ImagePlaneDesc::getBackwardMotionComponents() );
    }
    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0;
}

StatusEnum
MotionBlur3DNode::render(const RenderActionArgs& args)
{
    if ( args.outputPlanes.empty() ) {
        return eStatusFailed;
    }
    RectI R = args.outputPlanes.front().second->getBounds();
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        if (it->second) {
            R.merge( it->second->getBounds() );
        }
    }
    const double t = args.time;
    Frame src;
    if ( !fetchFrame(this, 0, t, args.mappedScale, args.view, R, 4, &src) ) {
        return eStatusFailed;
    }
    const int outUV = _imp->outputUV.lock()->getValueAtTime(t);

    // Depth
    std::vector<float> depth;
    if ( _imp->useZ.lock()->getValueAtTime(t) ) {
        DepthOption opt;
        bool have = false;
        {
            const int idx = _imp->zChannel.lock()->getValue() - 1;
            std::lock_guard<std::mutex> l(_imp->optionsMutex);
            if ( idx >= 0 && idx < (int)_imp->depthOptions.size() ) {
                opt = _imp->depthOptions[idx];
                have = true;
            }
        }
        Frame z;
        if ( have && fetchFrame(this, 0, t, args.mappedScale, args.view, R, opt.plane.getNumComponents(), &z, &opt.plane) ) {
            const bool farZero = _imp->depthConv.lock()->getValueAtTime(t) == eDepthFarZero;
            depth.resize( (size_t)R.width() * R.height() );
            for (size_t i = 0; i < depth.size(); ++i) {
                const float v = z.px[i * z.nc + std::min(opt.channel, z.nc - 1)];
                depth[i] = farZero ? (v > 1e-12f ? 1.f / v : 1e12f) : (v > 0.f ? v : 1e12f);
            }
        }
    }
    const double dist = _imp->distance.lock()->getValueAtTime(t);

    // Shutter
    const double L = _imp->shutterLength.lock()->getValueAtTime(t);
    double open = t;
    switch ( _imp->shutterOffset.lock()->getValueAtTime(t) ) {
    case eShutterStart: open = t; break;
    case eShutterCentred: open = t - 0.5 * L; break;
    case eShutterEnd: open = t - L; break;
    default: open = t + _imp->shutterCustom.lock()->getValueAtTime(t); break;
    }
    const double close = open + L;

    Frame vec;
    vec.bounds = R;
    vec.nc = 4;
    vec.px.assign(src.px.size(), 0.f);

    EffectInstancePtr camEff = skipDots( getInput(1) );
    CameraProvider* cp = camEff ? dynamic_cast<CameraProvider*>( camEff.get() ) : NULL;
    Cam cNow, cOpen, cClose;
    if ( cp && L > 0. && getCam(cp, t, &cNow) && getCam(cp, open, &cOpen) && getCam(cp, close, &cClose) ) {
        clearPersistentMessage(false);
        // pixel -> NDC through the project format (the camera's frame)
        Format fmt;
        getApp()->getProject()->getProjectDefaultFormat(&fmt);
        const OfxPointD sc = args.mappedScale.toOfxPointD();
        const double sx = sc.x, sy = sc.y;
        const double fx1 = fmt.x1 * sx, fy1 = fmt.y1 * sy;
        const double fw = std::max(1., (double)fmt.width() * sx), fh = std::max(1., (double)fmt.height() * sy);
        const int w = R.width(), h = R.height();
        flow::parallelFor(h, [&](int b, int e) {
            for (int yy = b; yy < e; ++yy) {
                for (int xx = 0; xx < w; ++xx) {
                    const size_t i = (size_t)yy * w + xx;
                    const double px = R.x1 + xx + 0.5, py = R.y1 + yy + 0.5;
                    const double nx = (px - fx1) / fw * 2. - 1., ny = (py - fy1) / fh * 2. - 1.;
                    const double z = depth.empty() ? dist : depth[i];
                    double P[3];
                    unproject(cNow, nx, ny, z, P);
                    double ox, oy, cx, cy;
                    if ( !project(cOpen, P, &ox, &oy) || !project(cClose, P, &cx, &cy) ) {
                        continue;
                    }
                    vec.px[i * 4] = (float)( (cx - ox) * 0.5 * fw );
                    vec.px[i * 4 + 1] = (float)( (cy - oy) * 0.5 * fh );
                    vec.px[i * 4 + 3] = 1.f;
                }
            }
        });
    } else if (!cp) {
        setPersistentMessage(eMessageTypeWarning, "MotionBlur3D: connect a camera to the Cam input.");
    }

    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const bool isColor = it->first.getPlaneID() == ImagePlaneDesc::getRGBAComponents().getPlaneID() ||
                             it->first.getNumComponents() >= 3;
        if (isColor && outUV != eOutUVRGB) {
            writeFrame(src, it->second);
        } else {
            writeFrame(vec, it->second);   // u, v (+ alpha 1 in rgb mode)
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
