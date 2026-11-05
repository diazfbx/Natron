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
#include <mutex>
#include <sstream>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/TimeLine.h"

#include "MotionCommon.h"

NATRON_NAMESPACE_ENTER

using namespace motion;

// ============================================================================ SmartVector

struct SmartVectorNodePrivate
{
    KnobDoubleWPtr vectorDetail;
    KnobDoubleWPtr strength;
    KnobButtonWPtr exportWrite;
};

const ImagePlaneDesc&
SmartVectorNode::smartVectorPlane()
{
    static const char* chans[4] = { "fwd_u", "fwd_v", "bwd_u", "bwd_v" };
    static const ImagePlaneDesc p("smartvector_f01", "smartvector_f01", "", chans, 4);
    return p;
}

SmartVectorNode::SmartVectorNode(NodePtr node)
    : EffectInstance(node)
    , _imp(new SmartVectorNodePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

SmartVectorNode::~SmartVectorNode()
{
}

std::string
SmartVectorNode::getPluginDescription() const
{
    return "Generates motion vectors for VectorDistort (like NukeX's SmartVector).\n\n"
           "Adds the layer smartvector_f01 to the image: fwd_u / fwd_v = motion to the next frame, "
           "bwd_u / bwd_v = motion to the previous frame (pixels). VectorDistort accumulates them to "
           "carry a paint / patch done on one frame over the whole shot.\n\n"
           "The vectors are long to compute: render them once to EXR with Export Write (all layers), "
           "then feed VectorDistort with the Read of that EXR.";
}

void
SmartVectorNode::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
}

void
SmartVectorNode::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, tr("SmartVector") );
    addComputeInfo(this, page);
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Vector Detail") );
        k->setName("vectorDetail");
        k->setDefaultValue(0.3);
        k->setMinimum(0.01);
        k->setMaximum(1.);
        k->setDisplayMinimum(0.01);
        k->setDisplayMaximum(1.);
        k->setHintToolTip( tr("Density of the vectors (proportion of the image size). Higher follows finer "
                              "deformations, slower.") );
        page->addKnob(k);
        _imp->vectorDetail = k;
    }
    {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Strength") );
        k->setName("strength");
        k->setDefaultValue(1.5);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(3.);
        k->setHintToolTip( tr("Smoothing of the vectors (higher: steadier, less detail).") );
        page->addKnob(k);
        _imp->strength = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>( this, tr("Export") );
        page->addKnob(sep);
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>( this, tr("Export Write") );
        k->setName("exportWrite");
        k->setHintToolTip( tr("Creates a Write node after this one, to render the vectors (and the image) "
                              "to an EXR sequence with every layer.") );
        page->addKnob(k);
        _imp->exportWrite = k;
    }
}

void
SmartVectorNode::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime, int* passThroughView, int* passThroughInput)
{
    (*comps)[0].push_back( ImagePlaneDesc::getRGBAComponents() );
    (*comps)[-1].push_back( ImagePlaneDesc::getRGBAComponents() );
    (*comps)[-1].push_back( smartVectorPlane() );
    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0;
}

FramesNeededMap
SmartVectorNode::getFramesNeeded(double time, ViewIdx view)
{
    std::vector<int> ins(1, 0);
    return framesRange(this, ins, std::floor(time) - 1, std::floor(time) + 1, view);
}

bool
SmartVectorNode::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/,
                             bool /*originatedFromMainThread*/)
{
    if ( k == _imp->exportWrite.lock().get() ) {
        exportWrite();
        return true;
    }
    return false;
}

void
SmartVectorNode::exportWrite()
{
    NodePtr self = getNode();
    if (!self || !getApp()) {
        return;
    }
    const std::string name = self->getScriptName_mt_safe();
    const std::string pattern = "[Project]/smartvector/" + name + ".####.exr";
    double first, last;
    inputFrameRange(this, 0, &first, &last);
    CreateNodeArgs args( PLUGINID_NATRON_WRITE, self->getGroup() );
    args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
    args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, true);
    NodePtr w = getApp()->createWriter(pattern, args, (int)first, (int)last);
    if (!w) {
        setPersistentMessage(eMessageTypeError, "Export Write: could not create the Write node.");
        return;
    }
    w->connectInput(self, 0);
    double x, y;
    self->getPosition(&x, &y);
    w->setPosition(x, y + 120);
    w->setLabel(name + "_write");
    // every layer, so smartvector_f01 is written next to rgba
    KnobIPtr layers = w->getKnobByName("channels");
    if (KnobChoice* c = dynamic_cast<KnobChoice*>( layers.get() )) {
        std::vector<ChoiceOption> e = c->getEntries_mt_safe();
        for (size_t i = 0; i < e.size(); ++i) {
            if (e[i].id == "All" || e[i].label == "All" || e[i].id == "all") {
                c->setValue( (int)i );
                break;
            }
        }
    }
}

StatusEnum
SmartVectorNode::render(const RenderActionArgs& args)
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
    Frame cur;
    if ( !fetchFrame(this, 0, t, args.mappedScale, args.view, R, 4, &cur) ) {
        return eStatusFailed;
    }
    bool wantVectors = false;
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        if ( it->first.getPlaneID() == smartVectorPlane().getPlaneID() ) {
            wantVectors = true;
        }
    }
    Frame vec;
    if (wantVectors) {
        vec.bounds = R;
        vec.nc = 4;
        vec.px.assign(cur.px.size(), 0.f);
        flow::Params fp;
        fp.detail = _imp->vectorDetail.lock()->getValueAtTime(t);
        fp.useGpu = useGpu(this);
        fp.smoothness = _imp->strength.lock()->getValueAtTime(t) / 3.;
        double first, last;
        inputFrameRange(this, 0, &first, &last);
        const std::string tag = "smartvector";
        for (int dir = 0; dir < 2; ++dir) {
            const double tn = dir == 0 ? t + 1 : t - 1;
            if (tn < first || tn > last) {
                continue;   // no neighbour: zero vectors
            }
            Frame nb;
            if ( !fetchFrame(this, 0, tn, args.mappedScale, args.view, R, 4, &nb) ) {
                continue;
            }
            flow::FieldPtr f = getFlow(this, 0, t, tn, args.mappedScale, args.view, R, fp, tag, &cur, &nb);
            if ( !f || isAborted(this) ) {
                return eStatusFailed;
            }
            const flow::Field& ff = *f;
            for (size_t i = 0; i < ff.u.size(); ++i) {
                vec.px[i * 4 + dir * 2] = ff.u[i];
                vec.px[i * 4 + dir * 2 + 1] = ff.v[i];
            }
        }
    }
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator it = args.outputPlanes.begin();
         it != args.outputPlanes.end(); ++it) {
        if (!it->second) {
            continue;
        }
        if ( it->first.getPlaneID() == smartVectorPlane().getPlaneID() ) {
            writeFrame(vec, it->second);
        } else {
            writeFrame(cur, it->second);
        }
    }
    return eStatusOK;
}

// ============================================================================ VectorDistort

namespace {

enum VDOutputEnum { eVDOutputWarped = 0, eVDOutputSTMap, eVDOutputVectors };

// Accumulated map: for each pixel of frame t, its position in the reference frame.
struct WarpMap
{
    int w = 0, h = 0;
    std::vector<float> x, y;
};
typedef std::shared_ptr<const WarpMap> WarpMapPtr;

} // anonymous namespace

struct VectorDistortNodePrivate
{
    KnobIntWPtr referenceFrame;
    KnobButtonWPtr setRef;
    KnobChoiceWPtr output;
    KnobDoubleWPtr blurSize;
    KnobIntWPtr blurSamples;
    KnobChoiceWPtr filter;

    std::mutex cacheMutex;
    std::string cacheKey;               // reference frame + vectors input + scale
    std::map<int, WarpMapPtr> maps;     // frame -> accumulated map

    void clearCache()
    {
        std::lock_guard<std::mutex> l(cacheMutex);
        maps.clear();
        cacheKey.clear();
    }
};

VectorDistortNode::VectorDistortNode(NodePtr node)
    : EffectInstance(node)
    , _imp(new VectorDistortNodePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

VectorDistortNode::~VectorDistortNode()
{
}

std::string
VectorDistortNode::getPluginDescription() const
{
    return "Propagates an image (a paint, a patch, a texture) made on one frame of a shot along "
           "the shot's motion (like NukeX's VectorDistort).\n\n"
           "Src: the image to carry, as it is on the Reference Frame (e.g. a RotoPaint over a frame "
           "hold of the plate). SmartVector: the SmartVector node, or the Read of its EXR render "
           "(layer smartvector_f01). The vectors are accumulated frame by frame from the Reference "
           "Frame, so the result follows deformations (faces, cloth), not only tracks.\n\n"
           "Output: Warped Src (the result), ST Map (red/green = position in the reference frame, "
           "for STMap), Vectors (accumulated displacement in pixels). Blur Size adds motion blur "
           "along the motion.";
}

void
VectorDistortNode::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back( ImagePlaneDesc::getRGBAComponents() );
    comps->push_back( ImagePlaneDesc::getRGBComponents() );
    comps->push_back( ImagePlaneDesc::getAlphaComponents() );
}

void
VectorDistortNode::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>( this, tr("VectorDistort") );
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Reference Frame") );
        k->setName("referenceFrame");
        k->setDefaultValue(1);
        k->setAddNewLine(false);
        k->setHintToolTip( tr("The frame Src was made for (the paint frame).") );
        page->addKnob(k);
        _imp->referenceFrame = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>( this, tr("Set Current Frame") );
        k->setName("setReferenceFrame");
        page->addKnob(k);
        _imp->setRef = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>( this, tr("Output") );
        k->setName("output");
        std::vector<ChoiceOption> o;
        o.push_back( ChoiceOption("warped", "Warped Src", "Src carried to the current frame.") );
        o.push_back( ChoiceOption("stmap", "ST Map", "Position in the reference frame (0..1) in R, G: feed an STMap.") );
        o.push_back( ChoiceOption("vectors", "Vectors", "Accumulated displacement to the reference frame (pixels) in R, G.") );
        k->populateChoices(o);
        k->setDefaultValue(eVDOutputWarped);
        page->addKnob(k);
        _imp->output = k;
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
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, tr("Blur Size") );
        k->setName("blurSize");
        k->setDefaultValue(0.);
        k->setMinimum(0.);
        k->setDisplayMinimum(0.);
        k->setDisplayMaximum(2.);
        k->setHintToolTip( tr("Motion blur of the result, as a shutter length in frames (0 = none).") );
        page->addKnob(k);
        _imp->blurSize = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>( this, tr("Blur Samples") );
        k->setName("blurSamples");
        k->setDefaultValue(8);
        k->setMinimum(2);
        k->setDisplayMinimum(2);
        k->setDisplayMaximum(32);
        page->addKnob(k);
        _imp->blurSamples = k;
    }
}

void
VectorDistortNode::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                                  EffectInstance::ComponentsNeededMap* comps,
                                                  double* passThroughTime, int* passThroughView, int* passThroughInput)
{
    (*comps)[0].push_back( ImagePlaneDesc::getRGBAComponents() );
    (*comps)[1].push_back( SmartVectorNode::smartVectorPlane() );
    (*comps)[-1].push_back( ImagePlaneDesc::getRGBAComponents() );
    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0;
}

FramesNeededMap
VectorDistortNode::getFramesNeeded(double time, ViewIdx view)
{
    const double ref = _imp->referenceFrame.lock()->getValue();
    FramesNeededMap ret = framesRange(this, std::vector<int>(1, 0), ref, ref, view);
    FramesNeededMap v = framesRange(this, std::vector<int>(1, 1), std::min(ref, std::floor(time)) - 1,
                                    std::max(ref, std::ceil(time)) + 1, view);
    ret.insert( v.begin(), v.end() );
    return ret;
}

bool
VectorDistortNode::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/, double /*time*/,
                               bool /*originatedFromMainThread*/)
{
    if ( k == _imp->setRef.lock().get() ) {
        if ( getApp() && getApp()->getTimeLine() ) {
            _imp->referenceFrame.lock()->setValue( (int)getApp()->getTimeLine()->currentFrame() );
        }
        return true;
    }
    if ( k == _imp->referenceFrame.lock().get() ) {
        _imp->clearCache();
        return true;
    }
    return false;
}

void
VectorDistortNode::onInputChanged(int inputNo)
{
    if (inputNo == 1) {
        _imp->clearCache();
    }
}

StatusEnum
VectorDistortNode::render(const RenderActionArgs& args)
{
    if ( args.outputPlanes.empty() ) {
        return eStatusFailed;
    }
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) {
        return eStatusFailed;
    }
    const RectI R = outImg->getBounds();
    const int w = R.width(), h = R.height();
    const int t = (int)std::floor(args.time + 1e-6);
    const int ref = _imp->referenceFrame.lock()->getValue();
    const int output = _imp->output.lock()->getValueAtTime(args.time);
    const flow::FilterEnum filter = (flow::FilterEnum)_imp->filter.lock()->getValueAtTime(args.time);
    EffectInstancePtr vin = getInput(1);
    if (!vin) {
        setPersistentMessage(eMessageTypeError, "VectorDistort: connect the SmartVector input.");
        return eStatusFailed;
    }

    // Per-frame vectors (fwd = to t+1, bwd = to t-1) from the smartvector layer.
    const ImagePlaneDesc& svPlane = SmartVectorNode::smartVectorPlane();
    auto vectorsAt = [&](int f, Frame* fr) -> bool {
        return fetchFrame(this, 1, f, args.mappedScale, args.view, R, 4, fr, &svPlane);
    };

    std::ostringstream key;
    key << ref << ":" << (const void*)vin.get() << ":" << vin->getRenderHash() << ":" << scaleTag(args.mappedScale, R);
    WarpMapPtr startMap;
    int startFrame = ref;
    {
        std::lock_guard<std::mutex> l(_imp->cacheMutex);
        if ( _imp->cacheKey != key.str() ) {
            _imp->maps.clear();
            _imp->cacheKey = key.str();
        }
        // nearest cached frame between ref and t
        for (std::map<int, WarpMapPtr>::const_iterator it = _imp->maps.begin(); it != _imp->maps.end(); ++it) {
            const int f = it->first;
            const bool between = (t >= ref) ? (f >= ref && f <= t) : (f <= ref && f >= t);
            if ( between && std::abs(t - f) < std::abs(t - startFrame) ) {
                startFrame = f;
                startMap = it->second;
            }
        }
    }
    std::shared_ptr<WarpMap> cur = std::make_shared<WarpMap>();
    if (startMap) {
        *cur = *startMap;
    } else {
        cur->w = w;
        cur->h = h;
        cur->x.resize( (size_t)w * h );
        cur->y.resize( (size_t)w * h );
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                cur->x[(size_t)y * w + x] = (float)x;
                cur->y[(size_t)y * w + x] = (float)y;
            }
        }
    }
    // Step frame by frame towards t: M_f(x) = M_{f-1}(x + bwd_f(x)) (forward
    // in time), or M_f(x) = M_{f+1}(x + fwd_f(x)) (backward in time).
    const int step = t >= ref ? 1 : -1;
    for (int f = startFrame + step; step > 0 ? f <= t : f >= t; f += step) {
        Frame vec;
        if ( !vectorsAt(f, &vec) ) {
            setPersistentMessage(eMessageTypeError, "VectorDistort: no smartvector_f01 layer on the SmartVector input "
                                 "(connect a SmartVector node or the Read of its EXR render).");
            return eStatusFailed;
        }
        std::shared_ptr<WarpMap> next = std::make_shared<WarpMap>();
        next->w = w;
        next->h = h;
        next->x.resize( (size_t)w * h );
        next->y.resize( (size_t)w * h );
        const int ch = step > 0 ? 2 : 0;   // bwd (to f-1) when going forward in time
        flow::parallelFor(h, [&](int b, int e) {
            for (int y = b; y < e; ++y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = (size_t)y * w + x;
                    const float px = x + vec.px[i * 4 + ch], py = y + vec.px[i * 4 + ch + 1];
                    float mx, my;
                    flow::sampleImage(cur->x.data(), w, h, 1, px, py, flow::eFilterBilinear, &mx);
                    flow::sampleImage(cur->y.data(), w, h, 1, px, py, flow::eFilterBilinear, &my);
                    next->x[i] = mx;
                    next->y[i] = my;
                }
            }
        });
        cur = next;
        {
            std::lock_guard<std::mutex> l(_imp->cacheMutex);
            if ( _imp->cacheKey == key.str() ) {
                _imp->maps[f] = cur;
                // keep the cache bounded: every 10th frame + the last ones
                while (_imp->maps.size() > 64) {
                    std::map<int, WarpMapPtr>::iterator victim = _imp->maps.end();
                    for (std::map<int, WarpMapPtr>::iterator it = _imp->maps.begin(); it != _imp->maps.end(); ++it) {
                        if (it->first % 10 != 0 && it->first != f) {
                            victim = it;
                            break;
                        }
                    }
                    if ( victim == _imp->maps.end() ) {
                        break;
                    }
                    _imp->maps.erase(victim);
                }
            }
        }
        if ( isAborted(this) ) {
            return eStatusFailed;
        }
    }
    clearPersistentMessage(false);

    Frame result;
    result.bounds = R;
    result.nc = 4;
    result.px.assign( (size_t)w * h * 4, 0.f );
    if (output == eVDOutputSTMap || output == eVDOutputVectors) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                float* o = &result.px[i * 4];
                if (output == eVDOutputSTMap) {
                    o[0] = (cur->x[i] + 0.5f) / (float)w;
                    o[1] = (cur->y[i] + 0.5f) / (float)h;
                } else {
                    o[0] = cur->x[i] - x;
                    o[1] = cur->y[i] - y;
                }
                o[3] = 1.f;
            }
        }
        writeFrame(result, outImg);
        return eStatusOK;
    }

    Frame src;
    if ( !fetchFrame(this, 0, ref, args.mappedScale, args.view, R, 4, &src) ) {
        return eStatusFailed;
    }
    const double blur = _imp->blurSize.lock()->getValueAtTime(args.time);
    const int nb = blur > 0. ? std::max(2, _imp->blurSamples.lock()->getValueAtTime(args.time)) : 1;
    Frame vecT;
    if (nb > 1) {
        vectorsAt(t, &vecT);
    }
    flow::parallelFor(h, [&](int b, int e) {
        float c[4];
        for (int y = b; y < e; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                float* o = &result.px[i * 4];
                for (int s = 0; s < nb; ++s) {
                    float px = (float)x, py = (float)y;
                    if (nb > 1 && vecT.valid()) {
                        const float tau = (float)( ( (double)s / (nb - 1) - 0.5 ) * blur );
                        const int ch = tau >= 0.f ? 0 : 2;
                        px += std::fabs(tau) * vecT.px[i * 4 + ch];
                        py += std::fabs(tau) * vecT.px[i * 4 + ch + 1];
                    }
                    float mx, my;
                    flow::sampleImage(cur->x.data(), w, h, 1, px, py, flow::eFilterBilinear, &mx);
                    flow::sampleImage(cur->y.data(), w, h, 1, px, py, flow::eFilterBilinear, &my);
                    flow::sampleImage(src.px.data(), w, h, 4, mx, my, filter, c);
                    for (int k = 0; k < 4; ++k) {
                        o[k] += c[k];
                    }
                }
                for (int k = 0; k < 4; ++k) {
                    o[k] /= (float)nb;
                }
            }
        }
    });
    writeFrame(result, outImg);
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
