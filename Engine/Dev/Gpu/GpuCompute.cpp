/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#include "GpuCompute.h"

#ifndef NATRON_HAVE_WGPU

namespace gpuc {

static const char* kNoGpu = "this build has no GPU support (wgpu)";

bool available(std::string* why)
{
    if (why) {
        *why = kNoGpu;
    }
    return false;
}

std::string description()
{
    return std::string("none (") + kNoGpu + ")";
}

struct Session::Imp
{
    std::string err = kNoGpu;
};

Session::Session() : _imp(new Imp()) {}
Session::~Session() {}
bool Session::ok() const { return false; }
const std::string& Session::error() const { return _imp->err; }
std::uint64_t Session::maxBinding() const { return 0; }
Buffer Session::create(std::size_t, const void*) { return Buffer(); }
void Session::upload(const Buffer&, const void*, std::size_t) {}
void Session::clear(const Buffer&) {}
void Session::copy(const Buffer&, const Buffer&, std::size_t) {}
void Session::dispatch(const char*, const char*, std::initializer_list<Buffer>, const void*, std::size_t,
                       std::uint32_t, std::uint32_t, std::uint32_t) {}
bool Session::flush() { return false; }
bool Session::download(const Buffer&, void*, std::size_t) { return false; }

} // namespace gpuc

#else // NATRON_HAVE_WGPU

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "webgpu.h"
#include "wgpu.h"

namespace gpuc {

namespace {

WGPUStringView
sv(const char* s)
{
    WGPUStringView v;
    v.data = s;
    v.length = std::strlen(s);
    return v;
}

std::string
str(WGPUStringView v)
{
    if (!v.data) {
        return std::string();
    }
    return std::string(v.data, v.length == WGPU_STRLEN ? std::strlen(v.data) : v.length);
}

void
onAdapter(WGPURequestAdapterStatus, WGPUAdapter a, WGPUStringView, void* u, void*)
{
    *(WGPUAdapter*)u = a;
}

void
onDevice(WGPURequestDeviceStatus, WGPUDevice d, WGPUStringView, void* u, void*)
{
    *(WGPUDevice*)u = d;
}

void
onMap(WGPUMapAsyncStatus status, WGPUStringView, void* u, void*)
{
    *(int*)u = status == WGPUMapAsyncStatus_Success ? 1 : -1;
}

struct Context
{
    std::mutex lock;
    bool tried = false;
    std::atomic<bool> ready{false};     // init done: name / initError / device fixed
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPULimits limits = {};
    std::string name, initError;
    std::string error;      // since the last check
    std::map<std::string, WGPUComputePipeline> pipelines;

    static void onError(WGPUDevice const*, WGPUErrorType, WGPUStringView message, void* u, void*)
    {
        Context* c = (Context*)u;
        if ( c->error.empty() ) {
            c->error = str(message);
        }
    }

    void init()
    {
        tried = true;
        struct Done { std::atomic<bool>& r; ~Done() { r = true; } } done = { ready };
        WGPUInstanceDescriptor id = {};
        WGPUInstanceExtras ex = {};
        if ( const char* be = std::getenv("NATRON_GPU_BACKEND") ) {
            const std::string b(be);
            ex.chain.sType = (WGPUSType)WGPUSType_InstanceExtras;
            ex.backends = b == "gl" ? WGPUInstanceBackend_GL :
                          b == "vulkan" ? WGPUInstanceBackend_Vulkan :
                          b == "dx12" ? WGPUInstanceBackend_DX12 :
                          b == "metal" ? WGPUInstanceBackend_Metal : WGPUInstanceBackend_All;
            id.nextInChain = &ex.chain;
        }
        instance = wgpuCreateInstance(&id);
        if (!instance) {
            initError = "wgpuCreateInstance failed";
            return;
        }
        WGPURequestAdapterOptions opts = {};
        opts.powerPreference = WGPUPowerPreference_HighPerformance;
        WGPURequestAdapterCallbackInfo acb = {};
        acb.mode = WGPUCallbackMode_AllowProcessEvents;
        acb.callback = onAdapter;
        acb.userdata1 = &adapter;
        wgpuInstanceRequestAdapter(instance, &opts, acb);
        for (int i = 0; i < 10000 && !adapter; ++i) {
            wgpuInstanceProcessEvents(instance);
        }
        if (!adapter) {
            initError = "no GPU adapter";
            return;
        }
        WGPULimits supported = {};
        wgpuAdapterGetLimits(adapter, &supported);
        WGPUDeviceDescriptor dd = {};
        dd.requiredLimits = &supported;   // 4K frames exceed the default 128 MB bindings
        dd.uncapturedErrorCallbackInfo.callback = onError;
        dd.uncapturedErrorCallbackInfo.userdata1 = this;
        WGPURequestDeviceCallbackInfo dcb = {};
        dcb.mode = WGPUCallbackMode_AllowProcessEvents;
        dcb.callback = onDevice;
        dcb.userdata1 = &device;
        wgpuAdapterRequestDevice(adapter, &dd, dcb);
        for (int i = 0; i < 10000 && !device; ++i) {
            wgpuInstanceProcessEvents(instance);
        }
        if (!device) {
            initError = "GPU device request failed";
            return;
        }
        queue = wgpuDeviceGetQueue(device);
        wgpuDeviceGetLimits(device, &limits);
        WGPUAdapterInfo info = {};
        wgpuAdapterGetInfo(adapter, &info);
        name = str(info.device);
        if ( name.find_first_not_of(" ") == std::string::npos ) {
            name = str(info.description);
        }
        if ( name.find_first_not_of(" ") == std::string::npos ) {
            name = "GPU";
        }
    }

    WGPUComputePipeline pipeline(const char* key, const char* wgsl)
    {
        std::map<std::string, WGPUComputePipeline>::iterator it = pipelines.find(key);
        if ( it != pipelines.end() ) {
            return it->second;
        }
        WGPUShaderSourceWGSL src = {};
        src.chain.sType = WGPUSType_ShaderSourceWGSL;
        src.code = sv(wgsl);
        WGPUShaderModuleDescriptor smd = {};
        smd.nextInChain = &src.chain;
        WGPUShaderModule mod = wgpuDeviceCreateShaderModule(device, &smd);
        WGPUComputePipeline p = nullptr;
        if (mod) {
            WGPUComputePipelineDescriptor pd = {};
            pd.compute.module = mod;
            pd.compute.entryPoint = sv("main");
            p = wgpuDeviceCreateComputePipeline(device, &pd);
            wgpuShaderModuleRelease(mod);
        }
        wgpuDevicePoll(device, true, nullptr);
        if ( p && !error.empty() ) {
            wgpuComputePipelineRelease(p);
            p = nullptr;
        }
        if (p) {
            pipelines[key] = p;
        }
        return p;
    }
};

Context&
context()
{
    static Context c;
    return c;
}

} // anonymous namespace

bool
available(std::string* why)
{
    Context& g = context();
    if (!g.ready) {   // after the first init, never waits for a running GPU job
        std::lock_guard<std::mutex> l(g.lock);
        if (!g.tried) {
            g.init();
        }
    }
    if (why) {
        *why = g.device ? g.name : g.initError;
    }
    return g.device != nullptr;
}

std::string
description()
{
    std::string why;
    if ( available(&why) ) {
        return why;
    }
    return "none (" + why + ")";
}

struct Session::Imp
{
    Context& g;
    std::unique_lock<std::mutex> lock;
    std::string err;
    WGPUCommandEncoder enc = nullptr;
    std::vector<WGPUBuffer> owned;          // released with the session
    std::vector<WGPUBuffer> uniforms;       // released after each flush
    std::vector<WGPUBindGroup> groups;

    Imp() : g( context() ), lock(g.lock) {}

    WGPUCommandEncoder encoder()
    {
        if (!enc) {
            enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
        }
        return enc;
    }

    void releaseTransient()
    {
        for (WGPUBindGroup b : groups) {
            wgpuBindGroupRelease(b);
        }
        groups.clear();
        for (WGPUBuffer b : uniforms) {
            wgpuBufferRelease(b);
        }
        uniforms.clear();
    }
};

Session::Session()
    : _imp( new Imp() )
{
    Context& g = _imp->g;
    if (!g.tried) {
        g.init();
    }
    if (!g.device) {
        _imp->err = g.initError;
    }
    g.error.clear();
}

Session::~Session()
{
    if (_imp->enc) {
        wgpuCommandEncoderRelease(_imp->enc);
    }
    _imp->releaseTransient();
    for (WGPUBuffer b : _imp->owned) {
        if (b) {
            wgpuBufferRelease(b);
        }
    }
}

bool
Session::ok() const
{
    return _imp->g.device && _imp->err.empty();
}

const std::string&
Session::error() const
{
    return _imp->err;
}

std::uint64_t
Session::maxBinding() const
{
    return _imp->g.device ? _imp->g.limits.maxStorageBufferBindingSize : 0;
}

Buffer
Session::create(std::size_t bytes, const void* data)
{
    Buffer r;
    if ( !ok() ) {
        return r;
    }
    const std::uint64_t size = std::max<std::uint64_t>(16, (bytes + 3) & ~(std::uint64_t)3);
    if (size > _imp->g.limits.maxStorageBufferBindingSize || size > _imp->g.limits.maxBufferSize) {
        _imp->err = "the frame needs larger GPU buffers than this GPU allows";
        return r;
    }
    WGPUBufferDescriptor bd = {};
    bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    bd.size = size;
    WGPUBuffer b = wgpuDeviceCreateBuffer(_imp->g.device, &bd);
    if (!b) {
        _imp->err = "GPU buffer allocation failed";
        return r;
    }
    _imp->owned.push_back(b);
    r.h = b;
    r.bytes = bytes;
    if (data && bytes) {
        upload(r, data, bytes);
    }
    return r;
}

void
Session::upload(const Buffer& b, const void* data, std::size_t bytes)
{
    if ( !ok() || !b.valid() || !bytes ) {
        return;
    }
    // queue writes happen before the next submission: flush what is recorded first
    if (_imp->enc) {
        flush();
    }
    if ( (bytes & 3) == 0 ) {
        wgpuQueueWriteBuffer(_imp->g.queue, (WGPUBuffer)b.h, 0, data, bytes);
    } else {
        std::vector<unsigned char> pad( (bytes + 3) & ~(std::size_t)3, 0 );
        std::memcpy(pad.data(), data, bytes);
        wgpuQueueWriteBuffer(_imp->g.queue, (WGPUBuffer)b.h, 0, pad.data(), pad.size());
    }
}

void
Session::clear(const Buffer& b)
{
    if ( !ok() || !b.valid() ) {
        return;
    }
    wgpuCommandEncoderClearBuffer(_imp->encoder(), (WGPUBuffer)b.h, 0, WGPU_WHOLE_SIZE);
}

void
Session::copy(const Buffer& src, const Buffer& dst, std::size_t bytes)
{
    if ( !ok() || !src.valid() || !dst.valid() ) {
        return;
    }
    wgpuCommandEncoderCopyBufferToBuffer(_imp->encoder(), (WGPUBuffer)src.h, 0, (WGPUBuffer)dst.h, 0,
                                         (bytes + 3) & ~(std::size_t)3);
}

void
Session::dispatch(const char* name, const char* wgsl, std::initializer_list<Buffer> storage,
                  const void* uniforms, std::size_t uniformBytes,
                  std::uint32_t gx, std::uint32_t gy, std::uint32_t gz)
{
    if ( !ok() ) {
        return;
    }
    Context& g = _imp->g;
    WGPUComputePipeline p = g.pipeline(name, wgsl);
    if (!p) {
        _imp->err = std::string("GPU shader ") + name + " failed: " + g.error;
        return;
    }
    std::vector<WGPUBindGroupEntry> e;
    std::uint32_t binding = 0;
    for (const Buffer& b : storage) {
        WGPUBindGroupEntry x = {};
        x.binding = binding++;
        x.buffer = (WGPUBuffer)b.h;
        x.offset = 0;
        x.size = WGPU_WHOLE_SIZE;
        e.push_back(x);
    }
    if (uniforms && uniformBytes) {
        WGPUBufferDescriptor ud = {};
        ud.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        ud.size = std::max<std::uint64_t>(16, (uniformBytes + 15) & ~(std::uint64_t)15);
        ud.mappedAtCreation = true;
        WGPUBuffer u = wgpuDeviceCreateBuffer(g.device, &ud);
        void* m = wgpuBufferGetMappedRange(u, 0, ud.size);
        std::memset(m, 0, ud.size);
        std::memcpy(m, uniforms, uniformBytes);
        wgpuBufferUnmap(u);
        _imp->uniforms.push_back(u);
        WGPUBindGroupEntry x = {};
        x.binding = binding++;
        x.buffer = u;
        x.size = ud.size;
        e.push_back(x);
    }
    WGPUBindGroupDescriptor bd = {};
    bd.layout = wgpuComputePipelineGetBindGroupLayout(p, 0);
    bd.entryCount = e.size();
    bd.entries = e.data();
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(g.device, &bd);
    wgpuBindGroupLayoutRelease(bd.layout);
    _imp->groups.push_back(bg);
    WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(_imp->encoder(), nullptr);
    wgpuComputePassEncoderSetPipeline(pass, p);
    wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
    if (gx && gy && gz) {
        wgpuComputePassEncoderDispatchWorkgroups(pass, gx, gy, gz);
    }
    wgpuComputePassEncoderEnd(pass);
    wgpuComputePassEncoderRelease(pass);
    if ( !g.error.empty() ) {
        _imp->err = "GPU: " + g.error;
    }
}

bool
Session::flush()
{
    if ( !ok() ) {
        return false;
    }
    Context& g = _imp->g;
    if (_imp->enc) {
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(_imp->enc, nullptr);
        wgpuCommandEncoderRelease(_imp->enc);
        _imp->enc = nullptr;
        wgpuQueueSubmit(g.queue, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
    }
    wgpuDevicePoll(g.device, true, nullptr);
    _imp->releaseTransient();
    if ( !g.error.empty() ) {
        _imp->err = "GPU: " + g.error;
        return false;
    }
    return true;
}

bool
Session::download(const Buffer& b, void* out, std::size_t bytes)
{
    if ( !ok() || !b.valid() ) {
        return false;
    }
    Context& g = _imp->g;
    const std::uint64_t size = (bytes + 3) & ~(std::uint64_t)3;
    WGPUBufferDescriptor rd = {};
    rd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    rd.size = size;
    WGPUBuffer rb = wgpuDeviceCreateBuffer(g.device, &rd);
    wgpuCommandEncoderCopyBufferToBuffer(_imp->encoder(), (WGPUBuffer)b.h, 0, rb, 0, size);
    if ( !flush() ) {
        wgpuBufferRelease(rb);
        return false;
    }
    int mapped = 0;
    WGPUBufferMapCallbackInfo mcb = {};
    mcb.mode = WGPUCallbackMode_AllowProcessEvents;
    mcb.callback = onMap;
    mcb.userdata1 = &mapped;
    wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, size, mcb);
    for (int i = 0; mapped == 0 && i < 1000000; ++i) {
        wgpuDevicePoll(g.device, true, nullptr);
        wgpuInstanceProcessEvents(g.instance);
    }
    bool okRead = mapped == 1;
    if (okRead) {
        const void* px = wgpuBufferGetConstMappedRange(rb, 0, size);
        std::memcpy(out, px, bytes);
        wgpuBufferUnmap(rb);
    } else {
        _imp->err = "GPU read back failed";
    }
    wgpuBufferRelease(rb);
    return okRead && ok();
}

} // namespace gpuc

#endif // NATRON_HAVE_WGPU
