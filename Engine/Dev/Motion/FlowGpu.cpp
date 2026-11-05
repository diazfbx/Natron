/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * ***** END LICENSE BLOCK ***** */

#include "FlowGpu.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "../Gpu/GpuCompute.h"

namespace flow {
namespace gpu {

namespace {

// ------------------------------------------------------------------ refinement kernels
// Buffers: I0, I1 (f32), uv, duv, gxy (Ix, Iy), tins (It, inside), psi (psiD, psiS): vec2.

const char* kParams = R"WGSL(
struct Prm { w: i32, h: i32, alpha: f32, omega: f32, color: i32, radius: i32, epsD: f32, epsS: f32 };
fn idx(x: i32, y: i32) -> u32 { return u32(clamp(y, 0, P.h - 1) * P.w + clamp(x, 0, P.w - 1)); }
)WGSL";

const char* kGrad = R"WGSL(
@group(0) @binding(0) var<storage, read> I0: array<f32>;
@group(0) @binding(1) var<storage, read> I1: array<f32>;
@group(0) @binding(2) var<storage, read> uv: array<vec2<f32>>;
@group(0) @binding(3) var<storage, read_write> gxy: array<vec2<f32>>;
@group(0) @binding(4) var<storage, read_write> tins: array<vec2<f32>>;
@group(0) @binding(5) var<uniform> P: Prm;

fn bil1(x: f32, y: f32) -> f32 {
  let fx = floor(x);
  let fy = floor(y);
  let x0 = i32(fx);
  let y0 = i32(fy);
  let tx = x - fx;
  let ty = y - fy;
  let a = I1[idx(x0, y0)];
  let b = I1[idx(x0 + 1, y0)];
  let c = I1[idx(x0, y0 + 1)];
  let e = I1[idx(x0 + 1, y0 + 1)];
  return (a * (1.0 - tx) + b * tx) * (1.0 - ty) + (c * (1.0 - tx) + e * tx) * ty;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let x = i32(g.x);
  let y = i32(g.y);
  if (x >= P.w || y >= P.h) { return; }
  let i = u32(y * P.w + x);
  let f = uv[i];
  let xw = f32(x) + f.x;
  let yw = f32(y) + f.y;
  let inside = xw >= 0.0 && yw >= 0.0 && xw <= f32(P.w - 1) && yw <= f32(P.h - 1);
  let i1 = bil1(xw, yw);
  let gx1 = 0.5 * (bil1(xw + 1.0, yw) - bil1(xw - 1.0, yw));
  let gy1 = 0.5 * (bil1(xw, yw + 1.0) - bil1(xw, yw - 1.0));
  let gx0 = 0.5 * (I0[idx(x + 1, y)] - I0[idx(x - 1, y)]);
  let gy0 = 0.5 * (I0[idx(x, y + 1)] - I0[idx(x, y - 1)]);
  gxy[i] = vec2<f32>(0.5 * (gx0 + gx1), 0.5 * (gy0 + gy1));
  tins[i] = vec2<f32>(select(0.0, i1 - I0[i], inside), select(0.0, 1.0, inside));
}
)WGSL";

const char* kPsi = R"WGSL(
@group(0) @binding(0) var<storage, read> gxy: array<vec2<f32>>;
@group(0) @binding(1) var<storage, read> tins: array<vec2<f32>>;
@group(0) @binding(2) var<storage, read> uv: array<vec2<f32>>;
@group(0) @binding(3) var<storage, read> duv: array<vec2<f32>>;
@group(0) @binding(4) var<storage, read_write> psi: array<vec2<f32>>;
@group(0) @binding(5) var<uniform> P: Prm;

fn tot(x: i32, y: i32) -> vec2<f32> { let j = idx(x, y); return uv[j] + duv[j]; }

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let x = i32(g.x);
  let y = i32(g.y);
  if (x >= P.w || y >= P.h) { return; }
  let i = u32(y * P.w + x);
  let gr = gxy[i];
  let ti = tins[i];
  let d = duv[i];
  let r = ti.x + gr.x * d.x + gr.y * d.y;
  var pd = 0.0;
  if (ti.y > 0.5) { pd = 0.5 / sqrt(r * r + P.epsD * P.epsD); }
  let dx = 0.5 * (tot(x + 1, y) - tot(x - 1, y));
  let dy = 0.5 * (tot(x, y + 1) - tot(x, y - 1));
  let ps = 0.5 / sqrt(dx.x * dx.x + dy.x * dy.x + dx.y * dx.y + dy.y * dy.y + P.epsS * P.epsS);
  psi[i] = vec2<f32>(pd, ps);
}
)WGSL";

const char* kSor = R"WGSL(
@group(0) @binding(0) var<storage, read> gxy: array<vec2<f32>>;
@group(0) @binding(1) var<storage, read> tins: array<vec2<f32>>;
@group(0) @binding(2) var<storage, read> uv: array<vec2<f32>>;
@group(0) @binding(3) var<storage, read_write> duv: array<vec2<f32>>;
@group(0) @binding(4) var<storage, read> psi: array<vec2<f32>>;
@group(0) @binding(5) var<uniform> P: Prm;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let x = i32(g.x);
  let y = i32(g.y);
  if (x >= P.w || y >= P.h || ((x + y) & 1) != P.color) { return; }
  let i = u32(y * P.w + x);
  let ps = psi[i].y;
  var wsum = 0.0;
  var su = 0.0;
  var sv = 0.0;
  let ui = uv[i];
  for (var k = 0; k < 4; k++) {
    var nx = x;
    var ny = y;
    if (k == 0) { nx = x - 1; } else if (k == 1) { nx = x + 1; } else if (k == 2) { ny = y - 1; } else { ny = y + 1; }
    if (nx < 0 || ny < 0 || nx >= P.w || ny >= P.h) { continue; }
    let j = u32(ny * P.w + nx);
    let wn = P.alpha * 0.5 * (ps + psi[j].y);
    wsum += wn;
    let t = uv[j] + duv[j] - ui;
    su += wn * t.x;
    sv += wn * t.y;
  }
  let pd = psi[i].x;
  let gr = gxy[i];
  let itv = tins[i].x;
  var d = duv[i];
  let denU = pd * gr.x * gr.x + wsum;
  let denV = pd * gr.y * gr.y + wsum;
  if (denU > 1e-12) {
    let nu = (su - pd * (gr.x * itv + gr.x * gr.y * d.y)) / denU;
    d.x = (1.0 - P.omega) * d.x + P.omega * nu;
  }
  if (denV > 1e-12) {
    let nv = (sv - pd * (gr.y * itv + gr.x * gr.y * d.x)) / denV;
    d.y = (1.0 - P.omega) * d.y + P.omega * nv;
  }
  duv[i] = d;
}
)WGSL";

const char* kAdd = R"WGSL(
@group(0) @binding(0) var<storage, read_write> uv: array<vec2<f32>>;
@group(0) @binding(1) var<storage, read> duv: array<vec2<f32>>;
@group(0) @binding(2) var<uniform> P: Prm;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let x = i32(g.x);
  let y = i32(g.y);
  if (x >= P.w || y >= P.h) { return; }
  let i = u32(y * P.w + x);
  uv[i] = uv[i] + duv[i];
}
)WGSL";

const char* kMedian = R"WGSL(
@group(0) @binding(0) var<storage, read> src: array<vec2<f32>>;
@group(0) @binding(1) var<storage, read_write> dst: array<vec2<f32>>;
@group(0) @binding(2) var<uniform> P: Prm;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let x = i32(g.x);
  let y = i32(g.y);
  if (x >= P.w || y >= P.h) { return; }
  let r = min(P.radius, 2);
  var a: array<f32, 25>;
  var b: array<f32, 25>;
  var n = 0;
  for (var j = -r; j <= r; j++) {
    for (var i = -r; i <= r; i++) {
      let s = src[idx(x + i, y + j)];
      a[n] = s.x;
      b[n] = s.y;
      n++;
    }
  }
  let m = n / 2;
  // partial selection sort up to the middle element
  for (var k = 0; k <= m; k++) {
    var ka = k;
    var kb = k;
    for (var l = k + 1; l < n; l++) {
      if (a[l] < a[ka]) { ka = l; }
      if (b[l] < b[kb]) { kb = l; }
    }
    let ta = a[k]; a[k] = a[ka]; a[ka] = ta;
    let tb = b[k]; b[k] = b[kb]; b[kb] = tb;
  }
  dst[u32(y * P.w + x)] = vec2<f32>(a[m], b[m]);
}
)WGSL";

struct Params
{
    std::int32_t w, h;
    float alpha, omega;
    std::int32_t color, radius;
    float epsD, epsS;
};

std::string
src(const char* body)
{
    return std::string(kParams) + body;
}

} // anonymous namespace

bool
refineLevel(const std::vector<float>& I0, const std::vector<float>& I1, int w, int h,
            std::vector<float>& u, std::vector<float>& v, float alpha, int warps, int iters,
            int medianRadius, const std::function<bool()>& aborted, std::string* err)
{
    static const std::string sGrad = src(kGrad), sPsi = src(kPsi), sSor = src(kSor),
                             sAdd = src(kAdd), sMed = src(kMedian);
    gpuc::Session s;
    if ( !s.ok() ) {
        if (err) {
            *err = s.error();
        }
        return false;
    }
    const size_t n = (size_t)w * h;
    std::vector<float> uvh(n * 2);
    for (size_t i = 0; i < n; ++i) {
        uvh[i * 2] = u[i];
        uvh[i * 2 + 1] = v[i];
    }
    gpuc::Buffer bI0 = s.create(n * 4, I0.data()), bI1 = s.create(n * 4, I1.data());
    gpuc::Buffer bUV = s.create(n * 8, uvh.data()), bDUV = s.create(n * 8), bG = s.create(n * 8),
                 bT = s.create(n * 8), bPsi = s.create(n * 8), bTmp = s.create(n * 8);
    Params p = { w, h, alpha, 1.8f, 0, medianRadius, 1e-3f, 1e-2f };
    const std::uint32_t gx = gpuc::groups(w, 8), gy = gpuc::groups(h, 8);
    for (int warp = 0; warp < warps && s.ok(); ++warp) {
        if ( aborted && aborted() ) {
            if (err) {
                *err = "aborted";
            }
            return false;
        }
        s.dispatch("flow.grad", sGrad.c_str(), {bI0, bI1, bUV, bG, bT}, &p, sizeof(p), gx, gy);
        s.clear(bDUV);
        for (int it = 0; it < iters; ++it) {
            if ( (it & 3) == 0 ) {
                s.dispatch("flow.psi", sPsi.c_str(), {bG, bT, bUV, bDUV, bPsi}, &p, sizeof(p), gx, gy);
            }
            for (int c = 0; c < 2; ++c) {
                Params q = p;
                q.color = c;
                s.dispatch("flow.sor", sSor.c_str(), {bG, bT, bUV, bDUV, bPsi}, &q, sizeof(q), gx, gy);
            }
        }
        s.dispatch("flow.add", sAdd.c_str(), {bUV, bDUV}, &p, sizeof(p), gx, gy);
        if (medianRadius > 0) {
            s.dispatch("flow.median", sMed.c_str(), {bUV, bTmp}, &p, sizeof(p), gx, gy);
            s.copy(bTmp, bUV, n * 8);
        }
        s.flush();   // one submission per warp: stays under the OS GPU timeout
    }
    if ( !s.download(bUV, uvh.data(), n * 8) ) {
        if (err) {
            *err = s.error();
        }
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        u[i] = uvh[i * 2];
        v[i] = uvh[i * 2 + 1];
    }
    return true;
}

// ------------------------------------------------------------------ motion blur gather

namespace {

const char* kGather = R"WGSL(
struct G {
  w: i32, h: i32, tw: i32, th: i32,
  T: i32, ns: i32, filt: i32, haveDepth: i32,
  shutter: f32, halfS: f32, y0: i32, p1: i32,
};
@group(0) @binding(0) var<storage, read> img: array<vec4<f32>>;
@group(0) @binding(1) var<storage, read> F: array<vec2<f32>>;
@group(0) @binding(2) var<storage, read> B: array<vec2<f32>>;
@group(0) @binding(3) var<storage, read> D: array<f32>;
@group(0) @binding(4) var<storage, read> tiles: array<vec4<f32>>;
@group(0) @binding(5) var<storage, read_write> outp: array<vec4<f32>>;
@group(0) @binding(6) var<uniform> P: G;

fn cl(x: i32, y: i32) -> u32 { return u32(clamp(y, 0, P.h - 1) * P.w + clamp(x, 0, P.w - 1)); }

fn field(fwd: bool, x: f32, y: f32) -> vec2<f32> {
  let fx = floor(x);
  let fy = floor(y);
  let x0 = i32(fx);
  let y0 = i32(fy);
  let tx = x - fx;
  let ty = y - fy;
  var a: vec2<f32>; var b: vec2<f32>; var c: vec2<f32>; var d: vec2<f32>;
  if (fwd) {
    a = F[cl(x0, y0)]; b = F[cl(x0 + 1, y0)]; c = F[cl(x0, y0 + 1)]; d = F[cl(x0 + 1, y0 + 1)];
  } else {
    a = B[cl(x0, y0)]; b = B[cl(x0 + 1, y0)]; c = B[cl(x0, y0 + 1)]; d = B[cl(x0 + 1, y0 + 1)];
  }
  return (a * (1.0 - tx) + b * tx) * (1.0 - ty) + (c * (1.0 - tx) + d * tx) * ty;
}

fn cubicW(t0: f32) -> f32 {
  let t = abs(t0);
  if (t < 1.0) { return (1.5 * t - 2.5) * t * t + 1.0; }
  if (t < 2.0) { return ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0; }
  return 0.0;
}

fn sampleImg(x: f32, y: f32) -> vec4<f32> {
  if (P.filt == 0) {
    return img[cl(i32(floor(x + 0.5)), i32(floor(y + 0.5)))];
  }
  let fx = floor(x);
  let fy = floor(y);
  let x0 = i32(fx);
  let y0 = i32(fy);
  let tx = x - fx;
  let ty = y - fy;
  if (P.filt == 1) {
    return (img[cl(x0, y0)] * (1.0 - tx) + img[cl(x0 + 1, y0)] * tx) * (1.0 - ty) +
           (img[cl(x0, y0 + 1)] * (1.0 - tx) + img[cl(x0 + 1, y0 + 1)] * tx) * ty;
  }
  var acc = vec4<f32>(0.0);
  for (var j = 0; j < 4; j++) {
    let wy = cubicW(ty - f32(j - 1));
    for (var i = 0; i < 4; i++) {
      acc += (cubicW(tx - f32(i - 1)) * wy) * img[cl(x0 + i - 1, y0 + j - 1)];
    }
  }
  return acc;
}

fn nidx(x: f32, y: f32) -> u32 { return cl(i32(floor(x + 0.5)), i32(floor(y + 0.5))); }

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) g: vec3<u32>) {
  let x = i32(g.x);
  let y = i32(g.y) + P.y0;
  if (x >= P.w || y >= P.h) { return; }
  let i = u32(y * P.w + x);
  let t = u32(min(P.th - 1, y / P.T) * P.tw + min(P.tw - 1, x / P.T));
  let fo = F[i];
  let bo = B[i];
  let nt = tiles[t];
  let dX = D[i];
  let px = f32(x);
  let py = f32(y);
  var own: array<vec4<f32>, 64>;
  var st: array<i32, 64>;
  var acc = vec4<f32>(0.0);
  var nLand = 0;
  let ns = min(P.ns, 64);
  for (var k = 0; k < ns; k++) {
    let tau = ((f32(k) + 0.5) / f32(ns)) * P.shutter - P.halfS;
    let fw = tau >= 0.0;
    let a = abs(tau);
    let d = a * select(bo, fo, fw);
    let sx = px - d.x;
    let sy = py - d.y;
    let sv = field(fw, sx, sy);
    let e = a * sv - d;
    let tol = max(1.0, 0.3 * length(d));
    own[k] = sampleImg(sx, sy);
    if (dot(e, e) <= tol * tol) {
      st[k] = 1;
      acc += own[k];
      nLand++;
    } else if (P.haveDepth != 0 && dX > D[nidx(sx, sy)] + 0.25) {
      st[k] = 0;
    } else {
      st[k] = -1;
    }
  }
  if (nLand > 0) { acc = acc / f32(nLand); } else { acc = img[i]; }
  var o = vec4<f32>(0.0);
  for (var k = 0; k < ns; k++) {
    let tau = ((f32(k) + 0.5) / f32(ns)) * P.shutter - P.halfS;
    let fw = tau >= 0.0;
    let a = abs(tau);
    let d = a * select(bo, fo, fw);
    let nd = a * select(nt.zw, nt.xy, fw);
    var c = own[k];
    if (st[k] < 0) { c = acc; }
    let dd = nd - d;
    if (dot(dd, dd) > 1.0) {
      let sx = px - nd.x;
      let sy = py - nd.y;
      let sv = field(fw, sx, sy);
      let e = a * sv - nd;
      let tol = max(1.0, 0.3 * length(nd));
      if (dot(e, e) <= tol * tol) {
        let w2 = clamp(0.5 + (D[nidx(sx, sy)] - dX), 0.0, 1.0);
        if (w2 > 0.0) {
          c = (1.0 - w2) * c + w2 * sampleImg(sx, sy);
        }
      }
    }
    o += c;
  }
  outp[i] = o / f32(ns);
}
)WGSL";

struct GParams
{
    std::int32_t w, h, tw, th, T, ns, filter, haveDepth;
    float shutter, half;
    std::int32_t y0, p1;
};

} // anonymous namespace

bool
motionBlurGather(const GatherJob& j, float* out, std::string* err)
{
    gpuc::Session s;
    if ( !s.ok() ) {
        if (err) {
            *err = s.error();
        }
        return false;
    }
    const size_t n = (size_t)j.w * j.h;
    gpuc::Buffer bImg = s.create(n * 16, j.img), bF = s.create(n * 8, j.F), bB = s.create(n * 8, j.B),
                 bD = s.create(n * 4, j.D), bT = s.create( (size_t)j.tw * j.th * 16, j.tiles ),
                 bOut = s.create(n * 16);
    // bands of ~1M pixels, one submission each (OS GPU timeout)
    const int band = std::max( 8, ( (1 << 20) / std::max(1, j.w) ) & ~7 );
    for (int y0 = 0; y0 < j.h && s.ok(); y0 += band) {
        GParams p = { j.w, j.h, j.tw, j.th, j.T, std::min(64, j.Ns), j.filter, j.haveDepth ? 1 : 0,
                      j.shutter, 0.5f * j.shutter, y0, 0 };
        s.dispatch("flow.mbgather", kGather, {bImg, bF, bB, bD, bT, bOut}, &p, sizeof(p),
                   gpuc::groups(j.w, 8), gpuc::groups(std::min(band, j.h - y0), 8));
        s.flush();
    }
    if ( !s.download(bOut, out, n * 16) ) {
        if (err) {
            *err = s.error();
        }
        return false;
    }
    return true;
}

} // namespace gpu
} // namespace flow
