/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#ifndef NATRON_ENGINE_DEV_GPU_GPUCOMPUTE_H
#define NATRON_ENGINE_DEV_GPU_GPUCOMPUTE_H

// Small compute layer over wgpu (Vulkan / DX12 / Metal) shared by the nodes
// with a "Use GPU if available" switch (MotionBlur, Kronos, OFlow,
// SmartVector...). One device per process, created on first use; a Session
// holds it for one job (other renders wait), keeps buffers on the GPU
// between kernels and releases everything at the end.
//
// Kernels are WGSL compute shaders with entry point `main`; storage buffers
// are bound at 0..n-1 in the order given, the uniform block (if any) at n.
// Pipelines are compiled once per name.
//
// Plain C++ (no Natron / Qt). Without wgpu in the build (NATRON_HAVE_WGPU
// undefined) available() is false and every Session fails, so callers fall
// back to their CPU code.
//
// NATRON_GPU_BACKEND=gl|vulkan|dx12|metal forces a backend (tests, drivers).

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>

namespace gpuc {

// Whether a GPU can be used. why: the adapter's name, or the reason there is none.
bool available(std::string* why = nullptr);

// "NVIDIA GeForce RTX 4090" / "none (...)": for the nodes' "Local GPU" line.
std::string description();

struct Buffer
{
    void* h = nullptr;      // WGPUBuffer
    std::size_t bytes = 0;
    bool valid() const { return h != nullptr; }
};

class Session
{
public:
    Session();      // waits for the device
    ~Session();

    bool ok() const;                    // device there and no error so far
    const std::string& error() const;

    // Largest storage binding the device allows.
    std::uint64_t maxBinding() const;

    // Storage buffer (zero-filled, or with `data`), usable as copy source / target.
    Buffer create(std::size_t bytes, const void* data = nullptr);
    void upload(const Buffer& b, const void* data, std::size_t bytes);
    // Recorded in order with the dispatches.
    void clear(const Buffer& b);
    void copy(const Buffer& src, const Buffer& dst, std::size_t bytes);
    // Records one dispatch of gx x gy x gz workgroups.
    void dispatch(const char* name, const char* wgsl, std::initializer_list<Buffer> storage,
                  const void* uniforms, std::size_t uniformBytes,
                  std::uint32_t gx, std::uint32_t gy = 1, std::uint32_t gz = 1);
    // Submits what was recorded and waits. False on a GPU error.
    bool flush();
    // Flushes, then reads the buffer back.
    bool download(const Buffer& b, void* out, std::size_t bytes);

private:
    struct Imp;
    std::unique_ptr<Imp> _imp;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
};

// Workgroups to cover n items with groups of `size`.
inline std::uint32_t groups(int n, int size) { return (std::uint32_t)((n + size - 1) / size); }

} // namespace gpuc

#endif // NATRON_ENGINE_DEV_GPU_GPUCOMPUTE_H
