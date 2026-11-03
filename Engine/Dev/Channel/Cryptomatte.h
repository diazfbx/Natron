/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_CRYPTOMATTE_H
#define NATRON_ENGINE_CRYPTOMATTE_H

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "../../../Global/Macros.h"

#include "../../EffectInstance.h"
#include "../../ViewIdx.h"
#include "../../EngineFwd.h"

NATRON_NAMESPACE_ENTER

struct CryptomattePrivate;

/**
 * @brief Cryptomatte keyer, modelled on Nuke's Cryptomatte node.
 *
 * Reads the CryptoObject / CryptoMaterial / CryptoAsset (or any other) ranked
 * layers of a multi-layer EXR (<name>00, <name>01, ... each holding
 * id0/coverage0/id1/coverage1 in RGBA), resolves the Matte List to IDs (names
 * through the file's manifest, raw "<float>" IDs, '*'/'?' wildcards) and
 * writes the summed coverage of the selected IDs to alpha.
 *
 * Picking: set Picker to Add or Remove and click objects in the viewer.
 * Manifest: read from the EXR header of the upstream Read node
 * (cryptomatte/<key>/manifest or the manif_file sidecar).
 *
 * Input 0: Source (multi-layer EXR carrying the Cryptomatte layers)
 */
class Cryptomatte
    : public EffectInstance
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static EffectInstance* BuildEffect(NodePtr n) { return new Cryptomatte(n); }

    Cryptomatte(NodePtr node);
    virtual ~Cryptomatte();

    virtual int getMajorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual int getMinorVersion() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 0; }
    virtual int getNInputs() const OVERRIDE FINAL WARN_UNUSED_RETURN { return 1; }
    virtual bool getCanTransform() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }

    virtual std::string getPluginID() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return PLUGINID_NATRON_CRYPTOMATTE; }

    virtual std::string getPluginLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Cryptomatte"; }

    virtual std::string getPluginDescription() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getPluginGrouping(std::list<std::string>* grouping) const OVERRIDE FINAL
    { grouping->push_back("Keyer"); }

    virtual std::string getInputLabel(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return "Source"; }

    virtual bool isInputOptional(int /*inputNb*/) const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return false; }

    virtual void addAcceptedComponents(int inputNb, std::list<ImagePlaneDesc>* comps) OVERRIDE FINAL;
    virtual void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const OVERRIDE FINAL;

    virtual RenderSafetyEnum renderThreadSafety() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return eRenderSafetyFullySafe; }

    virtual bool supportsTiles() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    // IDs must never be resampled: render at full resolution only.
    virtual bool supportsMultiResolution() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool getCreateChannelSelectorKnob() const OVERRIDE FINAL WARN_UNUSED_RETURN { return false; }
    virtual bool isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const OVERRIDE WARN_UNUSED_RETURN;

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN { return true; }
    virtual PassThroughEnum isPassThroughForNonRenderedPlanes() const OVERRIDE FINAL WARN_UNUSED_RETURN
    { return ePassThroughPassThroughNonRenderedPlanes; }

    virtual bool hasOverlay() const OVERRIDE FINAL { return true; }
    virtual void drawOverlay(double time, const RenderScale& renderScale, ViewIdx view) OVERRIDE FINAL;
    virtual bool onOverlayPenDown(double time, const RenderScale& renderScale, ViewIdx view,
                                  const QPointF& viewportPos, const QPointF& pos,
                                  double pressure, double timestamp, PenType pen) OVERRIDE FINAL;

private:

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k, ValueChangedReasonEnum reason, ViewSpec view, double time, bool originatedFromMainThread) OVERRIDE FINAL;
    virtual StatusEnum getRegionOfDefinition(U64 hash, double time, const RenderScale& scale, ViewIdx view, RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getComponentsNeededAndProduced(double time, ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInput) OVERRIDE FINAL;
    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE WARN_UNUSED_RETURN;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onMetadataRefreshed(const NodeMetadata& metadata) OVERRIDE FINAL;

    void refreshLayers();
    void refreshManifest();
    void pickAt(double time, ViewIdx view, const QPointF& pos, bool add);

    std::unique_ptr<CryptomattePrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_CRYPTOMATTE_H
