/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_DEV_MOTION_MOTIONNODES_H
#define NATRON_ENGINE_DEV_MOTION_MOTIONNODES_H

// NukeX-style motion nodes, built on the optical flow of FlowCore:
//   MotionBlur   (Filter)     motion blur from the image's own motion
//   Kronos       (Time)       retime with motion interpolation
//   OFlow        (Time)       older retimer, same engine, Smoothness knob
//   SmartVector  (Transform)  per-frame forward / backward vectors
//   VectorDistort(Transform)  propagates a frame's paint along SmartVectors
//   MotionBlur3D (Filter)     motion vectors from a 3D camera and depth

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <memory>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

#define PLUGINID_NATRON_MOTIONBLUR    (NATRON_ORGANIZATION_DOMAIN_TOPLEVEL "." NATRON_ORGANIZATION_DOMAIN_SUB ".built-in.MotionBlur")
#define PLUGINID_NATRON_MOTIONBLUR3D  (NATRON_ORGANIZATION_DOMAIN_TOPLEVEL "." NATRON_ORGANIZATION_DOMAIN_SUB ".built-in.MotionBlur3D")
#define PLUGINID_NATRON_KRONOS        (NATRON_ORGANIZATION_DOMAIN_TOPLEVEL "." NATRON_ORGANIZATION_DOMAIN_SUB ".built-in.Kronos")
#define PLUGINID_NATRON_OFLOW         (NATRON_ORGANIZATION_DOMAIN_TOPLEVEL "." NATRON_ORGANIZATION_DOMAIN_SUB ".built-in.OFlow")
#define PLUGINID_NATRON_SMARTVECTOR   (NATRON_ORGANIZATION_DOMAIN_TOPLEVEL "." NATRON_ORGANIZATION_DOMAIN_SUB ".built-in.SmartVector")
#define PLUGINID_NATRON_VECTORDISTORT (NATRON_ORGANIZATION_DOMAIN_TOPLEVEL "." NATRON_ORGANIZATION_DOMAIN_SUB ".built-in.VectorDistort")

NATRON_NAMESPACE_ENTER

// Common declarations of the flow-based nodes (whole-frame, float RGBA).
#define MOTION_NODE_COMMON(Class) \
public: \
    static EffectInstance* BuildEffect(NodePtr n) { return new Class(n); } \
    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; } \
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; } \
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; } \
    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL; \
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL \
    { depths->push_back(eImageBitDepthFloat); } \
    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; } \
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; } \
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; } \
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; } \
    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN \
    { return eRenderSafetyInstanceSafe; }

// ------------------------------------------------------------------ MotionBlur
struct MotionBlurNodePrivate;
class MotionBlurNode
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON
    MOTION_NODE_COMMON(MotionBlurNode)

public:
    MotionBlurNode(NodePtr node);
    virtual ~MotionBlurNode();

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_NATRON_MOTIONBLUR; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "MotionBlur"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back(PLUGIN_GROUP_FILTER); }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 0 ? "Source" : "Matte"; }
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN { return inputNb == 1; }
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::unique_ptr<MotionBlurNodePrivate> _imp;
};

// ------------------------------------------------------------------ Kronos / OFlow
struct KronosNodePrivate;
class KronosNode
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON
    MOTION_NODE_COMMON(KronosNode)

public:
    KronosNode(NodePtr node);
    virtual ~KronosNode();

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual std::string getPluginID() const OVERRIDE WARN_UNUSED_RETURN { return PLUGINID_NATRON_KRONOS; }
    virtual std::string getPluginLabel() const OVERRIDE WARN_UNUSED_RETURN { return "Kronos"; }
    virtual std::string getPluginDescription() const OVERRIDE WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back(PLUGIN_GROUP_TIME); }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 0 ? "Source" : "Matte"; }
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN { return inputNb == 1; }
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getFrameRange(double *first, double *last) OVERRIDE FINAL;

protected:
    // OFlow: no Motion knob, "Smoothness" instead of "Strength", detail 0.2
    virtual bool isOFlow() const { return false; }

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    double sourceTime(double t) const;   // output frame -> source frame
    void refreshVisibility();

    std::unique_ptr<KronosNodePrivate> _imp;
};

class OFlowNode
    : public KronosNode
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static EffectInstance* BuildEffect(NodePtr n) { return new OFlowNode(n); }
    OFlowNode(NodePtr node) : KronosNode(node) {}
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_NATRON_OFLOW; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "OFlow"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

protected:
    virtual bool isOFlow() const OVERRIDE FINAL { return true; }
};

// ------------------------------------------------------------------ SmartVector
struct SmartVectorNodePrivate;
class SmartVectorNode
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON
    MOTION_NODE_COMMON(SmartVectorNode)

public:
    SmartVectorNode(NodePtr node);
    virtual ~SmartVectorNode();

    // plane "smartvector_f01": forward (to t+1) and backward (to t-1) vectors
    static const ImagePlaneDesc& smartVectorPlane();

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_NATRON_SMARTVECTOR; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "SmartVector"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back(PLUGIN_GROUP_TRANSFORM); }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { (void)inputNb; return "Source"; }
    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual PassThroughEnum isPassThroughForNonRenderedPlanes() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return ePassThroughPassThroughNonRenderedPlanes; }
    virtual void getComponentsNeededAndProduced(double time, ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInput) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void exportWrite();

    std::unique_ptr<SmartVectorNodePrivate> _imp;
};

// ------------------------------------------------------------------ VectorDistort
struct VectorDistortNodePrivate;
class VectorDistortNode
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON
    MOTION_NODE_COMMON(VectorDistortNode)

public:
    VectorDistortNode(NodePtr node);
    virtual ~VectorDistortNode();

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_NATRON_VECTORDISTORT; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "VectorDistort"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back(PLUGIN_GROUP_TRANSFORM); }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 0 ? "Src" : "SmartVector"; }
    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual PassThroughEnum isPassThroughForNonRenderedPlanes() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return ePassThroughPassThroughNonRenderedPlanes; }
    virtual void getComponentsNeededAndProduced(double time, ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInput) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::unique_ptr<VectorDistortNodePrivate> _imp;
};

// ------------------------------------------------------------------ MotionBlur3D
struct MotionBlur3DNodePrivate;
class MotionBlur3DNode
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON
    MOTION_NODE_COMMON(MotionBlur3DNode)

public:
    MotionBlur3DNode(NodePtr node);
    virtual ~MotionBlur3DNode();

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 2; }
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_NATRON_MOTIONBLUR3D; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "MotionBlur3D"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back(PLUGIN_GROUP_FILTER); }
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 0 ? "Source" : "Cam"; }
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN { return inputNb == 1; }

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual PassThroughEnum isPassThroughForNonRenderedPlanes() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return ePassThroughPassThroughNonRenderedPlanes; }
    virtual void getComponentsNeededAndProduced(double time, ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInput) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshDepthChoices();
    void refreshVisibility();

    std::unique_ptr<MotionBlur3DNodePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_MOTION_MOTIONNODES_H
