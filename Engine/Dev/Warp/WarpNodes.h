/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_DEV_WARP_WARPNODES_H
#define NATRON_ENGINE_DEV_WARP_WARPNODES_H

// GridWarp and SplineWarp (Transform), Nuke-style.

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include <memory>

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

#define PLUGINID_NATRON_GRIDWARP   (NATRON_ORGANIZATION_DOMAIN_TOPLEVEL "." NATRON_ORGANIZATION_DOMAIN_SUB ".built-in.GridWarp")
#define PLUGINID_NATRON_SPLINEWARP (NATRON_ORGANIZATION_DOMAIN_TOPLEVEL "." NATRON_ORGANIZATION_DOMAIN_SUB ".built-in.SplineWarp")

NATRON_NAMESPACE_ENTER

#define WARP_NODE_COMMON(Class) \
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
    { return eRenderSafetyInstanceSafe; } \
    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL \
    { grouping->push_back(PLUGIN_GROUP_TRANSFORM); }

// ------------------------------------------------------------------ GridWarp
struct GridWarpNodePrivate;
class GridWarpNode
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON
    WARP_NODE_COMMON(GridWarpNode)

public:
    GridWarpNode(NodePtr node);
    virtual ~GridWarpNode();

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_NATRON_GRIDWARP; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "GridWarp"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN { return "Source"; }
    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual bool hasOverlay() const OVERRIDE FINAL { return true; }
    virtual void drawOverlay(double time, const RenderScale& renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual bool onOverlayPenDown(double time, const RenderScale& renderScale, ViewIdx view,
                                  const QPointF& viewportPos, const QPointF& pos,
                                  double pressure, double timestamp, PenType pen) OVERRIDE FINAL;
    virtual bool onOverlayPenMotion(double time, const RenderScale& renderScale, ViewIdx view,
                                    const QPointF& viewportPos, const QPointF& pos,
                                    double pressure, double timestamp) OVERRIDE FINAL;
    virtual bool onOverlayPenUp(double time, const RenderScale& renderScale, ViewIdx view,
                                const QPointF& viewportPos, const QPointF& pos,
                                double pressure, double timestamp) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void resetGrids();
    void loadData();
    void saveData();
    void refreshKeyStatus(double time);

    std::unique_ptr<GridWarpNodePrivate> _imp;
};

// ------------------------------------------------------------------ SplineWarp
struct SplineWarpNodePrivate;
class SplineWarpNode
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON
    WARP_NODE_COMMON(SplineWarpNode)

public:
    SplineWarpNode(NodePtr node);
    virtual ~SplineWarpNode();

    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 3; }
    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN { return PLUGINID_NATRON_SPLINEWARP; }
    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN { return "SplineWarp"; }
    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual std::string getInputLabel(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return inputNb == 0 ? "Source" : (inputNb == 1 ? "A (Roto)" : "B (Roto)"); }
    virtual bool isInputOptional(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN { return inputNb != 0; }
    // the Roto inputs are read as shapes, not rendered
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool hasOverlay() const OVERRIDE FINAL { return true; }
    virtual void drawOverlay(double time, const RenderScale& renderScale, ViewIdx view) OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    std::unique_ptr<SplineWarpNodePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_DEV_WARP_WARPNODES_H
