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

#include "LD3DE4.h"

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include "../../AppManager.h"
#include "../../Image.h"
#include "../../KnobTypes.h"
#include "../../Node.h"

// LDPK 2.12 (libs/ldpk): Science-D-Visions' reference models, header-only.
#include <ldpk/tde4_ldp_classic_ld_model.h>
#include <ldpk/tde4_ldp_radial_standard_degree_4.h>
#include <ldpk/tde4_ldp_radial_fisheye_degree_8.h>
#include <ldpk/tde4_ldp_radial_fisheye_equidistant_degree_8.experimental.h>
#include <ldpk/tde4_ldp_radial_fisheye_equisolid_degree_8.experimental.h>
#include <ldpk/tde4_ldp_radial_fisheye_orthographic_degree_8.experimental.h>
#include <ldpk/tde4_ldp_radial_fisheye_stereographic_degree_8.experimental.h>
#include <ldpk/tde4_ldp_radial_homomorphic_decentered_degree_2.experimental.h>
#include <ldpk/tde4_ldp_radial_stabilizer_degree_6.h>
#include <ldpk/tde4_ldp_radial_deg_6_decentered_deg_2_opencv.h>
#include <ldpk/tde4_ldp_brown_conrady_degree_6_2.h>
#include <ldpk/tde4_ldp_anamorphic_degree_6.h>
#include <ldpk/tde4_ldp_anamorphic_homomorphic_degree_2.experimental.h>
#include <ldpk/tde4_ldp_anamorphic_rescaled_degree_4.h>
#include <ldpk/tde4_ldp_anamorphic_rescaled_degree_6.h>
#include <ldpk/tde4_ldp_anamorphic_stabilizer_degree_6.h>
#include <ldpk/tde4_ldp_anamorphic_standard_degree_4.h>
#include <ldpk/tde4_ldp_anamorphic_standard_degree_6.h>

NATRON_NAMESPACE_ENTER

namespace {

template <template <class, class> class M>
tde4_ld_plugin*
makeModel()
{
    return new M<ldpk::vec2d, ldpk::mat2d>();
}

struct LDModelDesc
{
    const char* nukeClass;              // node class in the .nk 3DE exports
    tde4_ld_plugin* (*create)();
};

// Same order as the Model menu. Never reorder: the menu value is saved by ID
// (the Nuke class), but keep it stable anyway.
const LDModelDesc kModels[] = {
    { "LD_3DE4_Radial_Standard_Degree_4",               &makeModel<tde4_ldp_radial_standard_degree_4> },
    { "LD_3DE4_Radial_Stabilizer_Degree_6",             &makeModel<tde4_ldp_radial_stabilizer_degree_6> },
    { "LD_3DE4_Radial_Homomorphic_Degree_2",            &makeModel<tde4_ldp_radial_homomorphic_decentered_degree_2> },
    { "LD_3DE4_Radial_Fisheye_Degree_8",                &makeModel<tde4_ldp_radial_fisheye_degree_8> },
    { "LD_3DE4_Radial_Fisheye_Equidistant_Degree_8",    &makeModel<tde4_ldp_radial_fisheye_equidistant_degree_8> },
    { "LD_3DE4_Radial_Fisheye_Equisolid_Degree_8",      &makeModel<tde4_ldp_radial_fisheye_equisolid_degree_8> },
    { "LD_3DE4_Radial_Fisheye_Orthographic_Degree_8",   &makeModel<tde4_ldp_radial_fisheye_orthographic_degree_8> },
    { "LD_3DE4_Radial_Fisheye_Stereographic_Degree_8",  &makeModel<tde4_ldp_radial_fisheye_stereographic_degree_8> },
    { "LD_3DE4_Anamorphic_Standard_Degree_4",           &makeModel<tde4_ldp_anamorphic_standard_degree_4> },
    { "LD_3DE4_Anamorphic_Standard_Degree_6",           &makeModel<tde4_ldp_anamorphic_standard_degree_6> },
    { "LD_3DE4_Anamorphic_Rescaled_Degree_4",           &makeModel<tde4_ldp_anamorphic_rescaled_degree_4> },
    { "LD_3DE4_Anamorphic_Rescaled_Degree_6",           &makeModel<tde4_ldp_anamorphic_rescaled_degree_6> },
    { "LD_3DE4_Anamorphic_Stabilizer_Degree_6",         &makeModel<tde4_ldp_anamorphic_stabilizer_degree_6> },
    { "LD_3DE4_Anamorphic_Degree_6",                    &makeModel<tde4_ldp_anamorphic_degree_6> },
    { "LD_3DE4_Anamorphic_Homomorphic_Degree_2",        &makeModel<tde4_ldp_anamorphic_homomorphic_degree_2> },
    { "LD_3DE4_Brown_Conrady_Degree_6_2",               &makeModel<tde4_ldp_brown_conrady_degree_6_2> },
    { "LD_OpenCV_Radial_Degree_6_Decentered_Degree_2",  &makeModel<tde4_ldp_radial_deg_6_decentered_deg_2_opencv> },
    { "LD_3DE_Classic_LD_Model",                        &makeModel<tde4_ldp_classic_ld_model> },
};
const int kNumModels = (int)(sizeof(kModels) / sizeof(kModels[0]));

// Nuke knob name of an LDPK parameter (nuke_ld_3de4_base::nukify_name):
// alphanumerics kept, any other run of characters -> one underscore.
std::string
nukifyName(const std::string& name)
{
    if ( name.empty() ) {
        return "_";
    }
    std::string s;
    bool space = true;
    if ( std::isdigit( (unsigned char)name[0] ) ) {
        s += '_';
    }
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if ( std::isalnum( (unsigned char)c ) ) {
            space = false;
            s += c;
        } else if (!space) {
            space = true;
            s += '_';
        }
    }
    return s;
}

// One knob per distinct model parameter across all models (all LDPK 2.12
// model parameters are adjustable doubles).
struct LDParamDesc
{
    std::string knobName;   // Nuke name, e.g. Quartic_Distortion_Degree_4
    std::string label;      // LDPK name, e.g. "Quartic Distortion - Degree 4"
    double def, lo, hi;
    std::vector<bool> inModel;   // [kNumModels]
};

struct LDCatalog
{
    std::vector<std::string> modelNames;   // LDPK model names, menu labels
    std::vector<LDParamDesc> params;
};

const LDCatalog&
catalog()
{
    static LDCatalog cat = []() {
        LDCatalog c;
        for (int m = 0; m < kNumModels; ++m) {
            std::unique_ptr<tde4_ld_plugin> p( kModels[m].create() );
            char buf[256] = { 0 };
            p->getModelName(buf);
            c.modelNames.push_back(buf);
            int np = 0;
            p->getNumParameters(np);
            for (int i = 0; i < np; ++i) {
                char pn[256] = { 0 };
                p->getParameterName(i, pn);
                tde4_ldp_ptype type;
                if ( !p->getParameterType(pn, type) || (type != TDE4_LDP_ADJUSTABLE_DOUBLE && type != TDE4_LDP_DOUBLE) ) {
                    continue;
                }
                const std::string kn = nukifyName(pn);
                LDParamDesc* d = 0;
                for (size_t j = 0; j < c.params.size(); ++j) {
                    if (c.params[j].knobName == kn) { d = &c.params[j]; break; }
                }
                if (!d) {
                    LDParamDesc nd;
                    nd.knobName = kn;
                    nd.label = pn;
                    nd.def = 0.; nd.lo = -1.; nd.hi = 1.;
                    p->getParameterDefaultValue(pn, nd.def);
                    if (type == TDE4_LDP_ADJUSTABLE_DOUBLE) {
                        p->getParameterRange(pn, nd.lo, nd.hi);
                    }
                    nd.inModel.assign(kNumModels, false);
                    c.params.push_back(nd);
                    d = &c.params.back();
                }
                d->inModel[m] = true;
            }
        }
        return c;
    }();
    return cat;
}

// The seven 3DE built-in parameters, Nuke knob names = LDPK names.
struct LDBuiltin { const char* name; const char* label; double def, lo, hi; };
const LDBuiltin kBuiltins[] = {
    { "tde4_focal_length_cm",          "tde4 focal length [cm]",        1.0,   0.5,  50.0 },
    { "tde4_custom_focus_distance_cm", "tde4 focus distance [cm]",    100.0,  10.0, 1000.0 },
    { "tde4_filmback_width_cm",        "tde4 filmback width [cm]",      0.8,   0.1,  10.0 },
    { "tde4_filmback_height_cm",       "tde4 filmback height [cm]",     0.6,   0.1,  10.0 },
    { "tde4_lens_center_offset_x_cm",  "tde4 lens center offset x [cm]", 0.0, -5.0,   5.0 },
    { "tde4_lens_center_offset_y_cm",  "tde4 lens center offset y [cm]", 0.0, -5.0,   5.0 },
    { "tde4_pixel_aspect",             "tde4 pixel aspect",              1.0,  0.25,  4.0 },
};
const int kNumBuiltins = (int)(sizeof(kBuiltins) / sizeof(kBuiltins[0]));

} // anon namespace

struct LD3DE4Private
{
    KnobChoiceWPtr model;
    KnobChoiceWPtr direction;   // 0 distort, 1 undistort (Nuke order)
    KnobChoiceWPtr outputMode;  // 0 image, 1 STMap
    KnobDoubleWPtr fov[4];      // xa, ya, xb, yb (unit coordinates)
    KnobDoubleWPtr builtins[kNumBuiltins];
    std::vector<KnobDoubleWPtr> params;   // parallel to catalog().params

    int modelIndex() const
    {
        KnobChoicePtr k = model.lock();
        const int i = k ? k->getValue() : 0;
        return (i >= 0 && i < kNumModels) ? i : 0;
    }
};

LD3DE4::LD3DE4(NodePtr node)
    : EffectInstance(node)
    , _imp(new LD3DE4Private())
{
}

LD3DE4::~LD3DE4()
{
}

std::vector<std::string>
LD3DE4::supportedNukeClasses()
{
    std::vector<std::string> v;
    for (int m = 0; m < kNumModels; ++m) {
        v.push_back(kModels[m].nukeClass);
    }
    return v;
}

std::string
LD3DE4::getPluginDescription() const
{
    return "3DEqualizer lens distortion: every model of the Lens Distortion Plugin Kit (LDPK 2.12, "
           "Science-D-Visions) in one node, the equivalent of the Nuke LD_3DE4_* nodes exported by 3DE.\n\n"
           "Models: Radial Standard 4, Radial Stabilizer 6, Radial Homomorphic 2, Radial Fisheye 8 "
           "(and its Equidistant / Equisolid / Orthographic / Stereographic variants), Anamorphic Standard "
           "4 / 6, Anamorphic Rescaled 4 / 6, Anamorphic Stabilizer 6, Anamorphic 6, Anamorphic "
           "Homomorphic 2, Brown-Conrady 6/2, OpenCV Radial 6 Decentered 2, 3DE Classic.\n\n"
           "Knob names match the Nuke nodes, so the 3DE menu's \"Import Lens Distortion 3DE4 (.nk)\" "
           "fills this node from a 3DE export. Undistort removes the distortion (plate -> pinhole), "
           "Distort applies it (CG -> plate). Output Mode STMap writes the source position (R, G) "
           "for an STMap node.";
}

void
LD3DE4::addAcceptedComponents(int /*inputNb*/, std::list<ImagePlaneDesc>* comps)
{
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    comps->push_back(ImagePlaneDesc::getRGBComponents());
    comps->push_back(ImagePlaneDesc::getAlphaComponents());
}

void
LD3DE4::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
}

void
LD3DE4::getRegionsOfInterest(double time,
                             const RenderScale & scale,
                             const RectD & /*outputRoD*/,
                             const RectD & /*renderWindow*/,
                             ViewIdx view,
                             RoIMap* ret)
{
    // A distorted pixel can come from anywhere in the source: ask for all of it.
    EffectInstancePtr input = getInput(0);
    if (!input) {
        return;
    }
    RectD rod;
    bool isPF;
    const RenderScale inpScale = input->supportsRenderScale() ? scale : RenderScale::identity;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, inpScale, view, &rod, &isPF) == eStatusOK) {
        ret->insert( std::make_pair(input, rod) );
    }
}

void
LD3DE4::initializeKnobs()
{
    const LDCatalog& cat = catalog();
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("LD_3DE4"));

    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Model"));
        k->setName("ldModel");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> e;
        for (int m = 0; m < kNumModels; ++m) {
            e.push_back( ChoiceOption(kModels[m].nukeClass, cat.modelNames[m], kModels[m].nukeClass) );
        }
        k->populateChoices(e);
        k->setDefaultValue( modelFromPluginID() );
        k->setHintToolTip(tr("3DEqualizer lens distortion model (the Nuke node class is in each entry's tooltip)."));
        page->addKnob(k);
        _imp->model = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Direction"));
        k->setName("direction");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> e;
        e.push_back( ChoiceOption("distort", "Distort", "Apply the lens distortion (undistorted CG -> plate).") );
        e.push_back( ChoiceOption("undistort", "Undistort", "Remove the lens distortion (plate -> pinhole).") );
        k->populateChoices(e);
        k->setDefaultValue(1);
        k->setAddNewLine(false);
        page->addKnob(k);
        _imp->direction = k;
    }
    {
        KnobChoicePtr k = AppManager::createKnob<KnobChoice>(this, tr("Output Mode"));
        k->setName("mode");
        k->setAnimationEnabled(false);
        std::vector<ChoiceOption> e;
        e.push_back( ChoiceOption("image", "Image", "The warped image.") );
        e.push_back( ChoiceOption("STMap", "STMap", "R, G = normalized source position, for an STMap node.") );
        k->populateChoices(e);
        k->setDefaultValue(0);
        page->addKnob(k);
        _imp->outputMode = k;
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Camera"));
        sep->setName("sepCamera");
        page->addKnob(sep);
    }
    for (int i = 0; i < kNumBuiltins; ++i) {
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, std::string(kBuiltins[i].label) );
        k->setName(kBuiltins[i].name);
        k->setDefaultValue(kBuiltins[i].def);
        k->setDisplayMinimum(kBuiltins[i].lo);
        k->setDisplayMaximum(kBuiltins[i].hi);
        page->addKnob(k);
        _imp->builtins[i] = k;
    }
    {
        static const char* names[4]  = { "field_of_view_xa_unit", "field_of_view_ya_unit", "field_of_view_xb_unit", "field_of_view_yb_unit" };
        static const char* labels[4] = { "fov left [unit coord]", "fov bottom [unit coord]", "fov right [unit coord]", "fov top [unit coord]" };
        static const double defs[4]  = { 0., 0., 1., 1. };
        for (int i = 0; i < 4; ++i) {
            KnobDoublePtr k = AppManager::createKnob<KnobDouble>( this, std::string(labels[i]) );
            k->setName(names[i]);
            k->setDefaultValue(defs[i]);
            k->setDisplayMinimum(-0.2);
            k->setDisplayMaximum(1.2);
            k->setHintToolTip(tr("3DE field of view (unit coordinates); 0 / 0 / 1 / 1 = the whole image."));
            page->addKnob(k);
            _imp->fov[i] = k;
        }
    }
    {
        KnobSeparatorPtr sep = AppManager::createKnob<KnobSeparator>(this, tr("Lens"));
        sep->setName("sepLens");
        page->addKnob(sep);
    }
    _imp->params.clear();
    for (size_t i = 0; i < cat.params.size(); ++i) {
        const LDParamDesc& d = cat.params[i];
        KnobDoublePtr k = AppManager::createKnob<KnobDouble>(this, d.label);
        k->setName(d.knobName);
        k->setDefaultValue(d.def);
        k->setDisplayMinimum(d.lo);
        k->setDisplayMaximum(d.hi);
        k->setHintToolTip( tr("LDPK parameter \"%1\" (Nuke knob %2).")
                           .arg( QString::fromUtf8( d.label.c_str() ) )
                           .arg( QString::fromUtf8( d.knobName.c_str() ) ) );
        page->addKnob(k);
        _imp->params.push_back(k);
    }
    refreshModelKnobs();
}

int
LD3DE4::modelFromPluginID() const
{
    NodePtr n = getNode();
    if (!n) {
        return 0;
    }
    const std::string id = n->getPluginID();
    const std::string prefix = std::string(PLUGINID_NATRON_LD3DE4) + ".";
    if ( id.compare(0, prefix.size(), prefix) != 0 ) {
        return 0;
    }
    const std::string cls = id.substr( prefix.size() );
    for (int m = 0; m < kNumModels; ++m) {
        if (cls == kModels[m].nukeClass) {
            return m;
        }
    }
    return 0;
}

void
LD3DE4::refreshModelKnobs()
{
    const LDCatalog& cat = catalog();
    const int m = _imp->modelIndex();
    for (size_t i = 0; i < _imp->params.size() && i < cat.params.size(); ++i) {
        if ( KnobDoublePtr k = _imp->params[i].lock() ) {
            k->setSecret(!cat.params[i].inModel[m]);
        }
    }
}

bool
LD3DE4::knobChanged(KnobI* k,
                    ValueChangedReasonEnum /*reason*/,
                    ViewSpec /*view*/,
                    double /*time*/,
                    bool /*originatedFromMainThread*/)
{
    if ( k && _imp->model.lock() && k == _imp->model.lock().get() ) {
        refreshModelKnobs();
        return false;
    }
    return false;
}

void
LD3DE4::onKnobsLoaded()
{
    refreshModelKnobs();
}

StatusEnum
LD3DE4::render(const RenderActionArgs& args)
{
    if ( args.outputPlanes.empty() ) {
        return eStatusFailed;
    }
    ImagePtr outImg = args.outputPlanes.front().second;
    if (!outImg) {
        return eStatusFailed;
    }
    RectI srcRoi;
    ImagePtr srcImg = getImage(0, args.time, args.mappedScale, args.view,
                               NULL, NULL, false, false, eStorageModeRAM, 0, &srcRoi);

    // Image frame in pixels at this render scale: the output format
    // (the plate), else the source bounds.
    const double scale = args.mappedScale.toOfxPointD().x;
    RectI fmt = getOutputFormat();
    double fx1, fy1, fw, fh;
    if ( !fmt.isNull() && fmt.width() > 0 && fmt.height() > 0 ) {
        fx1 = fmt.x1 * scale; fy1 = fmt.y1 * scale;
        fw = fmt.width() * scale; fh = fmt.height() * scale;
    } else if (srcImg) {
        const RectI b = srcImg->getBounds();
        fx1 = b.x1; fy1 = b.y1; fw = b.width(); fh = b.height();
    } else {
        return eStatusFailed;
    }
    if (fw <= 0 || fh <= 0) {
        return eStatusOK;
    }

    // One model instance per call (LDPK models keep caches: not shared between threads).
    const int m = _imp->modelIndex();
    std::unique_ptr<tde4_ld_plugin> ldm( kModels[m].create() );
    for (int i = 0; i < kNumBuiltins; ++i) {
        KnobDoublePtr k = _imp->builtins[i].lock();
        ldm->setParameterValue(kBuiltins[i].name, k ? k->getValueAtTime(args.time) : kBuiltins[i].def);
    }
    const LDCatalog& cat = catalog();
    for (size_t i = 0; i < cat.params.size() && i < _imp->params.size(); ++i) {
        if (!cat.params[i].inModel[m]) {
            continue;
        }
        KnobDoublePtr k = _imp->params[i].lock();
        ldm->setParameterValue(cat.params[i].label.c_str(), k ? k->getValueAtTime(args.time) : cat.params[i].def);
    }
    if ( !ldm->initializeParameters() ) {
        return eStatusFailed;   // e.g. Squeeze = 0
    }

    double fov[4] = { 0., 0., 1., 1. };
    for (int i = 0; i < 4; ++i) {
        if ( KnobDoublePtr k = _imp->fov[i].lock() ) {
            fov[i] = k->getValueAtTime(args.time);
        }
    }
    const double fovW = fov[2] - fov[0], fovH = fov[3] - fov[1];
    if ( std::abs(fovW) < 1e-9 || std::abs(fovH) < 1e-9 ) {
        return eStatusFailed;
    }

    const bool undistort = _imp->direction.lock() ? (_imp->direction.lock()->getValue() == 1) : true;
    const bool stmap = _imp->outputMode.lock() && _imp->outputMode.lock()->getValue() == 1;

    const RectI outBounds = outImg->getBounds();
    const RectI win = args.roi.intersect(outBounds);
    if ( win.isNull() ) {
        return eStatusOK;
    }
    const int outN = std::min( (int)outImg->getComponentsCount(), 4 );

    std::unique_ptr<Image::ReadAccess> ra;
    RectI srcBounds;
    int srcN = 0;
    if (srcImg) {
        ra.reset( new Image::ReadAccess( srcImg.get() ) );
        srcBounds = srcImg->getBounds();
        srcN = std::min( (int)srcImg->getComponentsCount(), 4 );
    }
    static const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
    auto pix = [&](int cx, int cy) -> const float* {
        if ( !ra || cx < srcBounds.x1 || cx >= srcBounds.x2 || cy < srcBounds.y1 || cy >= srcBounds.y2 ) {
            return zero;
        }
        const float* p = (const float*)ra->pixelAt(cx, cy);
        return p ? p : zero;
    };

    Image::WriteAccess wa( outImg.get() );
    for (int y = win.y1; y < win.y2; ++y) {
        if ( (y & 15) == 0 && aborted() ) {
            return eStatusFailed;
        }
        // Unit coordinates of the pixel centre (Nuke LD node convention), in 3DE's fov.
        const double v = ( (y - fy1) + 0.5 ) / fh;
        const double vf = (v - fov[1]) / fovH;
        for (int x = win.x1; x < win.x2; ++x) {
            float* dst = (float*)wa.pixelAt(x, y);
            if (!dst) {
                continue;
            }
            const double u = ( (x - fx1) + 0.5 ) / fw;
            const double uf = (u - fov[0]) / fovW;
            double xd = 0., yd = 0.;
            // Image processing fetches: undistorted output pixel -> distorted source (distort), and back.
            const bool ok = undistort ? ldm->distort(uf, vf, xd, yd) : ldm->undistort(uf, vf, xd, yd);
            // Back to pixels (continuous coordinates, pixel centres at +0.5).
            const double sx = (xd * fovW + fov[0]) * fw + fx1;
            const double sy = (yd * fovH + fov[1]) * fh + fy1;

            if (stmap) {
                const float st[4] = { (float)( (sx - fx1) / fw ), (float)( (sy - fy1) / fh ), 0.f, ok ? 1.f : 0.f };
                for (int c = 0; c < outN; ++c) {
                    dst[c] = (outN == 1) ? st[3] : st[c];
                }
                continue;
            }
            if (!ok || srcN == 0) {
                for (int c = 0; c < outN; ++c) dst[c] = 0.f;
                continue;
            }
            // Bilinear sample.
            const double bx = sx - 0.5, by = sy - 0.5;
            const int ix = (int)std::floor(bx), iy = (int)std::floor(by);
            const double tx = bx - ix, ty = by - iy;
            const float* p00 = pix(ix, iy);
            const float* p10 = pix(ix + 1, iy);
            const float* p01 = pix(ix, iy + 1);
            const float* p11 = pix(ix + 1, iy + 1);
            for (int c = 0; c < outN; ++c) {
                const int sc = std::min(c, srcN - 1);
                const double bot = p00[sc] * (1.0 - tx) + p10[sc] * tx;
                const double top = p01[sc] * (1.0 - tx) + p11[sc] * tx;
                dst[c] = (float)( bot * (1.0 - ty) + top * ty );
            }
        }
    }
    return eStatusOK;
}

NATRON_NAMESPACE_EXIT

#include "moc_LD3DE4.cpp"
