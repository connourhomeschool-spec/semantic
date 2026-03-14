#pragma once

#include <cmath>
#include <cstdint>
#include <cfloat>
#include <algorithm>
#include <vector>
#include <cstring>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <condition_variable>

constexpr int   SCREEN_W = 800;
constexpr int   SCREEN_H = 600;
constexpr float FOV_DEG = 75.0f;
constexpr float NEAR_PLANE = 0.1f;
constexpr float FAR_PLANE = 300.0f;
constexpr float MOVE_SPEED = 6.0f;
constexpr float SPRINT_MULT = 3.0f;
constexpr float MOUSE_SENS = 0.10f;
constexpr float EYE_HEIGHT = 1.75f;
constexpr float GRAVITY = -14.0f;
constexpr float JUMP_IMPULSE = 6.5f;
constexpr float FOG_DENSITY = 0.007f;
constexpr float FLOOR_HALF = 80.0f;
constexpr int   FLOOR_DIVS = 20;
constexpr float CHECKER_SIZE = 2.0f;
constexpr float FOG_START = 20.0f;
constexpr float FOG_END = 100.0f;
constexpr float PI = 3.14159265358979f;
constexpr float DEG2RAD = PI / 180.0f;
constexpr float WRAP_AMOUNT = 0.3f;
constexpr float ACCEL_LERP = 12.0f;
constexpr float DECEL_LERP = 18.0f;

constexpr int TILE_W = 64;
constexpr int TILE_H = 64;
constexpr int TILES_X = (SCREEN_W + TILE_W - 1) / TILE_W;
constexpr int TILES_Y = (SCREEN_H + TILE_H - 1) / TILE_H;
constexpr int TILE_COUNT = TILES_X * TILES_Y;

// math
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Vec4 { float x, y, z, w; };
struct Mat4 { float m[4][4]; };

inline Vec3  v3add(Vec3 a, Vec3 b) { return{ a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3  v3sub(Vec3 a, Vec3 b) { return{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3  v3mul(Vec3 a, float s) { return{ a.x * s, a.y * s, a.z * s }; }
inline float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3  cross3(Vec3 a, Vec3 b) { return{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float len3sq(Vec3 v) { return v.x * v.x + v.y * v.y + v.z * v.z; }
inline float len3(Vec3 v) { return sqrtf(len3sq(v)); }
inline Vec3  norm3(Vec3 v) { float l = len3(v); return l > 1e-7f ? v3mul(v, 1.f / l) : Vec3{ 0,1,0 }; }
inline Vec3  lerp3(Vec3 a, Vec3 b, float t) { return{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t }; }
inline float clampf(float v, float lo, float hi) { return v<lo ? lo : v>hi ? hi : v; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstep(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f); return t * t * (3.f - 2.f * t);
}

// fast reciprocal sqrt (Quake III)
inline float rsqrtf(float x) {
    float xh = 0.5f * x; int i; memcpy(&i, &x, 4);
    i = 0x5f3759df - (i >> 1); memcpy(&x, &i, 4);
    return x * (1.5f - xh * x * x);
}
inline Vec3 fastNorm3(Vec3 v) {
    float r = rsqrtf(v.x * v.x + v.y * v.y + v.z * v.z + 1e-14f);
    return{ v.x * r, v.y * r, v.z * r };
}

// matrices
inline Mat4 mat4Mul(const Mat4& a, const Mat4& b) {
    Mat4 r = {};
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++)
        r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
inline Vec4 mulMV(const Mat4& m, Vec4 v) {
    return{ m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z + m.m[0][3] * v.w,
            m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z + m.m[1][3] * v.w,
            m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z + m.m[2][3] * v.w,
            m.m[3][0] * v.x + m.m[3][1] * v.y + m.m[3][2] * v.z + m.m[3][3] * v.w };
}
inline Mat4 perspMat(float fovDeg, float asp, float zn, float zf) {
    float f = 1.f / tanf(fovDeg * DEG2RAD * 0.5f); Mat4 m = {};
    m.m[0][0] = f / asp; m.m[1][1] = f;
    m.m[2][2] = -(zf + zn) / (zf - zn); m.m[2][3] = -(2.f * zf * zn) / (zf - zn);
    m.m[3][2] = -1.f; return m;
}
inline Mat4 viewMat(Vec3 pos, float yaw, float pitch) {
    Vec3 fwd = norm3({ cosf(pitch) * sinf(yaw), sinf(pitch), -cosf(pitch) * cosf(yaw) });
    Vec3 rgt = norm3(cross3(fwd, { 0,1,0 })); Vec3 up = cross3(rgt, fwd); Mat4 m = {};
    m.m[0][0] = rgt.x; m.m[0][1] = rgt.y; m.m[0][2] = rgt.z; m.m[0][3] = -dot3(rgt, pos);
    m.m[1][0] = up.x;  m.m[1][1] = up.y;  m.m[1][2] = up.z;  m.m[1][3] = -dot3(up, pos);
    m.m[2][0] = -fwd.x; m.m[2][1] = -fwd.y; m.m[2][2] = -fwd.z; m.m[2][3] = dot3(fwd, pos);
    m.m[3][3] = 1.f; return m;
}

// camera
struct Camera { Vec3 pos; float yaw, pitch, fov, nearP, farP; };
inline Camera cameraInit() { return{ {0.f,EYE_HEIGHT,0.f}, 0.f, 0.f, FOV_DEG, NEAR_PLANE, FAR_PLANE }; }
inline Vec3 camFwd(const Camera& c) { return{ sinf(c.yaw), 0.f, -cosf(c.yaw) }; }
inline Vec3 camRight(const Camera& c) { return{ cosf(c.yaw), 0.f,  sinf(c.yaw) }; }

// geometry
struct Vertex { Vec3 pos; Vec3 normal; Vec2 uv; };
struct Triangle { Vertex v[3]; };
struct ScreenVert { float sx, sy, invW, ndcZ; Vec3 normal, wpos; Vec2 uv; };
struct BinnedTri { ScreenVert sv[3]; bool isFloor; };

// framebuffer
struct Framebuffer { uint32_t* pixels; float* depth; int w, h; };
inline void clearBuffers(Framebuffer& fb) {
    int n = fb.w * fb.h;
    memset(fb.pixels, 0, n * sizeof(uint32_t));
    std::fill(fb.depth, fb.depth + n, 1.f);
}

// color
struct Color3 { float r, g, b; };
inline Color3 cadd(Color3 a, Color3 b) { return{ a.r + b.r, a.g + b.g, a.b + b.b }; }
inline Color3 cmul(Color3 a, Color3 b) { return{ a.r * b.r, a.g * b.g, a.b * b.b }; }
inline Color3 cscl(Color3 a, float s) { return{ a.r * s, a.g * s, a.b * s }; }
inline Color3 clerp(Color3 a, Color3 b, float t) { return{ lerpf(a.r,b.r,t), lerpf(a.g,b.g,t), lerpf(a.b,b.b,t) }; }
inline Color3 hexToLinear(uint32_t hex) {
    float r = ((hex >> 16) & 0xFF) / 255.f; r *= r;
    float g = ((hex >> 8) & 0xFF) / 255.f; g *= g;
    float b = ((hex >> 0) & 0xFF) / 255.f; b *= b;
    return{ r, g, b };
}

namespace GammaLUT {
    static uint8_t table[256];
    static bool ready = false;
    static void build() {
        if (ready) return;
        for (int i = 0; i < 256; i++) table[i] = (uint8_t)(sqrtf(i / 255.f) * 255.f + .5f);
        ready = true;
    }
    inline uint8_t apply(float linear) {
        int i = (int)(clampf(linear, 0.f, 1.f) * 255.f + .5f);
        return table[i];
    }
}

inline uint32_t finalize(Color3 c) {
    float r = c.r / (c.r + 1.f), g = c.g / (c.g + 1.f), b = c.b / (c.b + 1.f);
    return 0xFF000000u
        | ((uint32_t)GammaLUT::apply(r) << 16)
        | ((uint32_t)GammaLUT::apply(g) << 8)
        | (uint32_t)GammaLUT::apply(b);
}

// lighting
struct DirLight { Vec3 dir; Color3 color; float intensity; };
struct AmbLight { Color3 skyCol, groundCol; float skyI, groundI; };
struct Atmosphere { Color3 horizonColor, midSkyColor, zenithColor, belowHorizon, fogColor; float fogDensity; };

namespace FC {
    static const Color3 A = hexToLinear(0x8B7355);
    static const Color3 B = hexToLinear(0x7A6245);
    static const Color3 Fog = hexToLinear(0xE8C49A);
}

namespace FogLUT {
    constexpr int   SZ = 1024;
    constexpr float DMAX = FAR_PLANE * FAR_PLANE;
    static float table[SZ + 1];
    static bool  ready = false;
    static void build() {
        if (ready) return;
        for (int i = 0; i <= SZ; i++) {
            float d = sqrtf((float)i / SZ * DMAX);
            float t = clampf((d - FOG_START) / (FOG_END - FOG_START), 0.f, 1.f);
            table[i] = t * t;
        }
        ready = true;
    }
    inline float lookup(float distSq) {
        int idx = (int)(distSq * (SZ / DMAX));
        return table[idx < SZ ? idx : SZ];
    }
}

inline Color3 floorAlbedo(float wx, float wz, float distSq) {
    int cx = (int)floorf(wx / CHECKER_SIZE), cz = (int)floorf(wz / CHECKER_SIZE);
    Color3 base = ((cx + cz) & 1) == 0 ? FC::A : FC::B;
    return clerp(base, FC::Fog, FogLUT::lookup(distSq));
}

inline Color3 shadeSurface(Vec3 wpos, Vec3 N, Vec3 camPos, Color3 albedo, float distSq,
    const DirLight& sun, const AmbLight& amb, const Atmosphere& atm)
{
    float hemi = N.y * 0.5f + 0.5f;
    Color3 ambC = clerp(cscl(amb.groundCol, amb.groundI), cscl(amb.skyCol, amb.skyI), hemi);
    ambC = cmul(ambC, albedo);
    float NdL = dot3(N, sun.dir);
    float wrapped = clampf((NdL + WRAP_AMOUNT) / (1.f + WRAP_AMOUNT), 0.f, 1.f);
    Color3 res = cadd(ambC, cmul(cscl(sun.color, sun.intensity * wrapped), albedo));
    if (NdL > 0.f) {
        Vec3 toEye = v3sub(camPos, wpos);
        Vec3 V = v3mul(toEye, rsqrtf(len3sq(toEye) + 1e-14f));
        Vec3 H = fastNorm3(v3add(V, sun.dir));
        float s = clampf(dot3(N, H), 0.f, 1.f);
        s *= s; s *= s; s *= s; s *= s; s *= s;
        res = cadd(res, cscl({ 0.9f,0.95f,1.f }, s * 0.15f * sun.intensity));
    }
    float ff = clampf(1.f / (1.f + distSq * atm.fogDensity * 0.4f), 0.f, 1.f);
    return clerp(atm.fogColor, res, ff);
}

// sky
inline Color3 sampleSkyDir(float nrx, float nry, float nrz, const Atmosphere& atm, Vec3 sunDir) {
    Color3 sky;
    if (nry < 0.f) sky = clerp(atm.horizonColor, atm.belowHorizon, smoothstep(0.f, -0.25f, nry));
    else {
        float t0 = smoothstep(0.f, 0.08f, nry), t1 = smoothstep(0.f, 0.60f, nry);
        sky = clerp(clerp(atm.horizonColor, atm.midSkyColor, t0), atm.zenithColor, t1 * t1);
    }
    float sd = nrx * sunDir.x + nry * sunDir.y + nrz * sunDir.z;
    if (sd > 0.990f)  sky = clerp(sky, { 1.f,0.85f,0.5f }, smoothstep(0.990f, 0.998f, sd) * 0.6f);
    if (sd > 0.998f)  sky = clerp(sky, { 1.f,0.96f,0.8f }, smoothstep(0.998f, 0.9994f, sd));
    if (sd > 0.9994f) sky = { 1.5f,1.5f,1.4f };
    return sky;
}

inline void renderSkyBand(Framebuffer& fb, const Camera& cam, const Atmosphere& atm, Vec3 sunDir,
    Vec3 fwd, Vec3 rgtS, Vec3 upS, int rowStart, int rowEnd)
{
    int py0 = (rowStart / 2) * 2;
    for (int py = py0; py < rowEnd; py += 2) {
        float ndcY = 1.f - 2.f * (py + 1.f) / fb.h;
        float bx = fwd.x + upS.x * ndcY, by = fwd.y + upS.y * ndcY, bz = fwd.z + upS.z * ndcY;
        for (int px = 0; px < fb.w; px += 2) {
            float ndcX = 2.f * (px + 1.f) / fb.w - 1.f;
            float rx = bx + rgtS.x * ndcX, ry = by + rgtS.y * ndcX, rz = bz + rgtS.z * ndcX;
            float il = rsqrtf(rx * rx + ry * ry + rz * rz);
            uint32_t pix = finalize(sampleSkyDir(rx * il, ry * il, rz * il, atm, sunDir));
            int base = py * fb.w + px;
            fb.pixels[base] = pix;
            if (px + 1 < fb.w) fb.pixels[base + 1] = pix;
            if (py + 1 < fb.h && py + 1 < rowEnd) { fb.pixels[base + fb.w] = pix; if (px + 1 < fb.w) fb.pixels[base + fb.w + 1] = pix; }
        }
    }
}

inline void renderSky(Framebuffer& fb, const Camera& cam, const Atmosphere& atm, Vec3 sunDir) {
    float aspect = float(fb.w) / float(fb.h), tanHFov = tanf(cam.fov * DEG2RAD * 0.5f);
    Vec3 fwd = norm3({ cosf(cam.pitch) * sinf(cam.yaw), sinf(cam.pitch), -cosf(cam.pitch) * cosf(cam.yaw) });
    Vec3 rgt = norm3(cross3(fwd, { 0,1,0 })), up = cross3(rgt, fwd);
    renderSkyBand(fb, cam, atm, sunDir, fwd, v3mul(rgt, aspect * tanHFov), v3mul(up, tanHFov), 0, fb.h);
}

// near-plane clipping
struct ClipVert { Vec4 clip; Vec3 normal, wpos; Vec2 uv; };
inline ClipVert lerpCV(const ClipVert& a, const ClipVert& b, float t) {
    return{ {lerpf(a.clip.x,b.clip.x,t), lerpf(a.clip.y,b.clip.y,t),
             lerpf(a.clip.z,b.clip.z,t), lerpf(a.clip.w,b.clip.w,t)},
            lerp3(a.normal,b.normal,t), lerp3(a.wpos,b.wpos,t),
            {lerpf(a.uv.x,b.uv.x,t), lerpf(a.uv.y,b.uv.y,t)} };
}
inline int clipNear(ClipVert* in, int n, ClipVert* out) {
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        const ClipVert& cur = in[i], & nxt = in[(i + 1) % n];
        bool ci = cur.clip.w > NEAR_PLANE, ni = nxt.clip.w > NEAR_PLANE;
        if (ci) out[cnt++] = cur;
        if (ci != ni) {
            float t = (cur.clip.w - NEAR_PLANE) / (cur.clip.w - nxt.clip.w);
            out[cnt++] = lerpCV(cur, nxt, t);
        }
    }
    return cnt;
}

// thread pool
struct ThreadPool {
    std::vector<std::thread> workers;
    std::atomic<int>  jobIdx{ 0 };
    std::atomic<int>  doneCount{ 0 };
    std::atomic<bool> quit{ false };
    std::atomic<int>  generation{ 0 };
    int nWorkers = 0, nJobs = 0;
    std::function<void(int)> task;
    std::mutex              mtx;
    std::condition_variable cvWork;
    std::condition_variable cvDone;

    void start(int n) {
        nWorkers = n;
        workers.resize(n);
        for (int i = 0; i < n; i++) {
            workers[i] = std::thread([this] {
                int myGen = generation.load(std::memory_order_acquire);
                while (true) {
                    {
                        std::unique_lock<std::mutex> lk(mtx);
                        cvWork.wait(lk, [&] {
                            return generation.load(std::memory_order_acquire) != myGen || quit.load();
                            });
                    }
                    if (quit.load()) return;
                    myGen = generation.load(std::memory_order_acquire);
                    int j;
                    while ((j = jobIdx.fetch_add(1, std::memory_order_relaxed)) < nJobs)
                        task(j);
                    if (doneCount.fetch_add(1, std::memory_order_acq_rel) + 1 == nWorkers)
                        cvDone.notify_one();
                }
                });
        }
    }
    void dispatch(std::function<void(int)> fn, int count) {
        task = std::move(fn); nJobs = count;
        jobIdx.store(0, std::memory_order_relaxed);
        doneCount.store(0, std::memory_order_relaxed);
        { std::unique_lock<std::mutex> lk(mtx); generation.fetch_add(1, std::memory_order_release); }
        cvWork.notify_all();
        int j;
        while ((j = jobIdx.fetch_add(1, std::memory_order_relaxed)) < count) task(j);
        std::unique_lock<std::mutex> lk(mtx);
        cvDone.wait(lk, [&] { return doneCount.load(std::memory_order_acquire) >= nWorkers; });
    }
    void stop() {
        { std::unique_lock<std::mutex> lk(mtx); quit.store(true); generation.fetch_add(1); }
        cvWork.notify_all();
        for (auto& t : workers) t.join();
    }
    ~ThreadPool() { if (!workers.empty()) stop(); }
};

static ThreadPool gPool;

// per-tile state
struct TileBin { std::vector<BinnedTri> tris; };
struct RenderState {
    Framebuffer* fb;
    const Camera* cam;
    const DirLight* sun;
    const AmbLight* amb;
    const Atmosphere* atm;
    TileBin bins[TILE_COUNT];
};
static RenderState gRS;

// rasterize one triangle clipped to tile bounds
inline void drawTriTile(
    Framebuffer& fb, const ScreenVert sv[3],
    const Camera& cam, const DirLight& sun, const AmbLight& amb,
    const Atmosphere& atm, bool isFloor,
    int tx0, int ty0, int tx1, int ty1)
{
    float dAx = sv[1].sx - sv[0].sx, dAy = sv[1].sy - sv[0].sy;
    float dBx = sv[2].sx - sv[0].sx, dBy = sv[2].sy - sv[0].sy;
    float area2 = dAx * dBy - dAy * dBx;
    if (area2 >= 0.f) return;
    float invArea = 1.f / area2;

    int x0 = std::max(tx0, (int)floorf(std::min({ sv[0].sx,sv[1].sx,sv[2].sx })));
    int x1 = std::min(tx1 - 1, (int)ceilf(std::max({ sv[0].sx,sv[1].sx,sv[2].sx })));
    int y0 = std::max(ty0, (int)floorf(std::min({ sv[0].sy,sv[1].sy,sv[2].sy })));
    int y1 = std::min(ty1 - 1, (int)ceilf(std::max({ sv[0].sy,sv[1].sy,sv[2].sy })));
    if (x0 > x1 || y0 > y1) return;

    float dX01 = sv[1].sx - sv[0].sx, dY01 = sv[1].sy - sv[0].sy;
    float dX12 = sv[2].sx - sv[1].sx, dY12 = sv[2].sy - sv[1].sy;
    float dX20 = sv[0].sx - sv[2].sx, dY20 = sv[0].sy - sv[2].sy;

    float px0f = (float)x0 + 0.5f, py0f = (float)y0 + 0.5f;
    float e0r = dX01 * (py0f - sv[0].sy) - dY01 * (px0f - sv[0].sx);
    float e1r = dX12 * (py0f - sv[1].sy) - dY12 * (px0f - sv[1].sx);
    float e2r = dX20 * (py0f - sv[2].sy) - dY20 * (px0f - sv[2].sx);

    // hoist floor lighting (normal is always {0,1,0})
    Color3 floorLit = { 0,0,0 };
    if (isFloor) {
        Color3 ambC = cscl(amb.skyCol, amb.skyI);
        float NdL = sun.dir.y;
        float w = clampf((NdL + WRAP_AMOUNT) / (1.f + WRAP_AMOUNT), 0.f, 1.f);
        floorLit = cadd(ambC, cscl(sun.color, sun.intensity * w));
    }

    for (int py = y0; py <= y1; py++, e0r += dX01, e1r += dX12, e2r += dX20) {
        float e0 = e0r, e1 = e1r, e2 = e2r;
        uint32_t* rp = fb.pixels + py * fb.w;
        float* rd = fb.depth + py * fb.w;
        for (int px = x0; px <= x1; px++, e0 -= dY01, e1 -= dY12, e2 -= dY20) {
            if (e0 > 0.f || e1 > 0.f || e2 > 0.f) continue;

            float w0 = e1 * invArea, w1 = e2 * invArea, w2 = e0 * invArea;
            float ndcZ = sv[0].ndcZ * w0 + sv[1].ndcZ * w1 + sv[2].ndcZ * w2;
            if (ndcZ >= rd[px]) continue;
            rd[px] = ndcZ;

            float p0 = sv[0].invW * w0, p1 = sv[1].invW * w1, p2 = sv[2].invW * w2;
            float pS = p0 + p1 + p2; if (pS < 1e-10f) continue;
            float iS = 1.f / pS;

            float wpx = (sv[0].wpos.x * p0 + sv[1].wpos.x * p1 + sv[2].wpos.x * p2) * iS;
            float wpz = (sv[0].wpos.z * p0 + sv[1].wpos.z * p1 + sv[2].wpos.z * p2) * iS;
            float dx = wpx - cam.pos.x, dz = wpz - cam.pos.z;
            float distSq = dx * dx + dz * dz + cam.pos.y * cam.pos.y;

            Color3 lit;
            if (isFloor) {
                Color3 alb = floorAlbedo(wpx, wpz, distSq);
                Color3 res = cmul(floorLit, alb);
                float ff = clampf(1.f / (1.f + distSq * atm.fogDensity * 0.4f), 0.f, 1.f);
                lit = clerp(atm.fogColor, res, ff);
            }
            else {
                float wpy = (sv[0].wpos.y * p0 + sv[1].wpos.y * p1 + sv[2].wpos.y * p2) * iS;
                Vec3 wp = { wpx,wpy,wpz };
                Vec3 N = fastNorm3({
                    (sv[0].normal.x * p0 + sv[1].normal.x * p1 + sv[2].normal.x * p2) * iS,
                    (sv[0].normal.y * p0 + sv[1].normal.y * p1 + sv[2].normal.y * p2) * iS,
                    (sv[0].normal.z * p0 + sv[1].normal.z * p1 + sv[2].normal.z * p2) * iS });
                lit = shadeSurface(wp, N, cam.pos, { 0.7f,0.7f,0.7f }, distSq, sun, amb, atm);
            }
            rp[px] = finalize(lit);
        }
    }
}

inline void binTri(const Triangle& tri, const Mat4& vp, float fbW, float fbH, bool isFloor) {
    ClipVert cv[3];
    for (int i = 0; i < 3; i++) {
        Vec4 p = { tri.v[i].pos.x, tri.v[i].pos.y, tri.v[i].pos.z, 1.f };
        cv[i] = { mulMV(vp,p), tri.v[i].normal, tri.v[i].pos, tri.v[i].uv };
    }
    if (cv[0].clip.w <= 0 && cv[1].clip.w <= 0 && cv[2].clip.w <= 0) return;
    ClipVert clipped[8]; int nc = clipNear(cv, 3, clipped); if (nc < 3) return;
    for (int i = 1; i + 1 < nc; i++) {
        ClipVert poly[3] = { clipped[0], clipped[i], clipped[i + 1] };
        ScreenVert sv[3]; bool ok = true;
        for (int k = 0; k < 3; k++) {
            float w = poly[k].clip.w; if (w <= 1e-6f) { ok = false; break; }
            float iw = 1.f / w;
            sv[k] = { (poly[k].clip.x * iw + 1.f) * 0.5f * fbW,
                      (-poly[k].clip.y * iw + 1.f) * 0.5f * fbH,
                      iw, poly[k].clip.z * iw,
                      poly[k].normal, poly[k].wpos, poly[k].uv };
        }
        if (!ok) continue;
        float dAx = sv[1].sx - sv[0].sx, dAy = sv[1].sy - sv[0].sy;
        float dBx = sv[2].sx - sv[0].sx, dBy = sv[2].sy - sv[0].sy;
        if (dAx * dBy - dAy * dBx >= 0.f) continue;

        int sx0 = (int)floorf(std::min({ sv[0].sx,sv[1].sx,sv[2].sx }));
        int sx1 = (int)ceilf(std::max({ sv[0].sx,sv[1].sx,sv[2].sx }));
        int sy0 = (int)floorf(std::min({ sv[0].sy,sv[1].sy,sv[2].sy }));
        int sy1 = (int)ceilf(std::max({ sv[0].sy,sv[1].sy,sv[2].sy }));
        int txa = std::max(0, sx0 / TILE_W), txb = std::min(TILES_X - 1, sx1 / TILE_W);
        int tya = std::max(0, sy0 / TILE_H), tyb = std::min(TILES_Y - 1, sy1 / TILE_H);

        BinnedTri bt; bt.sv[0] = sv[0]; bt.sv[1] = sv[1]; bt.sv[2] = sv[2]; bt.isFloor = isFloor;
        for (int ty = tya; ty <= tyb; ty++)
            for (int tx = txa; tx <= txb; tx++)
                gRS.bins[ty * TILES_X + tx].tris.push_back(bt);
    }
}

inline void renderTile(int idx) {
    int tx = idx % TILES_X, ty = idx / TILES_X;
    int x0 = tx * TILE_W, y0 = ty * TILE_H;
    int x1 = std::min(x0 + TILE_W, SCREEN_W), y1 = std::min(y0 + TILE_H, SCREEN_H);
    for (const BinnedTri& bt : gRS.bins[idx].tris)
        drawTriTile(*gRS.fb, bt.sv, *gRS.cam, *gRS.sun, *gRS.amb, *gRS.atm, bt.isFloor, x0, y0, x1, y1);
}

struct SkyJob { Framebuffer* fb; const Camera* cam; const Atmosphere* atm; Vec3 sunDir, fwd, rgtS, upS; };
static SkyJob gSky;

constexpr int SKY_JOBS = 4;

inline void renderJob(int idx) {
    if (idx < SKY_JOBS) {
        int bh = (SCREEN_H + SKY_JOBS - 1) / SKY_JOBS;
        int r0 = idx * bh, r1 = std::min(r0 + bh, SCREEN_H);
        renderSkyBand(*gSky.fb, *gSky.cam, *gSky.atm, gSky.sunDir, gSky.fwd, gSky.rgtS, gSky.upS, r0, r1);
    }
    else {
        renderTile(idx - SKY_JOBS);
    }
}

inline void renderScene(
    Framebuffer& fb, const std::vector<Triangle>& tris, const Mat4& vp,
    const Camera& cam, const DirLight& sun, const AmbLight& amb,
    const Atmosphere& atm, bool isFloor, Vec3 sunDir)
{
    FogLUT::build();
    GammaLUT::build();

    float aspect = float(fb.w) / float(fb.h), tanHFov = tanf(cam.fov * DEG2RAD * 0.5f);
    Vec3 fwd = norm3({ cosf(cam.pitch) * sinf(cam.yaw), sinf(cam.pitch), -cosf(cam.pitch) * cosf(cam.yaw) });
    Vec3 rgt = norm3(cross3(fwd, { 0,1,0 })), up = cross3(rgt, fwd);
    gSky = { &fb, &cam, &atm, sunDir, fwd, v3mul(rgt,aspect * tanHFov), v3mul(up,tanHFov) };

    for (auto& b : gRS.bins) b.tris.clear();
    gRS.fb = &fb; gRS.cam = &cam; gRS.sun = &sun; gRS.amb = &amb; gRS.atm = &atm;
    for (const Triangle& tri : tris) binTri(tri, vp, (float)fb.w, (float)fb.h, isFloor);

    gPool.dispatch(renderJob, SKY_JOBS + TILE_COUNT);
}