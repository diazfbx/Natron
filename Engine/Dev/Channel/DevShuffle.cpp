/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "DevShuffle.h"

#include <cassert>
#include <cstring>
#include <algorithm>
#include <cctype>

#include <QTimer>

#include <ofxNatron.h> // kNatronOfxParamOutputChannels

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "KnobShuffle.h"
#include "../../TimeLine.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

struct DevShufflePrivate
{
    // Which input to read from (row 1)
    KnobChoiceWPtr inputSource; // "B" (input 0) or "A" (input 1)

    // --- Row 1 (B input) ---
    KnobChoiceWPtr inputLayer;
    KnobChoiceWPtr outputLayer;
    KnobIntWPtr outputR;
    KnobIntWPtr outputG;
    KnobIntWPtr outputB;
    KnobIntWPtr outputA;

    // --- Row 2 (A input) ---
    KnobChoiceWPtr inputLayer2;
    KnobChoiceWPtr outputLayer2;
    KnobIntWPtr outputR2;
    KnobIntWPtr outputG2;
    KnobIntWPtr outputB2;
    KnobIntWPtr outputA2;

    // Wire (noodle widget) / Classic (one drop-down per output channel)
    KnobChoiceWPtr uiMode;
    // Classic mode: Red/Green/Blue/Alpha drop-downs. UI proxies only (not
    // saved): the routing lives in outputR/G/B/A. classicValues[i] is the
    // routing value of drop-down entry i.
    KnobChoiceWPtr classic[4];
    std::vector<int> classicValues;
    bool syncingClassic = false;

    // Cache of discovered layers for input B (row 1)
    std::vector<ImagePlaneDesc> cachedLayers;

    // Cache of discovered layers for input A (row 2)
    std::vector<ImagePlaneDesc> cachedLayers2;

    // User layers are owned by the Node (Node::addUserComponents). Fetch via
    // fetchUserLayers() — they survive save/reload through the canonical
    // <UserComponents> XML block.
};

// Pull user-created layers from the owning Node (the canonical source of truth).
static std::list<ImagePlaneDesc>
fetchUserLayers(const EffectInstance* self)
{
    std::list<ImagePlaneDesc> result;
    if (!self) return result;
    NodePtr node = const_cast<EffectInstance*>(self)->getNode();
    if (node) {
        node->getUserCreatedComponents(&result);
    }
    return result;
}


DevShuffle::DevShuffle(NodePtr node)
    : EffectInstance(node)
    , _imp(new DevShufflePrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

DevShuffle::~DevShuffle()
{
}

std::string
DevShuffle::getPluginDescription() const
{
    return tr("Shuffle: route channels between layers, as in Nuke.\n\n"
              "Mode Wire: drag cables from the input channels (B and A) to the "
              "output channels; the 0 / 1 buttons fill a channel with black / white.\n"
              "Mode Classic: one drop-down per output channel (Red, Green, Blue, "
              "Alpha) choosing a channel of B or A, 0 or 1, like the former Shuffle.\n"
              "Both modes edit the same routing.\n\n"
              "Two-row layer-aware channel shuffle.\n\n"
              "Row 1 routes channels from input B (primary).\n"
              "Row 2 routes channels from input A (secondary).\n\n"
              "Row 2 routing overrides row 1 for the same output channel.\n"
              "Cross-row noodle connections are supported.\n\n"
              "Key improvement over the standard Shuffle:\n"
              "- Pick a layer once, channels auto-populate\n"
              "- Dynamic layer discovery from connected inputs\n"
              "- Two independent routing rows with cross-row support").toStdString();
}

std::string
DevShuffle::getInputLabel(int inputNb) const
{
    if (inputNb == 0) return "B";
    if (inputNb == 1) return "A";
    return "";
}

bool
DevShuffle::isInputOptional(int inputNb) const
{
    return (inputNb == 1); // A is optional
}

void
DevShuffle::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
DevShuffle::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
DevShuffle::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

// ==================== User layer management ====================

void
DevShuffle::addUserLayer(const ImagePlaneDesc& layer, int targetRow)
{
    // Strategy: capture pre-values → addUserComponents (which mutates Row 1's
    // output combo as a hardcoded side effect) → refresh combos → restore both
    // rows → setValueFromID on the target row only.
    KnobChoicePtr outLk  = _imp->outputLayer.lock();
    KnobChoicePtr outL2k = _imp->outputLayer2.lock();
    const int preValue1 = outLk  ? outLk ->getValue() : -1;
    const int preValue2 = outL2k ? outL2k->getValue() : -1;

    NodePtr node = getNode();
    if (node) {
        node->addUserComponents(layer);   // canonical Natron API
    }
    refreshLayerChoices();
    refreshLayerChoices2();

    // Restore (undoes Node::addUserComponents's hardcoded auto-select on Row 1).
    if (outLk  && preValue1 >= 0) outLk ->setValue(preValue1);
    if (outL2k && preValue2 >= 0) outL2k->setValue(preValue2);

    // Target the row the user actually picked.
    if (targetRow == 0 && outLk) {
        outLk->setValueFromID(layer.getPlaneID(), 0);
    } else if (targetRow == 1 && outL2k) {
        outL2k->setValueFromID(layer.getPlaneID(), 0);
    }
}

const std::vector<ImagePlaneDesc>&
DevShuffle::getCachedLayers() const
{
    return _imp->cachedLayers;
}

const std::vector<ImagePlaneDesc>&
DevShuffle::getCachedLayers2() const
{
    return _imp->cachedLayers2;
}

// ==================== Knobs ====================

void
DevShuffle::initializeKnobs()
{
    KnobPagePtr mainPage = AppManager::createKnob<KnobPage>(this, tr("Shuffle"));

    // UI mode. Wire = noodle widget (default); Classic = drop-down per channel.
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Mode"));
        k->setName("shuffleMode");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("wire", "Wire", tr("Connect channels with cables (Nuke Shuffle).").toStdString()));
        entries.push_back(ChoiceOption("classic", "Classic", tr("One drop-down per output channel (former Shuffle).").toStdString()));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setAnimationEnabled(false);
        k->setEvaluateOnChange(false);   // UI only: does not change the image
        k->setHintToolTip(tr("Wire: drag cables between input and output channels.\n"
                              "Classic: pick the source of each output channel in a drop-down.\n"
                              "Both edit the same routing; switch at any time."));
        mainPage->addKnob(k);
        _imp->uiMode = k;
    }

    // Input source (hidden -- per-row input selection is in the widget)
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Input"));
        k->setName("inputSource");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("B", "B", "Read from input B (primary)"));
        entries.push_back(ChoiceOption("A", "A", "Read from input A (secondary)"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->inputSource = k;
    }

    // ---- Row 1 knobs (B input) ----
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("B Layer"));
        k->setName("inputLayer");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Color.RGBA", "Color.RGBA", "RGBA color channels"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->inputLayer = k;
    }
    {
        // Canonical name — Node::addUserComponents looks up this exact knob name
        // (kNatronOfxParamOutputChannels = "outputChannels") to auto-add new
        // user layers to its choices. Row 2's outputLayer2 keeps its own name.
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Output Layer"));
        k->setName(kNatronOfxParamOutputChannels);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Color.RGBA", "Color.RGBA", "Output as Color.RGBA"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputLayer = k;
    }

    // Row 1 routing as KnobInt (hidden) — values: 0-99 = B channel, 100-199 = A channel, -1 = disconnected
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("R"));
        k->setName("outputR");
        k->setIsMetadataSlave(true); // premult follows the alpha routing
        k->setDefaultValue(0);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputR = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("G"));
        k->setName("outputG");
        k->setIsMetadataSlave(true); // premult follows the alpha routing
        k->setDefaultValue(1);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputG = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("B"));
        k->setName("outputB");
        k->setIsMetadataSlave(true); // premult follows the alpha routing
        k->setDefaultValue(2);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputB = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("A"));
        k->setName("outputA");
        k->setIsMetadataSlave(true); // premult follows the alpha routing
        k->setDefaultValue(3);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputA = k;
    }

    // ---- Row 2 knobs (A input) ----
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("A Layer"));
        k->setName("inputLayer2");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Color.RGBA", "Color.RGBA", "RGBA color channels"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->inputLayer2 = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Output Layer 2"));
        k->setName("outputLayer2");
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("Color.RGBA", "Color.RGBA", "Output as Color.RGBA"));
        k->populateChoices(entries);
        k->setDefaultValue(0);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputLayer2 = k;
    }

    // Row 2 routing as KnobInt (hidden) — default: all disconnected (-1)
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("R2"));
        k->setName("outputR2");
        k->setIsMetadataSlave(true); // premult follows the alpha routing
        k->setDefaultValue(-1);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputR2 = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("G2"));
        k->setName("outputG2");
        k->setIsMetadataSlave(true); // premult follows the alpha routing
        k->setDefaultValue(-1);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputG2 = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("B2"));
        k->setName("outputB2");
        k->setIsMetadataSlave(true); // premult follows the alpha routing
        k->setDefaultValue(-1);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputB2 = k;
    }
    {
        KnobIntPtr k = AppManager::createKnob<KnobInt>(this, tr("A2"));
        k->setName("outputA2");
        k->setIsMetadataSlave(true); // premult follows the alpha routing
        k->setDefaultValue(-1);
        k->setSecret(true);
        mainPage->addKnob(k);
        _imp->outputA2 = k;
    }

    // Visual routing widget (row 1) -- embedded in properties panel via KnobGuiShuffle.
    // Marked non-persistent: real routing state lives in outputR/G/B/A ints; this
    // knob is a UI proxy. Skipping persistence also keeps Boost.serialization's
    // string-knob round-trip from breaking project loads.
    {
        KnobShufflePtr k = AppManager::createKnob<KnobShuffle>(this, tr("Routing"));
        k->setName("shuffleRouting");
        k->setEvaluateOnChange(false); // UI proxy: the routing ints render, this string must not
        k->setDefaultValue("0,1,2,3");
        k->setIsPersistent(false);
        mainPage->addKnob(k);
    }

    // Visual routing widget (row 2) -- data only, shares the same widget
    {
        KnobShufflePtr k = AppManager::createKnob<KnobShuffle>(this, tr("Routing 2"));
        k->setName("shuffleRouting2");
        k->setEvaluateOnChange(false); // UI proxy: the routing ints render, this string must not
        k->setDefaultValue("-1,-1,-1,-1");
        k->setIsPersistent(false);
        k->setSecret(true);
        mainPage->addKnob(k);
    }

    // Classic mode: one drop-down per output channel (hidden in Wire mode).
    {
        static const char* names[4]  = { "classicR", "classicG", "classicB", "classicA" };
        static const char* labels[4] = { "Red", "Green", "Blue", "Alpha" };
        for (int c = 0; c < 4; ++c) {
            KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr(labels[c]));
            k->setName(names[c]);
            k->setAnimationEnabled(false);
            k->setIsPersistent(false); // proxy of outputR/G/B/A
            k->setEvaluateOnChange(false); // the ints re-render; this one must not (it is resynced often)
            k->setHintToolTip(tr("Source of this output channel: a channel of B or A, "
                                  "0 (black) or 1 (white)."));
            k->setSecret(true);
            mainPage->addKnob(k);
            _imp->classic[c] = k;
        }
    }
    refreshClassicChoices();
    refreshModeVisibility();
}

// ==================== Wire / Classic modes ====================

void
DevShuffle::refreshModeVisibility()
{
    const bool classic = _imp->uiMode.lock() && _imp->uiMode.lock()->getValue() == 1;
    if (KnobIPtr k = getKnobByName("shuffleRouting")) k->setSecret(classic);
    if (KnobChoicePtr k = _imp->inputLayer.lock())  k->setSecret(!classic);
    if (KnobChoicePtr k = _imp->outputLayer.lock()) k->setSecret(!classic);
    if (KnobChoicePtr k = _imp->inputLayer2.lock()) k->setSecret(!classic);
    for (int c = 0; c < 4; ++c) {
        if (KnobChoicePtr k = _imp->classic[c].lock()) k->setSecret(!classic);
    }
}

static void
appendClassicEntries(const char* input, int base, int layerIdx,
                     const std::vector<ImagePlaneDesc>& layers, bool fallbackRgba,
                     std::vector<ChoiceOption>* opts, std::vector<int>* vals)
{
    std::vector<std::string> chans;
    std::string layerLabel;
    bool color = true;
    if (layerIdx >= 0 && layerIdx < (int)layers.size()) {
        chans = layers[layerIdx].getChannels();
        color = layers[layerIdx].isColorPlane();
        layerLabel = layers[layerIdx].getPlaneLabel();
        if (layerLabel.empty()) layerLabel = layers[layerIdx].getPlaneID();
    } else if (fallbackRgba) {
        chans.push_back("R"); chans.push_back("G"); chans.push_back("B"); chans.push_back("A");
    }
    for (size_t c = 0; c < chans.size() && c < 100; ++c) {
        std::string ch = chans[c];
        std::string name = std::string(input) + ".";
        if (color) {
            std::transform(ch.begin(), ch.end(), ch.begin(), ::tolower);   // B.r, B.g ...
            name += ch;
        } else {
            name += layerLabel + "." + ch;                                // B.depth.Z ...
        }
        opts->push_back(ChoiceOption(name, name, ""));
        vals->push_back(base + (int)c);
    }
}

void
DevShuffle::refreshClassicChoices()
{
    if (_imp->syncingClassic) return;
    _imp->syncingClassic = true;

    std::vector<ChoiceOption> opts;
    std::vector<int> vals;
    const int bIdx = _imp->inputLayer.lock()  ? _imp->inputLayer.lock()->getValue()  : -1;
    const int aIdx = _imp->inputLayer2.lock() ? _imp->inputLayer2.lock()->getValue() : -1;
    appendClassicEntries("B", 0,   bIdx, _imp->cachedLayers,  true,  &opts, &vals);
    appendClassicEntries("A", 100, aIdx, _imp->cachedLayers2, getInput(1) != NULL, &opts, &vals);
    opts.push_back(ChoiceOption("0", "0", "Black")); vals.push_back(-2);
    opts.push_back(ChoiceOption("1", "1", "White")); vals.push_back(-3);
    _imp->classicValues = vals;

    KnobIntPtr ints[4] = { _imp->outputR.lock(), _imp->outputG.lock(), _imp->outputB.lock(), _imp->outputA.lock() };
    for (int c = 0; c < 4; ++c) {
        KnobChoicePtr k = _imp->classic[c].lock();
        if (!k) continue;
        k->populateChoices(opts);
        int v = ints[c] ? ints[c]->getValue() : -2;
        if (v == -1) v = -2;   // disconnected renders black: show "0"
        int idx = (int)(std::find(vals.begin(), vals.end(), v) - vals.begin());
        if (idx >= (int)vals.size()) idx = (int)vals.size() - 2;  // channel not in this layer: "0"
        if (k->getValue() != idx) k->setValue(idx);
    }
    _imp->syncingClassic = false;
}

void
DevShuffle::pushRoutingToWidget()
{
    KnobIPtr shuffleKnob = getKnobByName("shuffleRouting");
    KnobStringBase* strKnob = shuffleKnob ? dynamic_cast<KnobStringBase*>(shuffleKnob.get()) : NULL;
    if (!strKnob) return;
    KnobIntPtr ints[4] = { _imp->outputR.lock(), _imp->outputG.lock(), _imp->outputB.lock(), _imp->outputA.lock() };
    std::string routingStr;
    for (int c = 0; c < 4; ++c) {
        if (c > 0) routingStr += ",";
        routingStr += std::to_string(ints[c] ? ints[c]->getValue() : -1);
    }
    strKnob->setValue("_refresh");   // the widget re-reads the ints on every change
    strKnob->setValue(routingStr);
}

// ==================== Dynamic layer discovery ====================

void
DevShuffle::onInputChanged(int inputNo)
{
    if (inputNo == 0) {
        refreshLayerChoices();
    } else if (inputNo == 1) {
        refreshLayerChoices2();
    }
}

static void discoverLayers(EffectInstance* self, int inputNb,
                           std::vector<ImagePlaneDesc>& outCachedLayers,
                           KnobChoiceWPtr& inputLayerKnob,
                           KnobChoiceWPtr& outputLayerKnob)
{
    // User layers come from the owning Node (canonical Natron API).
    std::list<ImagePlaneDesc> userLayers = fetchUserLayers(self);

    std::list<ImagePlaneDesc> availableLayers;
    EffectInstancePtr input = self->getInput(inputNb);
    if (input) {
        double time = 0;
        if (self->getApp() && self->getApp()->getTimeLine()) {
            time = self->getApp()->getTimeLine()->currentFrame();
        }
        input->getAvailableLayers(time, ViewIdx(0), -1, &availableLayers);
    }

    std::vector<ChoiceOption> layerEntries;
    outCachedLayers.clear();

    for (std::list<ImagePlaneDesc>::const_iterator it = availableLayers.begin();
         it != availableLayers.end(); ++it) {
        const ImagePlaneDesc& layer = *it;
        std::string layerId = layer.getPlaneID();
        std::string label = layer.getPlaneLabel();

        std::string channelsStr;
        const std::vector<std::string>& channels = layer.getChannels();
        for (size_t c = 0; c < channels.size(); ++c) {
            channelsStr += channels[c];
        }

        std::string displayName = label.empty() ? layerId : label;
        if (!channelsStr.empty()) {
            displayName += "." + channelsStr;
        }

        layerEntries.push_back(ChoiceOption(layerId, displayName, displayName));
        outCachedLayers.push_back(layer);
    }

    // ALWAYS populate — user layers must appear in the output combo even when
    // the input has no planes (e.g. before the upstream is wired or evaluated).
    KnobChoicePtr lk = inputLayerKnob.lock();
    if (lk) lk->populateChoices(layerEntries);

    // Output layers = input layers + user-created layers
    std::vector<ChoiceOption> outputEntries = layerEntries;
    for (std::list<ImagePlaneDesc>::const_iterator it = userLayers.begin();
         it != userLayers.end(); ++it) {
        std::string id = it->getPlaneID();
        std::string label = it->getPlaneLabel();
        std::string chans;
        const std::vector<std::string>& ch = it->getChannels();
        for (size_t c = 0; c < ch.size(); ++c) chans += ch[c];
        std::string displayName = (label.empty() ? id : label) + "." + chans;
        outputEntries.push_back(ChoiceOption(id, displayName, displayName));
    }

    KnobChoicePtr olk = outputLayerKnob.lock();
    if (olk) olk->populateChoices(outputEntries);
}

void
DevShuffle::refreshLayerChoices()
{
    discoverLayers(this, 0, _imp->cachedLayers, _imp->inputLayer, _imp->outputLayer);
    refreshClassicChoices();

    // Refresh the wire widget (it re-reads the routing ints).
    pushRoutingToWidget();
}

void
DevShuffle::refreshLayerChoices2()
{
    discoverLayers(this, 1, _imp->cachedLayers2, _imp->inputLayer2, _imp->outputLayer2);
    refreshClassicChoices();

    // Trigger visual widget refresh
    KnobIPtr shuffleKnob = getKnobByName("shuffleRouting");
    if (shuffleKnob) {
        KnobStringBase* strKnob = dynamic_cast<KnobStringBase*>(shuffleKnob.get());
        if (strKnob) {
            strKnob->setValue("_refresh2");
            strKnob->setValue("0,1,2,3");
        }
    }
}

// ==================== knobChanged ====================

bool
DevShuffle::knobChanged(KnobI* k, ValueChangedReasonEnum /*reason*/, ViewSpec /*view*/,
                        double /*time*/, bool /*originatedFromMainThread*/)
{
    KnobChoicePtr inputSourceKnob = _imp->inputSource.lock();
    KnobChoicePtr inputLayerKnob = _imp->inputLayer.lock();
    KnobChoicePtr inputLayer2Knob = _imp->inputLayer2.lock();

    if (!k) return false;

    // Wire / Classic switch
    if (_imp->uiMode.lock() && k == _imp->uiMode.lock().get()) {
        refreshClassicChoices();
        refreshModeVisibility();
        return false;
    }

    // Classic drop-down edited -> write the routing int, refresh the wires
    for (int c = 0; c < 4; ++c) {
        KnobChoicePtr ck = _imp->classic[c].lock();
        if (!ck || k != ck.get()) continue;
        if (_imp->syncingClassic) return false;
        const int idx = ck->getValue();
        if (idx < 0 || idx >= (int)_imp->classicValues.size()) return false;
        _imp->syncingClassic = true;
        KnobIntPtr ints[4]  = { _imp->outputR.lock(),  _imp->outputG.lock(),  _imp->outputB.lock(),  _imp->outputA.lock() };
        KnobIntPtr ints2[4] = { _imp->outputR2.lock(), _imp->outputG2.lock(), _imp->outputB2.lock(), _imp->outputA2.lock() };
        if (ints[c]) ints[c]->setValue(_imp->classicValues[idx]);
        // Classic drives row 1 only: a row-2 wire on the same plane would override it.
        const bool samePlane = resolveOutputPlane(_imp->outputLayer, _imp->cachedLayers).getPlaneID()
                            == resolveOutputPlane(_imp->outputLayer2, _imp->cachedLayers2).getPlaneID();
        if (samePlane && ints2[c] && ints2[c]->getValue() != -1) {
            ints2[c]->setValue(-1);
            if (KnobIPtr r2 = getKnobByName("shuffleRouting2")) {
                if (KnobStringBase* s2 = dynamic_cast<KnobStringBase*>(r2.get())) {
                    s2->setValue("_refresh2");
                    std::string str;
                    for (int i = 0; i < 4; ++i) {
                        if (i > 0) str += ",";
                        str += std::to_string(ints2[i] ? ints2[i]->getValue() : -1);
                    }
                    s2->setValue(str);
                }
            }
        }
        _imp->syncingClassic = false;
        pushRoutingToWidget();
        return true;
    }

    // Routing changed elsewhere (wires, layer change, script): follow in Classic
    {
        KnobIntPtr ints[4] = { _imp->outputR.lock(), _imp->outputG.lock(), _imp->outputB.lock(), _imp->outputA.lock() };
        for (int c = 0; c < 4; ++c) {
            if (ints[c] && k == ints[c].get()) {
                refreshClassicChoices();
                return false;
            }
        }
    }

    if (k == inputSourceKnob.get()) {
        refreshLayerChoices();
        refreshLayerChoices2();
        return true;
    }

    if (k == inputLayerKnob.get()) {
        int layerIdx = inputLayerKnob->getValue();
        if (layerIdx >= 0 && layerIdx < (int)_imp->cachedLayers.size()) {
            const std::vector<std::string>& channels = _imp->cachedLayers[layerIdx].getChannels();
            int numChan = (int)channels.size();

            KnobIntPtr rk = _imp->outputR.lock();
            KnobIntPtr gk = _imp->outputG.lock();
            KnobIntPtr bk = _imp->outputB.lock();
            KnobIntPtr ak = _imp->outputA.lock();

            if (rk) rk->setValue(numChan > 0 ? 0 : -1);
            if (gk) gk->setValue(numChan > 1 ? 1 : (numChan > 0 ? 0 : -1));
            if (bk) bk->setValue(numChan > 2 ? 2 : (numChan > 0 ? 0 : -1));
            if (ak) ak->setValue(numChan > 3 ? 3 : -1);
        }

        refreshClassicChoices(); // Classic entries follow the layer
        // Trigger visual widget refresh
        KnobIPtr shuffleKnob = getKnobByName("shuffleRouting");
        if (shuffleKnob) {
            KnobIntPtr rk = _imp->outputR.lock();
            KnobIntPtr gk = _imp->outputG.lock();
            KnobIntPtr bk = _imp->outputB.lock();
            KnobIntPtr ak = _imp->outputA.lock();
            std::string routingStr;
            for (int c = 0; c < 4; ++c) {
                if (c > 0) routingStr += ",";
                int val = (c == 0) ? rk->getValue() : (c == 1) ? gk->getValue() : (c == 2) ? bk->getValue() : ak->getValue();
                routingStr += std::to_string(val);
            }
            KnobStringBase* strKnob = dynamic_cast<KnobStringBase*>(shuffleKnob.get());
            if (strKnob) strKnob->setValue(routingStr);
        }
        return true;
    }

    if (k == inputLayer2Knob.get()) {
        int layerIdx = inputLayer2Knob->getValue();
        if (layerIdx >= 0 && layerIdx < (int)_imp->cachedLayers2.size()) {
            const std::vector<std::string>& channels = _imp->cachedLayers2[layerIdx].getChannels();
            int numChan = (int)channels.size();

            KnobIntPtr rk = _imp->outputR2.lock();
            KnobIntPtr gk = _imp->outputG2.lock();
            KnobIntPtr bk = _imp->outputB2.lock();
            KnobIntPtr ak = _imp->outputA2.lock();

            // Row 2 uses 100+ offset for its own input channels
            if (rk) rk->setValue(numChan > 0 ? 100 : -1);
            if (gk) gk->setValue(numChan > 1 ? 101 : (numChan > 0 ? 100 : -1));
            if (bk) bk->setValue(numChan > 2 ? 102 : (numChan > 0 ? 100 : -1));
            if (ak) ak->setValue(numChan > 3 ? 103 : -1);
        }

        refreshClassicChoices(); // Classic entries follow the layer
        // Trigger visual widget refresh
        KnobIPtr shuffleKnob = getKnobByName("shuffleRouting");
        if (shuffleKnob) {
            KnobStringBase* strKnob = dynamic_cast<KnobStringBase*>(shuffleKnob.get());
            if (strKnob) {
                strKnob->setValue("_refresh2");
                strKnob->setValue("0,1,2,3");
            }
        }
        return true;
    }

    return false;
}

// ==================== Metadata ====================

StatusEnum
DevShuffle::getPreferredMetadata(NodeMetadata& metadata)
{
    const ImagePlaneDesc r1Plane = resolveOutputPlane(_imp->outputLayer, _imp->cachedLayers);
    if ( !r1Plane.isColorPlane() ) {
        return eStatusOK;   // other planes: keep the default
    }
    if (r1Plane.getNumComponents() < 4) {
        metadata.setOutputPremult(eImagePremultiplicationOpaque);
        return eStatusOK;
    }
    KnobIntPtr ak = _imp->outputA.lock();
    const int a = ak ? ak->getValue() : 3;
    // Is the routed alpha a real alpha channel of B (0..99) or A (100..199)?
    bool realAlpha = false;
    if (a >= 0) {
        const bool fromA = (a >= 100);
        const int ch = fromA ? a - 100 : a;
        const std::vector<ImagePlaneDesc>& layers = fromA ? _imp->cachedLayers2 : _imp->cachedLayers;
        KnobChoicePtr lk = fromA ? _imp->inputLayer2.lock() : _imp->inputLayer.lock();
        const int li = lk ? lk->getValue() : -1;
        if ( li >= 0 && li < (int)layers.size() ) {
            const std::vector<std::string>& chans = layers[li].getChannels();
            if ( ch >= 0 && ch < (int)chans.size() ) {
                const std::string& n = chans[ch];
                realAlpha = (n == "A" || n == "a" || n == "Alpha" || n == "alpha");
            }
        }
    }
    if (!realAlpha) {
        metadata.setOutputPremult(a == -3 ? eImagePremultiplicationOpaque      // alpha = 1
                                          : eImagePremultiplicationUnPremultiplied);
    }
    return eStatusOK;
}

// ==================== RoD ====================

StatusEnum
DevShuffle::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale,
                                  ViewIdx view, RectD* rod)
{
    // Use the union of both inputs' RoD
    EffectInstancePtr inputB = getInput(0);
    EffectInstancePtr inputA = getInput(1);

    bool gotRod = false;
    RectD rodB, rodA;
    bool isProjectFormat = false;

    if (inputB) {
        StatusEnum s = inputB->getRegionOfDefinition_public(0, time, scale, view, &rodB, &isProjectFormat);
        if (s == eStatusOK) {
            *rod = rodB;
            gotRod = true;
        }
    }

    if (inputA) {
        StatusEnum s = inputA->getRegionOfDefinition_public(0, time, scale, view, &rodA, &isProjectFormat);
        if (s == eStatusOK) {
            if (gotRod) {
                rod->merge(rodA);
            } else {
                *rod = rodA;
                gotRod = true;
            }
        }
    }

    return gotRod ? eStatusOK : eStatusFailed;
}

// ==================== Components needed ====================

void
DevShuffle::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                           EffectInstance::ComponentsNeededMap* comps,
                                           double* passThroughTime,
                                           int* passThroughView,
                                           int* passThroughInput)
{
    // Row 1: request layer from input B (0)
    KnobChoicePtr ilk = _imp->inputLayer.lock();
    if (ilk) {
        int layerIdx = ilk->getValue();
        if (layerIdx >= 0 && layerIdx < (int)_imp->cachedLayers.size()) {
            (*comps)[0].push_back(_imp->cachedLayers[layerIdx]);
        }
    }

    // Row 2: request layer from input A (1)
    KnobChoicePtr il2k = _imp->inputLayer2.lock();
    if (il2k) {
        int layerIdx2 = il2k->getValue();
        if (layerIdx2 >= 0 && layerIdx2 < (int)_imp->cachedLayers2.size()) {
            (*comps)[1].push_back(_imp->cachedLayers2[layerIdx2]);
        }
    }

    // Output: BOTH rows' planes. If row 2 targets a distinct plane, we declare
    // it as a second produced plane so Natron actually allocates + renders it.
    // Without this, Row 2's "shuffle into new layer" UX is a no-op.
    ImagePlaneDesc r1Plane = resolveOutputPlane(_imp->outputLayer,  _imp->cachedLayers);
    ImagePlaneDesc r2Plane = resolveOutputPlane(_imp->outputLayer2, _imp->cachedLayers2);
    (*comps)[-1].push_back(r1Plane);
    if (r2Plane.getPlaneID() != r1Plane.getPlaneID()) {
        (*comps)[-1].push_back(r2Plane);
    }

    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0; // B is the primary passthrough
}

// ==================== Output-plane resolution helper ====================

ImagePlaneDesc
DevShuffle::resolveOutputPlane(const KnobChoiceWPtr& outputLayerKnob,
                                const std::vector<ImagePlaneDesc>& cachedInputLayers) const
{
    KnobChoicePtr olk = outputLayerKnob.lock();
    int idx = olk ? olk->getValue() : -1;
    if (idx < 0) {
        return ImagePlaneDesc::getRGBAComponents();
    }
    if (idx < (int)cachedInputLayers.size()) {
        return cachedInputLayers[idx];
    }
    // Fall through to user layers, indexed after the cached input layers.
    const std::list<ImagePlaneDesc> userLayers = fetchUserLayers(this);
    int userIdx = idx - (int)cachedInputLayers.size();
    int uIdx = 0;
    for (std::list<ImagePlaneDesc>::const_iterator it = userLayers.begin();
         it != userLayers.end(); ++it, ++uIdx) {
        if (uIdx == userIdx) return *it;
    }
    return ImagePlaneDesc::getRGBAComponents();
}

// ==================== Post-load / metadata-refresh hooks ====================

void
DevShuffle::onKnobsLoaded()
{
    // Defer to next event-loop iteration. Inputs may not be wired yet at this
    // synchronous moment; the deferred call picks up user layers + restored
    // routing once the Node is fully constructed.
    refreshModeVisibility();   // saved Mode (Wire / Classic)
    QTimer::singleShot(0, this, [this]() {
        refreshLayerChoices();
        refreshLayerChoices2();
        refreshClassicChoices();
    });
}

void
DevShuffle::onMetadataRefreshed(const NodeMetadata& /*metadata*/)
{
    // Canonical refresh trigger — matches the stock Shuffle's getClipPreferences.
    // Fires after upstream metadata is computed → getAvailableLayers returns
    // real data. Both input layers + user layers populate in one shot.
    refreshLayerChoices();
    refreshLayerChoices2();
}

// ==================== Render ====================

StatusEnum
DevShuffle::render(const RenderActionArgs& args)
{
    assert(!args.outputPlanes.empty());

    // Resolve the planes each row wants to write to.
    const ImagePlaneDesc r1Plane = resolveOutputPlane(_imp->outputLayer,  _imp->cachedLayers);
    const ImagePlaneDesc r2Plane = resolveOutputPlane(_imp->outputLayer2, _imp->cachedLayers2);

    // --- Get Row 1 source image (input B = 0) ---
    KnobChoicePtr inputLayerKnob = _imp->inputLayer.lock();
    int layerIdx = inputLayerKnob ? inputLayerKnob->getValue() : -1;
    const ImagePlaneDesc* requestedPlane = NULL;
    if (layerIdx >= 0 && layerIdx < (int)_imp->cachedLayers.size()) {
        requestedPlane = &_imp->cachedLayers[layerIdx];
    }

    RectI roiPixelB;
    ImagePtr srcImgB = getImage(0, args.time, args.mappedScale, args.view,
                                NULL, requestedPlane, false, false,
                                eStorageModeRAM, 0, &roiPixelB);

    // --- Get Row 2 source image (input A = 1) ---
    KnobChoicePtr inputLayer2Knob = _imp->inputLayer2.lock();
    int layerIdx2 = inputLayer2Knob ? inputLayer2Knob->getValue() : -1;
    const ImagePlaneDesc* requestedPlane2 = NULL;
    if (layerIdx2 >= 0 && layerIdx2 < (int)_imp->cachedLayers2.size()) {
        requestedPlane2 = &_imp->cachedLayers2[layerIdx2];
    }

    RectI roiPixelA;
    ImagePtr srcImgA = getImage(1, args.time, args.mappedScale, args.view,
                                NULL, requestedPlane2, false, false,
                                eStorageModeRAM, 0, &roiPixelA);

    // Must have at least one source
    if (!srcImgB && !srcImgA) return eStatusFailed;

    // --- Get Row 1 channel routing from KnobInt ---
    KnobIntPtr rKnob1 = _imp->outputR.lock();
    KnobIntPtr gKnob1 = _imp->outputG.lock();
    KnobIntPtr bKnob1 = _imp->outputB.lock();
    KnobIntPtr aKnob1 = _imp->outputA.lock();

    int routing1[4];
    routing1[0] = rKnob1 ? rKnob1->getValue() : 0;
    routing1[1] = gKnob1 ? gKnob1->getValue() : 1;
    routing1[2] = bKnob1 ? bKnob1->getValue() : 2;
    routing1[3] = aKnob1 ? aKnob1->getValue() : 3;

    // --- Get Row 2 channel routing from KnobInt ---
    KnobIntPtr rKnob2 = _imp->outputR2.lock();
    KnobIntPtr gKnob2 = _imp->outputG2.lock();
    KnobIntPtr bKnob2 = _imp->outputB2.lock();
    KnobIntPtr aKnob2 = _imp->outputA2.lock();

    int routing2[4];
    routing2[0] = rKnob2 ? rKnob2->getValue() : -1;
    routing2[1] = gKnob2 ? gKnob2->getValue() : -1;
    routing2[2] = bKnob2 ? bKnob2->getValue() : -1;
    routing2[3] = aKnob2 ? aKnob2->getValue() : -1;

    const int srcBNumComp = srcImgB ? srcImgB->getComponents().getNumComponents() : 0;
    const int srcANumComp = srcImgA ? srcImgA->getComponents().getNumComponents() : 0;

    // Decode routing value to a float from the source images.
    // -1 = disconnected, -2 = black, -3 = white,
    // 0-99 = B input channel[value], 100-199 = A input channel[value-100].
    auto readChannel = [&](int routingVal, const float* srcB, const float* srcA) -> float {
        if (routingVal == -2) return 0.0f;
        if (routingVal == -3) return 1.0f;
        if (routingVal < 0) return 0.0f;
        if (routingVal < 100) {
            return (srcB && routingVal < srcBNumComp) ? srcB[routingVal] : 0.0f;
        }
        int ch = routingVal - 100;
        return (srcA && ch < srcANumComp) ? srcA[ch] : 0.0f;
    };

    const RectI srcBBounds = srcImgB ? srcImgB->getBounds() : RectI();
    const RectI srcABounds = srcImgA ? srcImgA->getBounds() : RectI();

    // Optional read access (may be null)
    std::unique_ptr<Image::ReadAccess> raB;
    std::unique_ptr<Image::ReadAccess> raA;
    if (srcImgB) raB.reset(new Image::ReadAccess(srcImgB.get()));
    if (srcImgA) raA.reset(new Image::ReadAccess(srcImgA.get()));

    // Iterate over every output plane Natron asked us to produce. For each
    // plane, decide whether it's Row 1's target, Row 2's target, or both,
    // then run a per-plane pixel fill.
    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator pit = args.outputPlanes.begin();
         pit != args.outputPlanes.end(); ++pit) {
        const ImagePlaneDesc& planeDesc = pit->first;
        const ImagePtr&       outImg    = pit->second;
        if (!outImg) continue;

        const std::string planeId = planeDesc.getPlaneID();
        const bool isR1 = (planeId == r1Plane.getPlaneID());
        const bool isR2 = (planeId == r2Plane.getPlaneID());
        if (!isR1 && !isR2) continue; // plane we don't write to

        // Only the render window: the output image can be a cached image larger
        // than what is asked now (Natron renders the missing rectangles of a
        // cached image). Filling all of outBounds overwrote the already
        // rendered parts with black where the source was not fetched -> nodes
        // downstream (Grade, ColorCorrect, Glow) went black / stopped updating.
        const RectI outBounds = args.roi.intersect( outImg->getBounds() );
        if ( outBounds.isNull() ) continue;
        const int   outNumComp = outImg->getComponents().getNumComponents();

        Image::WriteAccess wa(outImg.get());

        if (isR1 && isR2) {
            // Same plane targeted by both rows → Row 1 fills, Row 2 overrides
            // connected channels (matches the original single-plane behavior).
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;

                    const float* srcB = NULL;
                    const float* srcA = NULL;
                    if (raB && srcBBounds.contains(x, y)) srcB = (const float*)raB->pixelAt(x, y);
                    if (raA && srcABounds.contains(x, y)) srcA = (const float*)raA->pixelAt(x, y);

                    // Row 1 fills (B1: per-component guards against sub-RGBA dst).
                    if (outNumComp > 0) dst[0] = readChannel(routing1[0], srcB, srcA);
                    if (outNumComp > 1) dst[1] = readChannel(routing1[1], srcB, srcA);
                    if (outNumComp > 2) dst[2] = readChannel(routing1[2], srcB, srcA);
                    if (outNumComp > 3) dst[3] = readChannel(routing1[3], srcB, srcA);

                    // Row 2 overrides only connected channels (routing != -1).
                    if (srcImgA) {
                        if (outNumComp > 0 && routing2[0] != -1) dst[0] = readChannel(routing2[0], srcB, srcA);
                        if (outNumComp > 1 && routing2[1] != -1) dst[1] = readChannel(routing2[1], srcB, srcA);
                        if (outNumComp > 2 && routing2[2] != -1) dst[2] = readChannel(routing2[2], srcB, srcA);
                        if (outNumComp > 3 && routing2[3] != -1) dst[3] = readChannel(routing2[3], srcB, srcA);
                    }
                }
            }
        } else if (isR1) {
            // Row 1 only fills this plane.
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;

                    const float* srcB = NULL;
                    const float* srcA = NULL;
                    if (raB && srcBBounds.contains(x, y)) srcB = (const float*)raB->pixelAt(x, y);
                    if (raA && srcABounds.contains(x, y)) srcA = (const float*)raA->pixelAt(x, y);

                    if (outNumComp > 0) dst[0] = readChannel(routing1[0], srcB, srcA);
                    if (outNumComp > 1) dst[1] = readChannel(routing1[1], srcB, srcA);
                    if (outNumComp > 2) dst[2] = readChannel(routing1[2], srcB, srcA);
                    if (outNumComp > 3) dst[3] = readChannel(routing1[3], srcB, srcA);
                }
            }
        } else {
            // Row 2 only — this plane is row 2's distinct output. Treat
            // disconnected (-1) channels as black (0.0).
            for (int y = outBounds.y1; y < outBounds.y2; ++y) {
                for (int x = outBounds.x1; x < outBounds.x2; ++x) {
                    float* dst = (float*)wa.pixelAt(x, y);
                    if (!dst) continue;

                    const float* srcB = NULL;
                    const float* srcA = NULL;
                    if (raB && srcBBounds.contains(x, y)) srcB = (const float*)raB->pixelAt(x, y);
                    if (raA && srcABounds.contains(x, y)) srcA = (const float*)raA->pixelAt(x, y);

                    if (outNumComp > 0) dst[0] = readChannel(routing2[0], srcB, srcA);
                    if (outNumComp > 1) dst[1] = readChannel(routing2[1], srcB, srcA);
                    if (outNumComp > 2) dst[2] = readChannel(routing2[2], srcB, srcA);
                    if (outNumComp > 3) dst[3] = readChannel(routing2[3], srcB, srcA);
                }
            }
        }
    }

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING

#include "moc_DevShuffle.cpp"
