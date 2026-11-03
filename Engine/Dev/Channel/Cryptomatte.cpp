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

// ***** BEGIN PYTHON BLOCK *****
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Cryptomatte.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <sstream>

#include <QTimer>
#include <QPointF>

#include "CryptomatteCore.h"

#include "../../AppInstance.h"
#include "../../AppManager.h"
#include "../../Image.h"
#include "../../ImagePlaneDesc.h"
#include "../../KnobFile.h"
#include "../../KnobTypes.h"
#include "../../Node.h"
#include "../../NodeMetadata.h"
#include "../../TimeLine.h"
#include "../../ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

typedef std::vector<std::pair<std::string, uint32_t> > Manifest;

// One Cryptomatte layer: its ranked planes <name>00, <name>01, ...
struct CryptoFamily
{
    std::string name;
    std::vector<ImagePlaneDesc> ranks;
};

enum PickerModeEnum
{
    ePickerOff = 0,
    ePickerAdd,
    ePickerRemove
};

// Channel indices of R,G,B,A (id0, coverage0, id1, coverage1) in a plane,
// from the channel names; falls back to storage order.
void
cryptoChannelIndices(const ImagePlaneDesc& comps, int idx[4])
{
    for (int i = 0; i < 4; ++i) idx[i] = i;
    const std::vector<std::string>& ch = comps.getChannels();
    if (ch.size() < 4) return;
    static const char* names = "rgba";
    int found[4] = { -1, -1, -1, -1 };
    for (size_t c = 0; c < ch.size(); ++c) {
        std::string n = ch[c];
        const size_t dot = n.find_last_of('.');
        if (dot != std::string::npos) n = n.substr(dot + 1);
        if (n.size() != 1) continue;
        const char lc = (char)std::tolower((unsigned char)n[0]);
        for (int k = 0; k < 4; ++k) if (lc == names[k]) found[k] = (int)c;
    }
    if (found[0] >= 0 && found[1] >= 0 && found[2] >= 0 && found[3] >= 0) {
        for (int k = 0; k < 4; ++k) idx[k] = found[k];
    }
}

} // anonymous namespace

struct CryptomattePrivate
{
    KnobChoiceWPtr layer;
    KnobChoiceWPtr picker;
    KnobStringWPtr matteList;
    KnobButtonWPtr clearList;
    KnobBoolWPtr preview;
    KnobBoolWPtr matteOnly;
    KnobBoolWPtr invert;
    KnobButtonWPtr reloadManifest;
    KnobStringWPtr info;

    mutable std::mutex mtx;
    std::vector<CryptoFamily> families;          // from the input's available layers
    std::map<std::string, Manifest> manifests;   // layer name -> manifest
    std::string manifestSource;                  // EXR the manifests came from

    // Current layer (copy, so render threads never hold the lock while working).
    bool currentFamily(const std::string& name, CryptoFamily* out) const
    {
        std::lock_guard<std::mutex> lk(mtx);
        for (size_t i = 0; i < families.size(); ++i) {
            if (families[i].name == name) { *out = families[i]; return true; }
        }
        if (name.empty() && !families.empty()) { *out = families.front(); return true; }
        return false;
    }

    Manifest manifestFor(const std::string& name) const
    {
        std::lock_guard<std::mutex> lk(mtx);
        std::map<std::string, Manifest>::const_iterator it = manifests.find(name);
        return it == manifests.end() ? Manifest() : it->second;
    }

    std::string selectedLayerName() const
    {
        KnobChoicePtr k = layer.lock();
        if (!k || k->getNumEntries() == 0) return std::string();
        return k->getActiveEntry().id;
    }
};

Cryptomatte::Cryptomatte(NodePtr node)
    : EffectInstance(node)
    , _imp(new CryptomattePrivate())
{
    // Cryptomatte IDs are hashes stored as floats: any downscale/filtering
    // would invent IDs, so always render at full resolution.
    setSupportsRenderScaleMaybe(eSupportsNo);
}

Cryptomatte::~Cryptomatte()
{
}

std::string
Cryptomatte::getPluginDescription() const
{
    return "Extract mattes from Cryptomatte layers (CryptoObject, CryptoMaterial, CryptoAsset, ...), "
           "like Nuke's Cryptomatte node.\n\n"
           "Connect a Read of a multi-layer EXR rendered with Cryptomatte (Arnold, Redshift, V-Ray, "
           "RenderMan, Karma, Blender/Cycles, Natron's CyclesRender...). Pick the Layer, then either "
           "set Picker to Add / Remove and click objects in the viewer, or type names in the Matte List "
           "(comma-separated, \"quoted\" when a name contains a comma, '*' and '?' wildcards, "
           "<float> for an ID that has no manifest name).\n\n"
           "The matte is the summed coverage of the selected IDs, written to alpha. Preview shows every "
           "ID in a random colour with the selection highlighted. Names come from the manifest stored in "
           "the EXR header of the upstream Read (cryptomatte/<key>/manifest or a manif_file sidecar).\n\n"
           "Cryptomatte is an open standard by Psyop (Jonah Friedman, Andy Jones): "
           "https://github.com/Psyop/Cryptomatte";
}

void
Cryptomatte::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
}

void
Cryptomatte::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

bool
Cryptomatte::isHostChannelSelectorSupported(bool*, bool*, bool*, bool*) const
{
    return false;
}

void
Cryptomatte::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Cryptomatte"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Layer"));
        k->setName("cryptoLayer"); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Cryptomatte layer to key: CryptoObject, CryptoMaterial, CryptoAsset... "
                              "Filled from the input's layers named <name>00, <name>01, ..."));
        page->addKnob(k); _imp->layer = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Picker"));
        k->setName("picker"); k->setAnimationEnabled(false);
        std::vector<ChoiceOption> e;
        e.push_back(ChoiceOption("Off", "", "Clicks in the viewer do nothing"));
        e.push_back(ChoiceOption("Add", "", "Click an object in the viewer to add it to the Matte List"));
        e.push_back(ChoiceOption("Remove", "", "Click an object in the viewer to remove it from the Matte List"));
        k->populateChoices(e);
        k->setDefaultValue(ePickerAdd);
        k->setEvaluateOnChange(false);
        k->setHintToolTip(tr("Viewer picking (the node must be selected / its panel open). "
                              "Add: click an object to add it. Remove: click to remove it."));
        page->addKnob(k); _imp->picker = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Matte List"));
        k->setName("matteList"); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Selected names, comma-separated (Nuke syntax): name, \"name, with comma\", "
                              "wildcards * and ? (e.g. rock_*), <float> for a raw ID. Names are resolved "
                              "through the manifest, or hashed (MurmurHash3) when there is none."));
        page->addKnob(k); _imp->matteList = k;
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Clear"));
        k->setName("clearMatteList");
        k->setHintToolTip(tr("Empty the Matte List."));
        page->addKnob(k); _imp->clearList = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Output"));
        page->addKnob(sep);
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Preview"));
        k->setName("preview"); k->setDefaultValue(true); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("RGB shows every ID in a random colour, the selected ones highlighted. "
                              "Off: RGB passes the source through. Alpha is the matte either way."));
        page->addKnob(k); _imp->preview = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Matte Only"));
        k->setName("matteOnly"); k->setDefaultValue(false); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Write the matte to R, G, B and A (overrides Preview)."));
        page->addKnob(k); _imp->matteOnly = k;
    }
    {
        KnobBoolPtr k = AppManager::createKnob<KnobBool>(this, tr("Invert"));
        k->setName("invert"); k->setDefaultValue(false); k->setAnimationEnabled(false);
        k->setHintToolTip(tr("Invert the matte."));
        page->addKnob(k); _imp->invert = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Manifest"));
        page->addKnob(sep);
    }
    {
        KnobButtonPtr k = AppManager::createKnob<KnobButton>(this, tr("Reload Manifest"));
        k->setName("reloadManifest");
        k->setHintToolTip(tr("Re-read the layers and the manifest from the upstream Read's EXR."));
        page->addKnob(k); _imp->reloadManifest = k;
    }
    {
        KnobStringPtr k = AppManager::createKnob<KnobString>(this, tr("Info"));
        k->setName("info"); k->setAsLabel(); k->setIsPersistent(false);
        k->setEvaluateOnChange(false);
        page->addKnob(k); _imp->info = k;
    }
}

// ==================== Layers + manifest ====================

void
Cryptomatte::refreshLayers()
{
    std::list<ImagePlaneDesc> available;
    EffectInstancePtr input = getInput(0);
    if (input) {
        double time = 0;
        if (getApp() && getApp()->getTimeLine()) time = getApp()->getTimeLine()->currentFrame();
        input->getAvailableLayers(time, ViewIdx(0), -1, &available);
    }

    // Group <name>NN planes (4 channels) into families, ranks in order.
    std::map<std::string, std::map<int, ImagePlaneDesc> > grouped;
    std::vector<std::string> order;
    for (std::list<ImagePlaneDesc>::const_iterator it = available.begin(); it != available.end(); ++it) {
        if (it->getNumComponents() < 4) continue;
        std::string family; int rank;
        if (!CryptomatteCore::splitRankedLayer(it->getPlaneID(), &family, &rank)) continue;
        if (!grouped.count(family)) order.push_back(family);
        grouped[family][rank] = *it;
    }
    std::vector<CryptoFamily> fams;
    for (size_t i = 0; i < order.size(); ++i) {
        CryptoFamily f; f.name = order[i];
        const std::map<int, ImagePlaneDesc>& r = grouped[order[i]];
        for (int k = 0; r.count(k); ++k) f.ranks.push_back(r.find(k)->second);   // contiguous 00, 01, ...
        if (!f.ranks.empty()) fams.push_back(f);
    }

    KnobChoicePtr layerKnob = _imp->layer.lock();
    const std::string previous = _imp->selectedLayerName();
    {
        std::lock_guard<std::mutex> lk(_imp->mtx);
        _imp->families = fams;
    }
    if (layerKnob) {
        std::vector<ChoiceOption> entries;
        for (size_t i = 0; i < fams.size(); ++i) {
            std::ostringstream tip; tip << fams[i].ranks.size() << " rank layer(s)";
            entries.push_back(ChoiceOption(fams[i].name, "", tip.str()));
        }
        layerKnob->populateChoices(entries);
        if (!previous.empty()) {
            for (size_t i = 0; i < fams.size(); ++i) {
                if (fams[i].name == previous) { layerKnob->setValue((int)i); break; }
            }
        }
    }
    refreshManifest();
}

void
Cryptomatte::refreshManifest()
{
    // Nearest upstream node with a "filename" knob = the Read of the EXR.
    NodePtr n = getNode();
    n = n ? n->getInput(0) : NodePtr();
    KnobIPtr fileKnobI;
    for (int hop = 0; n && hop < 16 && !fileKnobI; ++hop) {
        KnobIPtr k = n->getKnobByName("filename");
        if (k && std::dynamic_pointer_cast<KnobStringBase>(k)) { fileKnobI = k; break; }
        n = n->getInput(0);
    }

    std::map<std::string, Manifest> manifests;
    std::string source;
    if (fileKnobI) {
        const double time = getCurrentTime();
        std::vector<std::string> candidates;
        if (KnobFile* kf = dynamic_cast<KnobFile*>(fileKnobI.get())) {
            candidates.push_back(kf->getFileName((int)std::floor(time + 0.5), ViewSpec(0)));
        }
        KnobStringBasePtr ks = std::dynamic_pointer_cast<KnobStringBase>(fileKnobI);
        const std::string pattern = ks ? ks->getValue() : std::string();
        // Manual ### / %0Nd expansion at the current frame, then the raw value.
        {
            std::string s = pattern;
            const size_t h = s.find('#');
            if (h != std::string::npos) {
                size_t e = h;
                while (e < s.size() && s[e] == '#') ++e;
                std::ostringstream num;
                num.width((std::streamsize)(e - h)); num.fill('0'); num << (int)std::floor(time + 0.5);
                candidates.push_back(s.substr(0, h) + num.str() + s.substr(e));
            } else if (std::count(s.begin(), s.end(), '%') == 1) {
                char buf[4096];
                std::snprintf(buf, sizeof(buf), s.c_str(), (int)std::floor(time + 0.5));
                candidates.push_back(buf);
            }
            candidates.push_back(pattern);
        }
        for (size_t c = 0; c < candidates.size(); ++c) {
            std::map<std::string, std::string> attrs;
            if (candidates[c].empty() || !CryptomatteCore::readExrStringAttributes(candidates[c], &attrs)) continue;
            const std::vector<CryptomatteCore::CryptoLayerInfo> layers =
                CryptomatteCore::cryptoLayersFromHeader(attrs, candidates[c]);
            for (size_t l = 0; l < layers.size(); ++l) {
                Manifest m;
                if (CryptomatteCore::parseManifest(layers[l].manifest, &m)) manifests[layers[l].name] = m;
            }
            source = candidates[c];
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lk(_imp->mtx);
        _imp->manifests = manifests;
        _imp->manifestSource = source;
    }

    // Info line.
    std::ostringstream info;
    CryptoFamily fam;
    const std::string layerName = _imp->selectedLayerName();
    if (!_imp->currentFamily(layerName, &fam)) {
        info << "No Cryptomatte layer found in the input (expected layers named CryptoObject00, CryptoMaterial00, ...).";
    } else {
        info << fam.name << ": " << fam.ranks.size() << " rank layer(s). ";
        std::map<std::string, Manifest>::const_iterator it = manifests.find(fam.name);
        if (it != manifests.end()) {
            info << it->second.size() << " names in the manifest";
        } else if (!fileKnobI) {
            info << "No Read upstream: names are matched by hash only";
        } else if (source.empty()) {
            info << "Could not read the EXR header: names are matched by hash only";
        } else {
            info << "No manifest for this layer: names are matched by hash only";
        }
    }
    if (KnobStringPtr k = _imp->info.lock()) k->setValue(info.str());
}

void
Cryptomatte::onInputChanged(int /*inputNo*/)
{
    refreshLayers();
}

void
Cryptomatte::onKnobsLoaded()
{
    // Inputs may not be wired yet when knobs are restored: refresh on the next
    // event-loop iteration, once the node graph is complete.
    QTimer::singleShot(0, this, [this]() { refreshLayers(); });
}

void
Cryptomatte::onMetadataRefreshed(const NodeMetadata& /*metadata*/)
{
    refreshLayers();
}

bool
Cryptomatte::knobChanged(KnobI* k,
                         ValueChangedReasonEnum /*reason*/,
                         ViewSpec /*view*/,
                         double /*time*/,
                         bool /*originatedFromMainThread*/)
{
    if (k == _imp->clearList.lock().get()) {
        _imp->matteList.lock()->setValue(std::string());
        return true;
    }
    if (k == _imp->reloadManifest.lock().get()) {
        refreshLayers();
        return true;
    }
    if (k == _imp->layer.lock().get()) {
        refreshManifest();   // info line follows the layer
        return true;
    }
    return false;
}

// ==================== Render ====================

StatusEnum
Cryptomatte::getRegionOfDefinition(U64 /*hash*/, double time, const RenderScale& scale, ViewIdx view, RectD* rod)
{
    EffectInstancePtr input = getInput(0);
    if (!input) return eStatusFailed;
    bool isProject;
    return input->getRegionOfDefinition_public(input->getHash(), time, scale, view, rod, &isProject);
}

void
Cryptomatte::getComponentsNeededAndProduced(double /*time*/, ViewIdx /*view*/,
                                            EffectInstance::ComponentsNeededMap* comps,
                                            double* passThroughTime,
                                            int* passThroughView,
                                            int* passThroughInput)
{
    (*comps)[0].push_back(ImagePlaneDesc::getRGBAComponents());
    CryptoFamily fam;
    if (_imp->currentFamily(_imp->selectedLayerName(), &fam)) {
        for (size_t r = 0; r < fam.ranks.size(); ++r) (*comps)[0].push_back(fam.ranks[r]);
    }
    (*comps)[-1].push_back(ImagePlaneDesc::getRGBAComponents());
    *passThroughTime = 0;
    *passThroughView = 0;
    *passThroughInput = 0;
}

StatusEnum
Cryptomatte::render(const RenderActionArgs& args)
{
    if (args.outputPlanes.empty()) return eStatusFailed;

    const std::string layerName = _imp->selectedLayerName();
    CryptoFamily fam;
    const bool haveFamily = _imp->currentFamily(layerName, &fam);
    const Manifest manifest = haveFamily ? _imp->manifestFor(fam.name) : Manifest();

    const std::vector<uint32_t> selected = CryptomatteCore::resolveMatteIds(
        CryptomatteCore::parseMatteList(_imp->matteList.lock()->getValue()), manifest);
    const bool preview = _imp->preview.lock()->getValue();
    const bool matteOnly = _imp->matteOnly.lock()->getValue();
    const bool invert = _imp->invert.lock()->getValue();

    // Source RGBA (pass-through colour).
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false, eStorageModeRAM, 0, &srcRoi);

    // Ranked Cryptomatte planes.
    struct Rank { ImagePtr img; RectI bounds; int nComp; int idx[4]; };
    std::vector<Rank> ranks;
    if (haveFamily) {
        for (size_t r = 0; r < fam.ranks.size(); ++r) {
            RectI roi;
            ImagePtr img = getImage(0, args.time, args.mappedScale, args.view,
                                    NULL, &fam.ranks[r], false, false, eStorageModeRAM, 0, &roi);
            if (!img) continue;
            Rank rk; rk.img = img; rk.bounds = img->getBounds();
            rk.nComp = img->getComponents().getNumComponents();
            if (rk.nComp < 4) continue;
            cryptoChannelIndices(img->getComponents(), rk.idx);
            ranks.push_back(rk);
        }
    }

    std::vector<std::unique_ptr<Image::ReadAccess> > rankRa;
    for (size_t r = 0; r < ranks.size(); ++r) rankRa.emplace_back(new Image::ReadAccess(ranks[r].img.get()));
    std::unique_ptr<Image::ReadAccess> srcRa;
    if (srcImg) srcRa.reset(new Image::ReadAccess(srcImg.get()));
    const RectI srcBounds = srcImg ? srcImg->getBounds() : RectI();
    const int srcNComp = srcImg ? srcImg->getComponents().getNumComponents() : 0;

    for (std::list<std::pair<ImagePlaneDesc, ImagePtr> >::const_iterator pit = args.outputPlanes.begin();
         pit != args.outputPlanes.end(); ++pit) {
        const ImagePtr& outImg = pit->second;
        if (!outImg) continue;
        const RectI outBounds = outImg->getBounds();
        const int outNComp = outImg->getComponents().getNumComponents();
        Image::WriteAccess wa(outImg.get());

        for (int y = args.roi.y1; y < args.roi.y2; ++y) {
            if (y < outBounds.y1 || y >= outBounds.y2) continue;
            for (int x = args.roi.x1; x < args.roi.x2; ++x) {
                if (x < outBounds.x1 || x >= outBounds.x2) continue;
                float* dst = (float*)wa.pixelAt(x, y);
                if (!dst) continue;

                float matte = 0.f;
                float pr = 0.f, pg = 0.f, pb = 0.f;
                for (size_t r = 0; r < ranks.size(); ++r) {
                    if (!ranks[r].bounds.contains(x, y)) continue;
                    const float* p = (const float*)rankRa[r]->pixelAt(x, y);
                    if (!p) continue;
                    for (int pair = 0; pair < 2; ++pair) {
                        const float id = p[ranks[r].idx[pair * 2]];
                        const float cov = p[ranks[r].idx[pair * 2 + 1]];
                        if (cov == 0.f) continue;
                        const uint32_t bits = CryptomatteCore::floatToBits(id);
                        if (CryptomatteCore::containsId(selected, bits)) matte += cov;
                        if (preview && !matteOnly) {
                            float c[3];
                            CryptomatteCore::idToColor(bits, c);
                            pr += c[0] * cov; pg += c[1] * cov; pb += c[2] * cov;
                        }
                    }
                }
                matte = std::min(1.f, std::max(0.f, matte));
                if (invert) matte = 1.f - matte;

                float rgb[3] = { 0.f, 0.f, 0.f };
                float srcA = 1.f;
                const float* s = NULL;
                if (srcRa && srcBounds.contains(x, y)) s = (const float*)srcRa->pixelAt(x, y);
                if (s && srcNComp >= 3) { rgb[0] = s[0]; rgb[1] = s[1]; rgb[2] = s[2]; }
                if (s && srcNComp >= 4) srcA = s[3];
                (void)srcA;

                if (matteOnly) {
                    rgb[0] = rgb[1] = rgb[2] = matte;
                } else if (preview) {
                    // ID colours, the selection pushed towards white.
                    const float h = 0.65f * matte;
                    rgb[0] = pr * (1.f - h) + h;
                    rgb[1] = pg * (1.f - h) + h;
                    rgb[2] = pb * (1.f - h) + h;
                }

                if (outNComp >= 3) { dst[0] = rgb[0]; dst[1] = rgb[1]; dst[2] = rgb[2]; }
                if (outNComp >= 4) dst[3] = matte;
                else if (outNComp == 1) dst[0] = matte;
            }
        }
    }
    return eStatusOK;
}

// ==================== Viewer picking ====================

void
Cryptomatte::drawOverlay(double /*time*/, const RenderScale& /*renderScale*/, ViewIdx /*view*/)
{
    // Nothing drawn: picking only needs the clicks.
}

bool
Cryptomatte::onOverlayPenDown(double time,
                              const RenderScale& /*renderScale*/,
                              ViewIdx view,
                              const QPointF& /*viewportPos*/,
                              const QPointF& pos,
                              double /*pressure*/,
                              double /*timestamp*/,
                              PenType /*pen*/)
{
    KnobChoicePtr pk = _imp->picker.lock();
    const int mode = pk ? pk->getValue() : ePickerOff;
    if (mode == ePickerOff) return false;
    pickAt(time, view, pos, mode == ePickerAdd);
    return true;
}

void
Cryptomatte::pickAt(double time, ViewIdx view, const QPointF& pos, bool add)
{
    CryptoFamily fam;
    if (!_imp->currentFamily(_imp->selectedLayerName(), &fam) || fam.ranks.empty()) return;

    const int px = (int)std::floor(pos.x());
    const int py = (int)std::floor(pos.y());
    const RectD bounds(px, py, px + 1, py + 1);

    // The ID with the highest coverage under the cursor across all ranks.
    float bestCov = 0.f;
    uint32_t bestId = 0;
    for (size_t r = 0; r < fam.ranks.size(); ++r) {
        RectI roi;
        ImagePtr img = getImage(0, time, RenderScale(), view, &bounds, &fam.ranks[r],
                                false, true, eStorageModeRAM, 0, &roi);
        if (!img || !img->getBounds().contains(px, py)) continue;
        if (img->getComponents().getNumComponents() < 4) continue;
        int idx[4];
        cryptoChannelIndices(img->getComponents(), idx);
        Image::ReadAccess ra(img.get());
        const float* p = (const float*)ra.pixelAt(px, py);
        if (!p) continue;
        for (int pair = 0; pair < 2; ++pair) {
            const float cov = p[idx[pair * 2 + 1]];
            if (cov > bestCov) { bestCov = cov; bestId = CryptomatteCore::floatToBits(p[idx[pair * 2]]); }
        }
    }
    if (bestCov <= 0.f) return;

    // Name from the manifest, else the raw "<float>" token.
    const Manifest manifest = _imp->manifestFor(fam.name);
    std::string entry = CryptomatteCore::idToken(bestId);
    for (size_t m = 0; m < manifest.size(); ++m) {
        if (manifest[m].second == bestId) { entry = manifest[m].first; break; }
    }

    KnobStringPtr listKnob = _imp->matteList.lock();
    std::vector<std::string> entries = CryptomatteCore::parseMatteList(listKnob->getValue());
    if (add) {
        if (std::find(entries.begin(), entries.end(), entry) == entries.end()) entries.push_back(entry);
    } else {
        // Remove every plain entry that resolves to the picked ID (name or <float>).
        std::vector<std::string> kept;
        for (size_t i = 0; i < entries.size(); ++i) {
            const std::vector<std::string> one(1, entries[i]);
            const bool wildcard = entries[i].find_first_of("*?") != std::string::npos;
            const std::vector<uint32_t> ids = CryptomatteCore::resolveMatteIds(one, manifest);
            if (!wildcard && ids.size() == 1 && ids[0] == bestId) continue;
            kept.push_back(entries[i]);
        }
        entries.swap(kept);
    }
    listKnob->setValue(CryptomatteCore::formatMatteList(entries));
}

NATRON_NAMESPACE_EXIT
