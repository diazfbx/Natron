// braw2exr - convert a Blackmagic RAW (.braw) clip to an OpenEXR sequence.
//
// MIT licence (see LICENSE). Written for the Natron fork's CameraRAW_Import
// PyPlug, usable on its own.
//
// Built against Blackmagic Design's free Blackmagic RAW SDK, which you
// download yourself from https://www.blackmagicdesign.com/developer/products/braw
// (its own licence). This repository does not contain any SDK file: the
// SDK's include folder is given to CMake (see README.md).
//
// Usage:
//   braw2exr --list <clip.braw>
//       prints the clip (size, frames, fps) and the gamma / gamut values the
//       SDK offers for it, one per line ("gamma: ...", "gamut: ...").
//   braw2exr --input <clip.braw> --output <dir/name.%04d.exr>
//            [--first-number 1001] [--start 0] [--count 0 (all)]
//            [--scale full|half|quarter|eighth] [--gamma "<name>"] [--gamut "<name>"]
//            [--float]          32-bit float EXR instead of 16-bit half
//   Progress goes to stdout ("frame i/n"). Exit code 0 on success.

#include <BlackmagicRawAPI.h>

#include "exr_write.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <oleauto.h>
#endif

// ------------------------------------------------------------------ strings / variants per platform
#if defined(_WIN32)
typedef BSTR BrawString;
static BrawString makeString(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::vector<wchar_t> w(n > 0 ? n : 1, 0);
    if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    }
    return SysAllocString(w.data());
}
static void freeString(BrawString s) { if (s) SysFreeString(s); }
static std::string toStd(BrawString s)
{
    if (!s) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    std::string out(n > 0 ? n : 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, NULL, NULL);
    out.resize(std::strlen(out.c_str()));
    return out;
}
typedef VARIANT BrawVariant;
static void variantInit(BrawVariant* v) { VariantInit(v); }
static void variantClear(BrawVariant* v) { VariantClear(v); }
static void variantSetString(BrawVariant* v, BrawString s) { v->vt = VT_BSTR; v->bstrVal = s; }
static bool variantIsString(const BrawVariant& v) { return v.vt == VT_BSTR; }
static BrawString variantString(const BrawVariant& v) { return v.bstrVal; }
#elif defined(__APPLE__)
typedef CFStringRef BrawString;
static BrawString makeString(const std::string& s)
{
    return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(s.c_str()), s.size(),
                                   kCFStringEncodingUTF8, false);
}
static void freeString(BrawString s) { if (s) CFRelease(s); }
static std::string toStd(BrawString s)
{
    if (!s) return std::string();
    char buf[1024];
    if (CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8)) return buf;
    return std::string();
}
typedef Variant BrawVariant;
static void variantInit(BrawVariant* v) { VariantInit(v); }
static void variantClear(BrawVariant* v) { VariantClear(v); }
static void variantSetString(BrawVariant* v, BrawString s) { v->vt = blackmagicRawVariantTypeString; v->bstrVal = s; }
static bool variantIsString(const BrawVariant& v) { return v.vt == blackmagicRawVariantTypeString; }
static BrawString variantString(const BrawVariant& v) { return v.bstrVal; }
#else // Linux
typedef const char* BrawString;
static BrawString makeString(const std::string& s) { return strdup(s.c_str()); }
static void freeString(BrawString s) { std::free(const_cast<char*>(s)); }
static std::string toStd(BrawString s) { return s ? std::string(s) : std::string(); }
typedef Variant BrawVariant;
static void variantInit(BrawVariant* v) { VariantInit(v); }
static void variantClear(BrawVariant* v) { VariantClear(v); }
static void variantSetString(BrawVariant* v, BrawString s) { v->vt = blackmagicRawVariantTypeString; v->bstrVal = s; }
static bool variantIsString(const BrawVariant& v) { return v.vt == blackmagicRawVariantTypeString; }
static BrawString variantString(const BrawVariant& v) { return v.bstrVal; }
#endif

// ------------------------------------------------------------------ options
struct Options {
    std::string input, output;
    bool list = false;
    long long firstNumber = 1001;
    long long start = 0;
    long long count = 0;
    std::string scale = "full";
    std::string gamma, gamut;
    bool useFloat = false;
};

static void usage()
{
    std::printf(
        "braw2exr - Blackmagic RAW to OpenEXR (Blackmagic RAW SDK)\n"
        "  braw2exr --list <clip.braw>\n"
        "  braw2exr --input <clip.braw> --output <dir/name.%%04d.exr> [--first-number 1001]\n"
        "           [--start 0] [--count 0] [--scale full|half|quarter|eighth]\n"
        "           [--gamma \"<name>\"] [--gamut \"<name>\"] [--float]\n");
}

static bool parse(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--list") { o.list = true; if (i + 1 < argc && argv[i + 1][0] != '-') o.input = argv[++i]; }
        else if (a == "--input" || a == "-i") { if (!next(o.input)) return false; }
        else if (a == "--output" || a == "-o") { if (!next(o.output)) return false; }
        else if (a == "--first-number") { if (!next(v)) return false; o.firstNumber = std::atoll(v.c_str()); }
        else if (a == "--start") { if (!next(v)) return false; o.start = std::atoll(v.c_str()); }
        else if (a == "--count") { if (!next(v)) return false; o.count = std::atoll(v.c_str()); }
        else if (a == "--scale") { if (!next(o.scale)) return false; }
        else if (a == "--gamma") { if (!next(o.gamma)) return false; }
        else if (a == "--gamut") { if (!next(o.gamut)) return false; }
        else if (a == "--float") { o.useFloat = true; }
        else if (a == "--help" || a == "-h") { return false; }
        else if (o.input.empty() && a[0] != '-') { o.input = a; }
        else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); return false; }
    }
    if (o.input.empty()) return false;
    if (!o.list && o.output.empty()) return false;
    return true;
}

static std::string frameName(const std::string& pattern, long long n)
{
    // printf-style %0Nd, or #### (one # per digit)
    char buf[4096];
    size_t pct = pattern.find('%');
    if (pct != std::string::npos) {
        std::snprintf(buf, sizeof(buf), pattern.c_str(), n);
        return buf;
    }
    size_t h = pattern.find('#');
    if (h != std::string::npos) {
        size_t e = h;
        while (e < pattern.size() && pattern[e] == '#') ++e;
        std::snprintf(buf, sizeof(buf), "%0*lld", (int)(e - h), n);
        return pattern.substr(0, h) + buf + pattern.substr(e);
    }
    std::snprintf(buf, sizeof(buf), ".%04lld", n);
    size_t dot = pattern.rfind('.');
    if (dot == std::string::npos) return pattern + buf + ".exr";
    return pattern.substr(0, dot) + buf + pattern.substr(dot);
}

// ------------------------------------------------------------------ callback
class Callback : public IBlackmagicRawCallback {
public:
    BlackmagicRawResolutionScale scale = blackmagicRawResolutionScaleFull;
    IBlackmagicRawClipProcessingAttributes* clipAttrs = nullptr;
    std::string path;
    bool useFloat = false;
    std::string gammaName, gamutName;
    bool ok = false;
    std::string error;

    virtual ~Callback() {}

    void ReadComplete(IBlackmagicRawJob* job, HRESULT result, IBlackmagicRawFrame* frame) override
    {
        IBlackmagicRawJob* decodeJob = nullptr;
        if (result == S_OK) result = frame->SetResourceFormat(blackmagicRawResourceFormatRGBF32);
        if (result == S_OK && scale != blackmagicRawResolutionScaleFull) result = frame->SetResolutionScale(scale);
        if (result == S_OK) result = frame->CreateJobDecodeAndProcessFrame(clipAttrs, nullptr, &decodeJob);
        if (result == S_OK) result = decodeJob->Submit();
        if (result != S_OK) {
            if (decodeJob) decodeJob->Release();
            error = "decode job failed";
        }
        job->Release();
    }

    void ProcessComplete(IBlackmagicRawJob* job, HRESULT result, IBlackmagicRawProcessedImage* image) override
    {
        uint32_t w = 0, h = 0;
        void* data = nullptr;
        if (result == S_OK) result = image->GetWidth(&w);
        if (result == S_OK) result = image->GetHeight(&h);
        if (result == S_OK) result = image->GetResource(&data);
        if (result == S_OK && data && w && h) {
            std::vector<std::pair<std::string, std::string> > attrs;
            attrs.push_back(std::make_pair(std::string("software"), std::string("braw2exr (Blackmagic RAW SDK)")));
            if (!gammaName.empty()) attrs.push_back(std::make_pair(std::string("braw/gamma"), gammaName));
            if (!gamutName.empty()) attrs.push_back(std::make_pair(std::string("braw/gamut"), gamutName));
            ok = exrw::writeRGB(path, (int)w, (int)h, static_cast<const float*>(data), !useFloat, attrs);
            if (!ok) error = "cannot write " + path;
        } else {
            error = "processing failed";
        }
        job->Release();
    }

    void DecodeComplete(IBlackmagicRawJob*, HRESULT) override {}
    void TrimProgress(IBlackmagicRawJob*, float) override {}
    void TrimComplete(IBlackmagicRawJob*, HRESULT) override {}
    void SidecarMetadataParseWarning(IBlackmagicRawClip*, BrawString, uint32_t, BrawString) override {}
    void SidecarMetadataParseError(IBlackmagicRawClip*, BrawString, uint32_t, BrawString) override {}
    void PreparePipelineComplete(void*, HRESULT) override {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) override { return E_NOTIMPL; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }

private:
    ULONG refs = 0;
};

// ------------------------------------------------------------------ helpers
static IBlackmagicRawFactory* createFactory()
{
#if defined(_WIN32)
    IBlackmagicRawFactory* f = nullptr;
    HRESULT r = CoCreateInstance(CLSID_CBlackmagicRawFactory, nullptr, CLSCTX_ALL, IID_IBlackmagicRawFactory,
                                 reinterpret_cast<void**>(&f));
    return r == S_OK ? f : nullptr;
#elif defined(BRAW2EXR_LIBRARY_PATH)
    BrawString p = makeString(BRAW2EXR_LIBRARY_PATH);
    IBlackmagicRawFactory* f = CreateBlackmagicRawFactoryInstanceFromPath(p);
    freeString(p);
    return f ? f : CreateBlackmagicRawFactoryInstance();
#else
    return CreateBlackmagicRawFactoryInstance();
#endif
}

// GetClipAttributeList's "isReadOnly" type differs between platforms
// (bool / boolean / BOOL): take it from the SDK's own declaration.
template <typename C, typename A, typename V, typename B>
static B* listBoolOf(HRESULT (STDMETHODCALLTYPE C::*)(A, V*, uint32_t*, B*));
typedef std::remove_pointer<decltype(listBoolOf(&IBlackmagicRawClipProcessingAttributes::GetClipAttributeList))>::type ListBool;

static std::vector<std::string> attributeList(IBlackmagicRawClipProcessingAttributes* a,
                                              BlackmagicRawClipProcessingAttribute attr)
{
    std::vector<std::string> out;
    uint32_t n = 0;
    ListBool readOnly = ListBool();
    if (a->GetClipAttributeList(attr, nullptr, &n, &readOnly) != S_OK || n == 0) return out;
    std::vector<BrawVariant> vals(n);
    for (uint32_t i = 0; i < n; ++i) variantInit(&vals[i]);
    if (a->GetClipAttributeList(attr, vals.data(), &n, &readOnly) == S_OK) {
        for (uint32_t i = 0; i < n; ++i) {
            if (variantIsString(vals[i])) out.push_back(toStd(variantString(vals[i])));
        }
    }
    for (uint32_t i = 0; i < vals.size(); ++i) variantClear(&vals[i]);
    return out;
}

static std::string currentAttribute(IBlackmagicRawClipProcessingAttributes* a, BlackmagicRawClipProcessingAttribute attr)
{
    BrawVariant v;
    variantInit(&v);
    std::string s;
    if (a->GetClipAttribute(attr, &v) == S_OK && variantIsString(v)) s = toStd(variantString(v));
    variantClear(&v);
    return s;
}

static bool setAttribute(IBlackmagicRawClipProcessingAttributes* a, BlackmagicRawClipProcessingAttribute attr,
                         const std::string& value)
{
    BrawVariant v;
    variantInit(&v);
    variantSetString(&v, makeString(value));
    bool ok = a->SetClipAttribute(attr, &v) == S_OK;
#if defined(_WIN32)
    VariantClear(&v);
#else
    freeString(variantString(v));
#endif
    return ok;
}

// ------------------------------------------------------------------ main
int main(int argc, char** argv)
{
    Options o;
    if (!parse(argc, argv, o)) {
        usage();
        return 2;
    }
#if defined(_WIN32)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    int rc = 1;
    IBlackmagicRawFactory* factory = createFactory();
    IBlackmagicRaw* codec = nullptr;
    IBlackmagicRawClip* clip = nullptr;
    IBlackmagicRawClipProcessingAttributes* attrs = nullptr;
    Callback* cb = nullptr;
    BrawString clipName = nullptr;
    do {
        if (!factory) {
            std::fprintf(stderr, "braw2exr: cannot load the Blackmagic RAW library (install Blackmagic RAW "
                                 "from blackmagicdesign.com)\n");
            break;
        }
        if (factory->CreateCodec(&codec) != S_OK) { std::fprintf(stderr, "braw2exr: CreateCodec failed\n"); break; }
        clipName = makeString(o.input);
        if (codec->OpenClip(clipName, &clip) != S_OK) {
            std::fprintf(stderr, "braw2exr: cannot open %s\n", o.input.c_str());
            break;
        }
        if (clip->CloneClipProcessingAttributes(&attrs) != S_OK || !attrs) {
            std::fprintf(stderr, "braw2exr: no clip processing attributes\n");
            break;
        }
        uint32_t w = 0, h = 0;
        uint64_t frames = 0;
        float fps = 0.f;
        clip->GetWidth(&w);
        clip->GetHeight(&h);
        clip->GetFrameCount(&frames);
        clip->GetFrameRate(&fps);

        if (o.list) {
            std::printf("size: %u x %u\nframes: %llu\nfps: %g\n", w, h, (unsigned long long)frames, fps);
            std::printf("current gamma: %s\n", currentAttribute(attrs, blackmagicRawClipProcessingAttributeGamma).c_str());
            std::printf("current gamut: %s\n", currentAttribute(attrs, blackmagicRawClipProcessingAttributeGamut).c_str());
            std::vector<std::string> g = attributeList(attrs, blackmagicRawClipProcessingAttributeGamma);
            for (size_t i = 0; i < g.size(); ++i) std::printf("gamma: %s\n", g[i].c_str());
            g = attributeList(attrs, blackmagicRawClipProcessingAttributeGamut);
            for (size_t i = 0; i < g.size(); ++i) std::printf("gamut: %s\n", g[i].c_str());
            rc = 0;
            break;
        }

        // Gamut first: some gammas are only valid for some gamuts.
        if (!o.gamut.empty() && !setAttribute(attrs, blackmagicRawClipProcessingAttributeGamut, o.gamut)) {
            std::fprintf(stderr, "braw2exr: gamut \"%s\" refused (see --list)\n", o.gamut.c_str());
            break;
        }
        if (!o.gamma.empty() && !setAttribute(attrs, blackmagicRawClipProcessingAttributeGamma, o.gamma)) {
            std::fprintf(stderr, "braw2exr: gamma \"%s\" refused (see --list)\n", o.gamma.c_str());
            break;
        }

        cb = new Callback();
        cb->AddRef();
        cb->clipAttrs = attrs;
        cb->useFloat = o.useFloat;
        cb->gammaName = currentAttribute(attrs, blackmagicRawClipProcessingAttributeGamma);
        cb->gamutName = currentAttribute(attrs, blackmagicRawClipProcessingAttributeGamut);
        if (o.scale == "half") cb->scale = blackmagicRawResolutionScaleHalf;
        else if (o.scale == "quarter") cb->scale = blackmagicRawResolutionScaleQuarter;
        else if (o.scale == "eighth") cb->scale = blackmagicRawResolutionScaleEighth;
        if (codec->SetCallback(cb) != S_OK) { std::fprintf(stderr, "braw2exr: SetCallback failed\n"); break; }

        long long first = o.start < 0 ? 0 : o.start;
        long long last = (long long)frames - 1;
        if (o.count > 0 && first + o.count - 1 < last) last = first + o.count - 1;
        if (first > last) { std::fprintf(stderr, "braw2exr: empty frame range\n"); break; }
        const long long total = last - first + 1;
        rc = 0;
        for (long long f = first; f <= last; ++f) {
            cb->path = frameName(o.output, o.firstNumber + (f - first));
            cb->ok = false;
            cb->error.clear();
            IBlackmagicRawJob* job = nullptr;
            if (clip->CreateJobReadFrame((uint64_t)f, &job) != S_OK || job->Submit() != S_OK) {
                if (job) job->Release();
                std::fprintf(stderr, "braw2exr: cannot read frame %lld\n", f);
                rc = 1;
                break;
            }
            codec->FlushJobs();
            if (!cb->ok) {
                std::fprintf(stderr, "braw2exr: frame %lld: %s\n", f, cb->error.c_str());
                rc = 1;
                break;
            }
            std::printf("frame %lld/%lld  %s\n", f - first + 1, total, cb->path.c_str());
            std::fflush(stdout);
        }
    } while (false);

    if (codec) codec->SetCallback(nullptr);
    if (cb) cb->Release();
    if (attrs) attrs->Release();
    if (clip) clip->Release();
    if (codec) codec->Release();
    if (factory) factory->Release();
    if (clipName) freeString(clipName);
#if defined(_WIN32)
    CoUninitialize();
#endif
    return rc;
}
