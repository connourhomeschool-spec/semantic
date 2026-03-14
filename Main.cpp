#define _CRT_SECURE_NO_WARNINGS

#ifdef _WIN32
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#endif

#include <SDL3/SDL.h>
#include <GL/glew.h>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"   // defines STB_VORBIS_INCLUDE_STB_VORBIS_H + type decls
#undef  STB_VORBIS_HEADER_ONLY

#define MA_NO_FLAC
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// networking
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using SockLen   = int;
   using RawSocket = SOCKET;
   static const RawSocket BAD_SOCK = INVALID_SOCKET;
   inline int  netClose(RawSocket s)  { return closesocket(s); }
   inline int  netErrno()             { return WSAGetLastError(); }
   inline bool netWouldBlock(int e)   { return e == WSAEWOULDBLOCK; }
#else
#  include <sys/socket.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <fcntl.h>
#  include <errno.h>
   using SockLen   = socklen_t;
   using RawSocket = int;
   static const RawSocket BAD_SOCK = -1;
   inline int  netClose(RawSocket s)  { return close(s); }
   inline int  netErrno()             { return errno; }
   inline bool netWouldBlock(int e)   { return e == EWOULDBLOCK || e == EAGAIN; }
#endif

#include <array>

constexpr int    NET_PORT      = 7777;
constexpr int    NET_MAX_PEERS = 4;
constexpr double NET_SEND_HZ   = 30.0;
constexpr double NET_TIMEOUT_S = 5.0;

#pragma pack(push, 1)
struct PlayerPacket {
    uint8_t  type;
    uint8_t  peerId;
    uint16_t seq;
    float    x, y, z;
    float    yaw, pitch;
    uint8_t  flags;
};
#pragma pack(pop)
constexpr uint8_t PKT_STATE = 1;
constexpr uint8_t PKT_NAME  = 2;

#pragma pack(push, 1)
struct NamePacket {
    uint8_t type;
    uint8_t peerId;
    char    username[32];
};
#pragma pack(pop)

struct RemotePeer {
    bool        active   = false;
    uint8_t     id       = 0;
    float       x=0,y=0,z=0, yaw=0, pitch=0;
    uint8_t     flags    = 0;
    uint16_t    lastSeq  = 0;
    double      lastSeen = 0.0;
    float       vx=0,vy=0,vz=0;
    float       prevX=0,prevY=0,prevZ=0;
    double      prevTime = 0.0;
    sockaddr_in addr     = {};
    char        username[32] = "Player";
    GLuint      nameTex  = 0;
};

struct NetCtx {
    bool        isHost          = false;
    bool        ok              = false;
    uint8_t     myId            = 0;
    uint16_t    seq             = 0;
    RawSocket   sock            = BAD_SOCK;
    sockaddr_in bindAddr        = {};
    sockaddr_in hostAddr        = {};
    bool        connectedToHost = false;
    std::array<RemotePeer, NET_MAX_PEERS> peers = {};
    double      lastSendTime    = 0.0;
    double      lastNameTime    = 0.0;
    char        myUsername[32]  = "Player";
};

inline bool netInit(NetCtx& ctx, bool host, const char* joinIp = nullptr)
{
#ifdef _WIN32
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2,2), &wd) != 0) { fprintf(stderr,"[net] WSAStartup failed\n"); return false; }
#endif
    ctx.isHost = host;
    ctx.myId   = host ? 0 : 255;
    ctx.sock   = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx.sock == BAD_SOCK) { fprintf(stderr,"[net] socket() failed: %d\n", netErrno()); return false; }
#ifdef _WIN32
    u_long mode = 1; ioctlsocket(ctx.sock, FIONBIO, &mode);
#else
    int fl = fcntl(ctx.sock, F_GETFL, 0); fcntl(ctx.sock, F_SETFL, fl | O_NONBLOCK);
#endif
    int reuse = 1;
    setsockopt(ctx.sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    memset(&ctx.bindAddr, 0, sizeof(ctx.bindAddr));
    ctx.bindAddr.sin_family      = AF_INET;
    ctx.bindAddr.sin_port        = htons((u_short)NET_PORT);
    ctx.bindAddr.sin_addr.s_addr = INADDR_ANY;
    if (bind(ctx.sock, reinterpret_cast<sockaddr*>(&ctx.bindAddr), sizeof(ctx.bindAddr)) != 0) {
        fprintf(stderr,"[net] bind() failed: %d\n", netErrno());
        netClose(ctx.sock); ctx.sock = BAD_SOCK; return false;
    }
    if (!host && joinIp) {
        memset(&ctx.hostAddr, 0, sizeof(ctx.hostAddr));
        ctx.hostAddr.sin_family = AF_INET;
        ctx.hostAddr.sin_port   = htons((u_short)NET_PORT);
        inet_pton(AF_INET, joinIp, &ctx.hostAddr.sin_addr);
        ctx.connectedToHost = true;
        fprintf(stdout,"[net] Client — joining %s:%d\n", joinIp, NET_PORT);
    } else if (host) {
        fprintf(stdout,"[net] Host — listening on UDP %d\n", NET_PORT);
    }
    ctx.ok = true; return true;
}

inline void netShutdown(NetCtx& ctx)
{
    if (ctx.sock != BAD_SOCK) { netClose(ctx.sock); ctx.sock = BAD_SOCK; }
    ctx.ok = false;
#ifdef _WIN32
    WSACleanup();
#endif
}

inline RemotePeer* netFindPeer(NetCtx& ctx, const sockaddr_in& from)
{
    for (auto& p : ctx.peers)
        if (p.active && p.addr.sin_addr.s_addr==from.sin_addr.s_addr && p.addr.sin_port==from.sin_port)
            return &p;
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (!ctx.peers[i].active) {
            ctx.peers[i]        = RemotePeer{};
            ctx.peers[i].active = true;
            ctx.peers[i].id     = (uint8_t)i;
            ctx.peers[i].addr   = from;
            char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            fprintf(stdout,"[net] Peer connected: %s:%d (slot %d)\n", ip, ntohs(from.sin_port), i);
            return &ctx.peers[i];
        }
    }
    return nullptr;
}

inline void netSendState(NetCtx& ctx, float x, float y, float z,
                         float yaw, float pitch, uint8_t flags, double now)
{
    if (!ctx.ok) return;
    if (now - ctx.lastSendTime < 1.0 / NET_SEND_HZ) return;
    ctx.lastSendTime = now;
    PlayerPacket pkt;
    pkt.type=PKT_STATE; pkt.peerId=ctx.myId; pkt.seq=ctx.seq++;
    pkt.x=x; pkt.y=y; pkt.z=z; pkt.yaw=yaw; pkt.pitch=pitch; pkt.flags=flags;
    if (ctx.isHost) {
        for (auto& peer : ctx.peers)
            if (peer.active) sendto(ctx.sock, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
                reinterpret_cast<const sockaddr*>(&peer.addr), sizeof(peer.addr));
    } else if (ctx.connectedToHost) {
        sendto(ctx.sock, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
               reinterpret_cast<const sockaddr*>(&ctx.hostAddr), sizeof(ctx.hostAddr));
    }
}

inline void netSendName(NetCtx& ctx, double now)
{
    if (!ctx.ok) return;
    if (now - ctx.lastNameTime < 3.0) return;
    ctx.lastNameTime = now;
    NamePacket pkt;
    pkt.type   = PKT_NAME;
    pkt.peerId = ctx.myId;
    memset(pkt.username, 0, sizeof(pkt.username));
    strncpy(pkt.username, ctx.myUsername, 31);
    if (ctx.isHost) {
        for (auto& peer : ctx.peers)
            if (peer.active) sendto(ctx.sock, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
                reinterpret_cast<const sockaddr*>(&peer.addr), sizeof(peer.addr));
    } else if (ctx.connectedToHost) {
        sendto(ctx.sock, reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
               reinterpret_cast<const sockaddr*>(&ctx.hostAddr), sizeof(ctx.hostAddr));
    }
}

inline void netPoll(NetCtx& ctx, double now)
{
    if (!ctx.ok) return;
    uint8_t buf[512]; sockaddr_in from; SockLen fromLen = sizeof(from);
    for (;;) {
        int n = (int)recvfrom(ctx.sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                              reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 0) { if (netWouldBlock(netErrno())) break; break; }
        if (n < (int)sizeof(PlayerPacket)) continue;
        PlayerPacket* pkt = reinterpret_cast<PlayerPacket*>(buf);
        if (pkt->type == PKT_NAME) {
                        NamePacket* np = reinterpret_cast<NamePacket*>(buf);
            RemotePeer* peer = netFindPeer(ctx, from);
            if (peer) {
                memset(peer->username, 0, sizeof(peer->username));
                strncpy(peer->username, np->username, 31);
                peer->nameTex = 0;
                if (ctx.isHost) {
                    for (auto& other : ctx.peers) {
                        if (!other.active) continue;
                        if (other.addr.sin_addr.s_addr==from.sin_addr.s_addr && other.addr.sin_port==from.sin_port) continue;
                        sendto(ctx.sock, reinterpret_cast<const char*>(buf), n, 0,
                               reinterpret_cast<const sockaddr*>(&other.addr), sizeof(other.addr));
                    }
                }
            }
            continue;
        }
        if (pkt->type != PKT_STATE) continue;
        RemotePeer* peer = netFindPeer(ctx, from);
        if (!peer) continue;
        if (ctx.isHost && pkt->peerId == 255) pkt->peerId = peer->id;
        int16_t diff = (int16_t)(pkt->seq - peer->lastSeq);
        if (peer->lastSeen > 0 && diff <= 0) continue;
        if (peer->lastSeen > 0) {
            double dt = now - peer->prevTime;
            if (dt > 0.001) {
                peer->vx = (pkt->x - peer->prevX) / (float)dt;
                peer->vy = (pkt->y - peer->prevY) / (float)dt;
                peer->vz = (pkt->z - peer->prevZ) / (float)dt;
            }
        }
        peer->prevX=peer->x; peer->prevY=peer->y; peer->prevZ=peer->z; peer->prevTime=peer->lastSeen;
        peer->x=pkt->x; peer->y=pkt->y; peer->z=pkt->z;
        peer->yaw=pkt->yaw; peer->pitch=pkt->pitch; peer->flags=pkt->flags;
        peer->lastSeq=pkt->seq; peer->lastSeen=now;
        if (ctx.isHost) {
            for (auto& other : ctx.peers) {
                if (!other.active) continue;
                if (other.addr.sin_addr.s_addr==from.sin_addr.s_addr && other.addr.sin_port==from.sin_port) continue;
                sendto(ctx.sock, reinterpret_cast<const char*>(buf), n, 0,
                       reinterpret_cast<const sockaddr*>(&other.addr), sizeof(other.addr));
            }
        }
    }
    for (auto& p : ctx.peers)
        if (p.active && now - p.lastSeen > NET_TIMEOUT_S) { fprintf(stdout,"[net] Peer %d timed out\n", p.id); p.active=false; }
}

inline void netGetPeerPos(const RemotePeer& p, double now, float& ox, float& oy, float& oz)
{
    double dt = now - p.lastSeen;
    if (dt > 2.0 / NET_SEND_HZ) dt = 2.0 / NET_SEND_HZ;
    ox = p.x + p.vx*(float)dt;
    oy = p.y + p.vy*(float)dt;
    oz = p.z + p.vz*(float)dt;
}

#include <cstdio>
#ifdef _WIN32
#  include <direct.h>
#  define getcwd _getcwd
#else
#  include <unistd.h>
#endif
#include <cmath>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <algorithm>
#include <string>

constexpr float PI = 3.14159265358979f;
constexpr float DEG2RAD = PI / 180.0f;

constexpr int   SCREEN_W = 640;
constexpr int   SCREEN_H = 480;
constexpr int   WINDOW_W = 1280;
constexpr int   WINDOW_H = 960;
constexpr float FOV_DEG = 80.0f;
constexpr float MOUSE_SENS = 0.11f;
constexpr float NEAR_P = 0.2f;
constexpr float FAR_P = 8000.0f;

constexpr float MOVE_SPEED = 13.0f;
constexpr float EYE_H = 1.75f;
constexpr float EYE_CROUCH = 1.30f;
constexpr float GRAVITY = -28.0f;
constexpr float JUMP_IMP = 10.0f;

constexpr float GROUND_ACCEL = 90.0f;
constexpr float AIR_ACCEL = 18.0f;
constexpr float FRICTION = 7.0f;
constexpr float MAX_SPEED = 22.0f;

constexpr float BHOP_BOOST = 1.12f;
constexpr float BHOP_CAP = 55.0f;

constexpr float DASH_SPEED = 32.0f;
constexpr float DASH_CD = 1.2f;

constexpr float SLIDE_SPEED = 20.0f;
constexpr float SLIDE_DUR = 0.55f;

constexpr float WALLHOP_UP = 8.0f;
constexpr float WALLHOP_BOOST = 1.25f;

constexpr float FOV_SPEED_SCALE = 0.22f;
constexpr float FOV_SPEED_MAX = 18.0f;
constexpr float FOV_LERP_RATE = 7.0f;

constexpr float STRAFE_LEAN_MAX = 0.055f;
constexpr float STRAFE_LEAN_RATE = 10.0f;

constexpr float LAND_SQUISH_STR = 0.55f;
constexpr float LAND_SQUISH_STIFF = 280.0f;
constexpr float LAND_SQUISH_DAMP = 18.0f;

constexpr float CHUNK_SIZE = 96.0f;
constexpr int   VIEW_CHUNKS = 4;

constexpr float ROAD_HALF = 4.5f;
constexpr float MAX_H = 12000.0f;
constexpr float MIN_H = 80.0f;
constexpr float FLOOR_H = 3.4f;

constexpr float FOG_NEAR_DENSITY = 0.0015f;
constexpr float FOG_DIST_DENSITY = 0.0045f;
constexpr float FOG_DIST_DENSITY_NIGHT = 0.0080f;
constexpr float FOG_HEIGHT_DENSITY = 0.0050f;
constexpr float FOG_SKY_FLOOR = 0.55f;
constexpr float FOG_MAX = 0.88f;
constexpr float FOG_COLOR_DAY[3] = { 0.62f, 0.64f, 0.66f };
constexpr float FOG_COLOR_NIGHT[3] = { 0.180f, 0.195f, 0.230f };

constexpr float BLOOM_THRESHOLD = 0.60f;

constexpr float BLOOM_STRENGTH = 0.28f;
constexpr float BLOOM_TIGHT_MIX = 0.70f;

constexpr float BLOOM_WIDE_MIX = 0.55f;

constexpr float TONEMAP_EXPOSURE = 1.05f;
constexpr float TONEMAP_GAMMA = 2.20f;

constexpr int   FLARE_ON = 1;
constexpr float FLARE_STRENGTH = 0.4f;

constexpr float GRADE_SHADOW_TINT[3] = { 0.018f, 0.008f,  0.002f };

constexpr float GRADE_HIGHLIGHT_TINT[3] = { 0.012f, 0.006f, -0.008f };

constexpr float GRADE_CONTRAST = 0.28f;
constexpr float GRADE_SATURATION = 0.95f;
constexpr float GRADE_VIGNETTE = 1.85f;

constexpr float GRAIN_STRENGTH = 0.028f;

constexpr float AMBIENT_DAY = 0.32f;
constexpr float AMBIENT_NIGHT = 0.10f;
constexpr float SUN_WRAP = 0.10f;
constexpr float CITY_GLOW_NIGHT[3] = { 0.18f, 0.12f, 0.04f };

constexpr float DAY_MINUTES = 5.0f;
constexpr float NIGHT_MINUTES = 10.0f;

constexpr int SKY_ZENITH_DAY = 0x1C4470;
constexpr int SKY_ZENITH_DUSK = 0x0D2240;
constexpr int SKY_ZENITH_NIGHT = 0x020510;
constexpr int SKY_MIDSKY_DAY = 0x6888A0;
constexpr int SKY_MIDSKY_DUSK = 0x3A3060;
constexpr int SKY_MIDSKY_NIGHT = 0x050C18;
constexpr int SKY_HORIZON_DAY = 0xC4C0BB;
constexpr int SKY_HORIZON_DUSK = 0xFF8040;
constexpr int SKY_HORIZON_NIGHT = 0x0A0E18;
constexpr int SKY_FOG_DAY = 0x9CA4A8;
constexpr int SKY_FOG_DUSK = 0xCC6030;
constexpr int SKY_FOG_NIGHT = 0x050810;
constexpr int SKY_AMB_DAY = 0x8AAABB;
constexpr int SKY_AMB_DUSK = 0xCC7755;
constexpr int SKY_AMB_NIGHT = 0x101828;
constexpr int SUN_COL_DAY = 0xFFF0D0;
constexpr int SUN_COL_DUSK = 0xFF9040;
constexpr int SUN_COL_NIGHT = 0x102040;

constexpr float BARREL_DISTORTION = 0.022f;
constexpr float CHROM_ABER_BASE = 0.0006f;
constexpr float CHROM_ABER_EDGE = 0.003f;

constexpr float SSAO_RADIUS = 1.20f;
constexpr float SSAO_BIAS = 0.030f;
constexpr float SSAO_STRENGTH = 1.40f;
constexpr int   SSAO_SAMPLES = 16;

constexpr int   SHADOW_MAP_SIZE = 1024;
constexpr float SHADOW_DISTANCE = 200.0f;
constexpr float SHADOW_BIAS = 0.003f;
constexpr float SHADOW_SOFTNESS = 1.5f;

constexpr float DOF_FOCUS_DIST = 120.0f;
constexpr float DOF_FOCUS_RANGE = 80.0f;
constexpr float DOF_BLUR_STRENGTH = 3.0f;

constexpr float ROAD_WET_AMOUNT = 0.75f;
constexpr float ROAD_WET_ROUGHNESS = 0.08f;

constexpr float ATMO_BLUESHIFT = 0.025f;
constexpr float ATMO_BLUESHIFT_START = 80.0f;

constexpr int   PS1_COLOR_DEPTH = 8;      // PS2 used 24-bit internally; quantise to 8-bit per channel
constexpr float PS1_DITHER_STR = 0.45f;  // sub-pixel Bayer dither — softer than PS1
constexpr float PS1_WOBBLE_STR = 0.35f;
constexpr float PS1_SCANLINE_STR = 0.10f;
constexpr int   PS2_INTERLACE = 1;       // 1 = enable field-alternating interlace shimmer

constexpr float FOOTSTEP_INTERVAL = 0.38f;
constexpr int   AMBIENT_VOLUME = 60;
constexpr int   SFX_VOLUME = 90;
constexpr float FOOTSTEP_MIN_SPEED = 1.5f;

constexpr float WALL_TEX_SCALE = 3.0f;
constexpr float ROAD_TEX_SCALE = 4.0f;
constexpr float SIDEWALK_TEX_SCALE = 2.0f;
constexpr float TEX_BLEND = 0.55f;

// MATH
struct Vec3 { float x, y, z; };
struct Mat4 { float m[16]; };

static Vec3 v3add(Vec3 a, Vec3 b) { return{ a.x + b.x,a.y + b.y,a.z + b.z }; }
static Vec3 v3mul(Vec3 a, float s) { return{ a.x * s,a.y * s,a.z * s }; }
static float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross3(Vec3 a, Vec3 b) { return{ a.y * b.z - a.z * b.y,a.z * b.x - a.x * b.z,a.x * b.y - a.y * b.x }; }
static Vec3 norm3(Vec3 v) { float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); return l > 1e-7f ? v3mul(v, 1 / l) : Vec3{ 0,1,0 }; }
static float clampf(float v, float lo, float hi) { return v<lo ? lo : v>hi ? hi : v; }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static Mat4 perspMat(float fov, float asp, float zn, float zf) {
    float f = 1.f / tanf(fov * DEG2RAD * .5f); Mat4 m = {};
    m.m[0] = f / asp; m.m[5] = f;
    m.m[10] = -(zf + zn) / (zf - zn); m.m[14] = -(2.f * zf * zn) / (zf - zn); m.m[11] = -1.f;
    return m;
}
static Mat4 viewMat(Vec3 pos, float yaw, float pitch) {
    Vec3 fwd = norm3({ cosf(pitch) * sinf(yaw),sinf(pitch),-cosf(pitch) * cosf(yaw) });
    Vec3 rgt = norm3(cross3(fwd, { 0,1,0 })); Vec3 up = cross3(rgt, fwd);
    Mat4 m = {};
    m.m[0] = rgt.x; m.m[4] = rgt.y; m.m[8] = rgt.z; m.m[12] = -dot3(rgt, pos);
    m.m[1] = up.x; m.m[5] = up.y; m.m[9] = up.z; m.m[13] = -dot3(up, pos);
    m.m[2] = -fwd.x; m.m[6] = -fwd.y; m.m[10] = -fwd.z; m.m[14] = dot3(fwd, pos);
    m.m[15] = 1.f; return m;
}
static Mat4 mat4Mul(const Mat4& a, const Mat4& b) {
    Mat4 r = {};
    for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) for (int k = 0; k < 4; k++)
        r.m[row + c * 4] += a.m[row + k * 4] * b.m[k + c * 4];
    return r;
}
static Vec3 hexLin(int h) {
    float r = ((h >> 16) & 0xFF) / 255.f; r *= r;
    float g = ((h >> 8) & 0xFF) / 255.f;  g *= g;
    float b = (h & 0xFF) / 255.f;       b *= b;
    return{ r,g,b };
}

// PERLIN NOISE
static uint8_t P[512];
static void initPerlin() {
    for (int i = 0; i < 256; i++) P[i] = (uint8_t)i;
    uint32_t s = 0xDEADBEEF;
    for (int i = 255; i > 0; i--) { s = s * 1664525u + 1013904223u; int j = (s >> 16) % (i + 1); uint8_t t = P[i]; P[i] = P[j]; P[j] = t; }
    for (int i = 0; i < 256; i++) P[256 + i] = P[i];
}
static float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static float grad(int h, float x, float y, float z) {
    int hh = h & 15; float u = hh < 8 ? x : y, v = hh < 4 ? y : hh == 12 || hh == 14 ? x : z;
    return((hh & 1) ? -u : u) + ((hh & 2) ? -v : v);
}
static float perlin(float x, float y, float z) {
    int X = (int)floorf(x) & 255, Y = (int)floorf(y) & 255, Z = (int)floorf(z) & 255;
    x -= floorf(x); y -= floorf(y); z -= floorf(z);
    float u = fade(x), v = fade(y), w = fade(z);
    int A = P[X] + Y, AA = P[A] + Z, AB = P[A + 1] + Z, B = P[X + 1] + Y, BA = P[B] + Z, BB = P[B + 1] + Z;
    return lerpf(lerpf(lerpf(grad(P[AA], x, y, z), grad(P[BA], x - 1, y, z), u),
        lerpf(grad(P[AB], x, y - 1, z), grad(P[BB], x - 1, y - 1, z), u), v),
        lerpf(lerpf(grad(P[AA + 1], x, y, z - 1), grad(P[BA + 1], x - 1, y, z - 1), u),
            lerpf(grad(P[AB + 1], x, y - 1, z - 1), grad(P[BB + 1], x - 1, y - 1, z - 1), u), v), w);
}
static float fbm(float x, float z, int oct = 5) {
    float v = 0, a = 0.5f, f = 1.f, mx = 0;
    for (int i = 0; i < oct; i++) { v += perlin(x * f, 0, z * f) * a; mx += a; f *= 2.f; a *= 0.5f; }
    return v / mx;
}
static float fbm01(float x, float z, int oct = 5) { return fbm(x, z, oct) * 0.5f + 0.5f; }

// CAMERA
struct Camera { Vec3 pos; float yaw, pitch; };
static Vec3 camFwd(Camera c) { return{ sinf(c.yaw),0,-cosf(c.yaw) }; }
static Vec3 camRight(Camera c) { return{ cosf(c.yaw),0,sinf(c.yaw) }; }

struct Vert { float x, y, z, nx, ny, nz, mat; };

static void pushQuad(std::vector<Vert>& V,
    Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, float m) {
    auto v = [&](Vec3 p) {V.push_back({ p.x,p.y,p.z,n.x,n.y,n.z,m }); };
    v(a); v(b); v(c); v(c); v(b); v(d);
}

static void pushBox(std::vector<Vert>& V,
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float mat, bool skipBot = false) {
    if (x0 > x1)std::swap(x0, x1); if (y0 > y1)std::swap(y0, y1); if (z0 > z1)std::swap(z0, z1);

    pushQuad(V, { x1,y0,z1 }, { x1,y0,z0 }, { x1,y1,z1 }, { x1,y1,z0 }, { 1,0,0 }, mat);   // +X
    pushQuad(V, { x0,y0,z0 }, { x0,y0,z1 }, { x0,y1,z0 }, { x0,y1,z1 }, { -1,0,0 }, mat);  // -X
    pushQuad(V, { x0,y0,z1 }, { x1,y0,z1 }, { x0,y1,z1 }, { x1,y1,z1 }, { 0,0,1 }, mat);   // +Z
    pushQuad(V, { x1,y0,z0 }, { x0,y0,z0 }, { x1,y1,z0 }, { x0,y1,z0 }, { 0,0,-1 }, mat);  // -Z
    pushQuad(V, { x0,y1,z0 }, { x0,y1,z1 }, { x1,y1,z0 }, { x1,y1,z1 }, { 0,1,0 }, mat);   // +Y top
    if (!skipBot)
        pushQuad(V, { x0,y0,z1 }, { x0,y0,z0 }, { x1,y0,z1 }, { x1,y0,z0 }, { 0,-1,0 }, mat); // -Y bot
}

// building mesh
static void genBuilding(std::vector<Vert>& V,
    float cx, float cz, float hw, float hd,
    float height, float nv) {
    float matWall = 0.12f + nv * 0.18f;
    float matFace = 0.50f;
    float matDark = 0.06f + nv * 0.06f;

    float x0 = cx - hw, x1 = cx + hw;
    float z0 = cz - hd, z1 = cz + hd;

    int nF = std::min(64, std::max(1, (int)(height / FLOOR_H)));
    for (int fl = 0; fl < nF; fl++) {
        float ya = fl * FLOOR_H;
        float yb = (fl == nF - 1) ? height : ya + FLOOR_H;

        pushQuad(V, { x1,ya,z1 }, { x1,ya,z0 }, { x1,yb,z1 }, { x1,yb,z0 }, { 1,0,0 }, matFace);

        pushQuad(V, { x0,ya,z0 }, { x0,ya,z1 }, { x0,yb,z0 }, { x0,yb,z1 }, { -1,0,0 }, matFace);

        pushQuad(V, { x0,ya,z1 }, { x1,ya,z1 }, { x0,yb,z1 }, { x1,yb,z1 }, { 0,0,1 }, matFace);

        pushQuad(V, { x1,ya,z0 }, { x0,ya,z0 }, { x1,yb,z0 }, { x0,yb,z0 }, { 0,0,-1 }, matFace);
    }

    pushQuad(V, { x0,height,z0 }, { x1,height,z0 }, { x0,height,z1 }, { x1,height,z1 }, { 0,1,0 }, matWall);

        float finOut = 0.4f;   // how far they stick out past the wall
    float finH = 0.22f;  // fin thickness
    for (int fl = 5; fl < nF; fl += 5) {
        float fy = fl * FLOOR_H;
        if (fy >= height - 1.f) break;
        float yt = fy + finH;
            pushQuad(V, { x1,yt,z0 }, { x1 + finOut,yt,z0 }, { x1,yt,z1 }, { x1 + finOut,yt,z1 }, { 0,1,0 }, matDark);
            pushQuad(V, { x0 - finOut,yt,z0 }, { x0,yt,z0 }, { x0 - finOut,yt,z1 }, { x0,yt,z1 }, { 0,1,0 }, matDark);
            pushQuad(V, { x0,yt,z1 }, { x1,yt,z1 }, { x0,yt,z1 + finOut }, { x1,yt,z1 + finOut }, { 0,1,0 }, matDark);
            pushQuad(V, { x0,yt,z0 - finOut }, { x1,yt,z0 - finOut }, { x0,yt,z0 }, { x1,yt,z0 }, { 0,1,0 }, matDark);
            pushQuad(V, { x1 + finOut,fy,z0 }, { x1,fy,z0 }, { x1 + finOut,fy,z1 }, { x1,fy,z1 }, { 0,-1,0 }, matDark);
        pushQuad(V, { x0,fy,z0 }, { x0 - finOut,fy,z0 }, { x0,fy,z1 }, { x0 - finOut,fy,z1 }, { 0,-1,0 }, matDark);
        pushQuad(V, { x0,fy,z1 + finOut }, { x1,fy,z1 + finOut }, { x0,fy,z1 }, { x1,fy,z1 }, { 0,-1,0 }, matDark);
        pushQuad(V, { x0,fy,z0 }, { x1,fy,z0 }, { x0,fy,z0 - finOut }, { x1,fy,z0 - finOut }, { 0,-1,0 }, matDark);
    }

        float pw = 0.5f, ph = 1.5f;
    pushBox(V, x0 - pw, height, z0 - pw, x0, height + ph, z1 + pw, matDark);  // -X wall
    pushBox(V, x1, height, z0 - pw, x1 + pw, height + ph, z1 + pw, matDark);  // +X wall
    pushBox(V, x0, height, z0 - pw, x1, height + ph, z0, matDark);  // -Z wall
    pushBox(V, x0, height, z1, x1, height + ph, z1 + pw, matDark);  // +Z wall

        if (height > 25.f) {
        float phw = hw * .30f, phd = hd * .30f, phh = 3.5f + nv * 5.f;
        pushBox(V, cx - phw, height, cz - phd, cx + phw, height + phh, cz + phd, matWall);
        if (height > 100.f) {
            float sh = height * .08f, sb = 0.65f;
            pushBox(V, cx - sb * .5f, height + phh, cz - sb * .5f, cx + sb * .5f, height + phh + sh, cz + sb * .5f, matDark);
        }
    }

        if (height > 18.f && height < 65.f && nv>0.45f) {
        float wr = 1.1f, wh = 3.2f;
        float wx = cx + (nv - .5f) * hw * .6f, wz = cz + (nv - .3f) * hd * .6f;
        pushBox(V, wx - wr, height, wz - wr, wx + wr, height + wh, wz + wr, matDark);
    }
}

// CHUNK
struct Chunk {
    int cx, cz;
    GLuint vao, vbo;
    GLsizei count;
    struct FP { float x0, z0, x1, z1; };
    std::vector<FP> fps;
};

static void buildChunk(Chunk& ch) {
    float ox = ch.cx * CHUNK_SIZE, oz = ch.cz * CHUNK_SIZE;
    std::vector<Vert> V;
    V.reserve(128 * 1024);

    float gmat = 0.18f;  // same as sidewalk so ground tiles match
    pushQuad(V, { ox,      0, oz }, { ox,      0, oz + CHUNK_SIZE },
        { ox + CHUNK_SIZE, 0, oz }, { ox + CHUNK_SIZE, 0, oz + CHUNK_SIZE },
        { 0,1,0 }, gmat);

    float blockHalf = CHUNK_SIZE * .5f - ROAD_HALF - 1.5f;
    if (blockHalf >= 3.f) {
        float ncx = (float)ch.cx * .07f, ncz = (float)ch.cz * .07f;
        float dist = sqrtf((float)(ch.cx * ch.cx + ch.cz * ch.cz));
        float district = clampf(1.f - dist / 30.f, 0.f, 1.f);
        district = district * .6f + fbm01(ncx, ncz, 4) * .4f;

        float spawnN = fbm01(ncx * 3.f + 5.1f, ncz * 3.f + 8.7f, 3);
        if (spawnN >= 0.28f - district * .25f) {
            float plotN = fbm01(ncx * 5.f + 1.f, ncz * 5.f + 3.f, 3);
            int nP = (district > 0.6f && plotN > 0.7f) ? 4 : (district > 0.3f && plotN > 0.5f) ? 2 : 1;
            float pW = blockHalf / (nP > 2 ? 2.f : (float)nP);
            float pD = blockHalf / (nP == 4 ? 2.f : 1.f);

            for (int pi = 0; pi < nP; pi++) {
                float pox = 0, poz = 0;
                if (nP == 2) { pox = (pi == 0) ? -pW * .5f : pW * .5f; }
                else if (nP == 4) { pox = (pi % 2 == 0) ? -pW * .5f : pW * .5f; poz = (pi / 2 == 0) ? -pD * .5f : pD * .5f; }

                float pcx = ox + CHUNK_SIZE * .5f + pox;
                float pcz = oz + CHUNK_SIZE * .5f + poz;

                float bn = fbm01(ncx * 8.f + (float)pi * .3f, ncz * 8.f + (float)pi * .2f, 4);
                float bn2 = fbm01(ncx * 12.f + (float)pi * .7f, ncz * 12.f + (float)pi * .5f, 3);
                float hSp = fbm01(ncx * 18.f + (float)pi, ncz * 18.f + (float)pi, 4);
                hSp = hSp * hSp;

                float h = MIN_H + district * 1200.f + hSp * hSp * district * 8000.f;
                if (hSp > 0.75f) h = 3000.f + hSp * hSp * 9000.f;
                h = clampf(h, MIN_H, MAX_H);

                float hw = clampf(pW * (.5f + bn * .4f), 2.f, pW - .5f);
                float hd = clampf(pD * (.5f + bn2 * .4f), 2.f, pD - .5f);

                // no jitter — keep buildings grid-aligned to avoid overlap

                genBuilding(V, pcx, pcz, hw, hd, h, bn);
                ch.fps.push_back({ pcx - hw - .3f,pcz - hd - .3f,pcx + hw + .3f,pcz + hd + .3f });
            }
        }
    }

    ch.count = (GLsizei)V.size();
    glGenVertexArrays(1, &ch.vao); glGenBuffers(1, &ch.vbo);
    glBindVertexArray(ch.vao);
    glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(V.size() * sizeof(Vert)), V.data(), GL_STATIC_DRAW);
    constexpr int S = 7 * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, S, (void*)0);             glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, S, (void*)(3 * sizeof(float))); glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, S, (void*)(6 * sizeof(float))); glEnableVertexAttribArray(2);
    glBindVertexArray(0);
}

static void freeChunk(Chunk& ch) {
    if (ch.vao) glDeleteVertexArrays(1, &ch.vao);
    if (ch.vbo) glDeleteBuffers(1, &ch.vbo);
}

struct CK { int cx, cz; bool operator==(const CK& o)const { return cx == o.cx && cz == o.cz; } };
struct CKH {
    size_t operator()(const CK& k)const {
        return std::hash<long long>()((long long)k.cx << 32 | (unsigned)k.cz);
    }
};

// COLLISION
static std::unordered_map<CK, Chunk, CKH>* g_chunks = nullptr;

static bool hitsBuilding(float px, float pz, float r) {
    int ccx = (int)floorf(px / CHUNK_SIZE), ccz = (int)floorf(pz / CHUNK_SIZE);
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
        auto it = g_chunks->find({ ccx + dx,ccz + dz });
        if (it == g_chunks->end()) continue;
        for (auto& fp : it->second.fps)
            if (px + r > fp.x0 && px - r<fp.x1 && pz + r>fp.z0 && pz - r < fp.z1) return true;
    }
    return false;
}
static Vec3 resolveXZ(Vec3 old, Vec3 neo, float r) {
    if (!hitsBuilding(neo.x, neo.z, r)) return neo;
    Vec3 tx = { neo.x,neo.y,old.z }; if (!hitsBuilding(tx.x, tx.z, r)) return tx;
    Vec3 tz = { old.x,neo.y,neo.z }; if (!hitsBuilding(tz.x, tz.z, r)) return tz;
    return{ old.x,neo.y,old.z };
}

// SHADERS
static const char* CITY_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in float aMat;
uniform mat4 uVP;
uniform mat4 uView;
uniform vec3 uCamPos;
out vec3 vWorld;
out vec3 vNorm;
out vec3 vViewPos;    // position in view space (for SSR)
out float vMat;
out float vDist;
out vec2 vTexUV;      // world-space UV for texture sampling
void main(){
    vWorld  = aPos;
    vNorm   = aNorm;
    vMat    = aMat;
    vDist   = length(aPos - uCamPos);
    vec4 cp = uVP * vec4(aPos, 1.0);
    vViewPos = (uView * vec4(aPos, 1.0)).xyz;
    // Tri-planar UV: pick dominant axis for projection
    vec3 aN = abs(aNorm);
    if(aN.y > aN.x && aN.y > aN.z)      vTexUV = aPos.xz;
    else if(aN.x > aN.z)                 vTexUV = aPos.zy;
    else                                 vTexUV = aPos.xy;
    gl_Position = cp;
}
)GLSL";

static const char* CITY_FS = R"GLSL(
#version 330 core
in vec3 vWorld;
in vec3 vNorm;
in vec3 vViewPos;
in float vMat;
in float vDist;
in vec2 vTexUV;
out vec4 fragColor;

uniform vec3  uCamPos, uSunDir, uSunCol, uSkyAmb, uFogCol;
uniform vec3  uCityGlow;
uniform float uSunI, uSkyAmbI, uFogDensity, uSunWrap;
uniform float uTime;
uniform float uNightFactor;
uniform mat4  uVP;
uniform mat4  uView;
uniform mat4  uProj;
uniform vec2  uResolution;
uniform sampler2D uSceneTex;
uniform sampler2D uDepthTex;
// Shadow map
uniform sampler2D uShadowMap;
uniform mat4      uLightVP;
uniform float     uShadowBias;
uniform float     uShadowSoftness;
// Wet road
uniform float uRoadWetAmount;
uniform float uRoadWetRoughness;
// Surface textures (slots 5, 6, 7)
uniform sampler2D uWallTex;
uniform sampler2D uRoadTex;
uniform sampler2D uSidewalkTex;
// Texture scales and blend
uniform float uWallScale;
uniform float uRoadScale;
uniform float uSidewalkScale;
uniform float uTexBlend;
uniform sampler2D uWallNormal;
uniform sampler2D uRoadNormal;
uniform sampler2D uSidewalkNormal;

float hash2(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453); }
float hash3v(vec3 p){ return fract(sin(dot(p,vec3(127.1,311.7,74.7)))*43758.5453); }
float vnoise(vec2 p){
    vec2 i=floor(p),f=fract(p); f=f*f*(3.-2.*f);
    return mix(mix(hash2(i),hash2(i+vec2(1,0)),f.x),
               mix(hash2(i+vec2(0,1)),hash2(i+vec2(1,1)),f.x),f.y);
}

float D_GGX(float NdH,float r){float a=r*r,a2=a*a,d=NdH*NdH*(a2-1.)+1.;return a2/(3.14159*d*d+1e-4);}
float G_smith(float NdV,float NdL,float r){float k=(r+1.)*(r+1.)/8.;return(NdV/(NdV*(1.-k)+k))*(NdL/(NdL*(1.-k)+k));}
vec3 F_s(float c,vec3 f0){return f0+(1.-f0)*pow(clamp(1.-c,0.,1.),5.);}
vec3 normalFromMap(vec3 N, vec2 uv, sampler2D map)
{
    vec3 texN = texture(map, uv).xyz * 2.0 - 1.0;

    vec3 T = normalize(cross(N, vec3(0.0,1.0,0.0001)));
    vec3 B = cross(N, T);

    mat3 TBN = mat3(T, B, N);
    return normalize(TBN * texN);
}

// PCF soft shadow — 3x3 Poisson samples
float shadowPCF(vec4 lsPos){
    vec3 proj = lsPos.xyz / lsPos.w;
    proj = proj * 0.5 + 0.5;
    if(proj.x<0.||proj.x>1.||proj.y<0.||proj.y>1.||proj.z>1.) return 1.0;
    float currentDepth = proj.z - uShadowBias;
    // 9-tap PCF grid
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));
    float shadow = 0.0;
    for(int x=-1;x<=1;x++) for(int y=-1;y<=1;y++){
        float pcfDepth = texture(uShadowMap, proj.xy + vec2(x,y)*texelSize*uShadowSoftness).r;
        shadow += currentDepth > pcfDepth ? 1.0 : 0.0;
    }
    return 1.0 - shadow / 9.0;
}

// Procedural normal perturbation — breaks up flat concrete without a texture
vec3 perturbNormal(vec3 N, vec2 uv, float strength){
    float h0  = vnoise(uv * 2.3);
    float hx  = vnoise(uv * 2.3 + vec2(0.01, 0.0));
    float hy  = vnoise(uv * 2.3 + vec2(0.0, 0.01));
    vec2 grad = vec2(hx - h0, hy - h0) * strength * 80.0;
    // Build tangent frame from N
    vec3 T = normalize(cross(N, vec3(0,1,0.0001)));
    vec3 B = cross(N, T);
    return normalize(N + T * grad.x + B * grad.y);
}

// Procedural sky cubemap — analytically matches the sky shader.
vec3 sampleEnv(vec3 dir){
    dir = normalize(dir);
    float ny = dir.y;
    vec3 hz = uFogCol * 1.1;
    vec3 ms = mix(uFogCol, uSkyAmb, 0.5) * 1.3;
    vec3 ze = uSkyAmb * 0.8;
    vec3 sky;
    if(ny < 0.0){
        // Below horizon: blend to fog colour so reflected ground matches scene fog
        sky = mix(hz, hz * 0.15, clamp(-ny * 3.5, 0.0, 1.0));
    } else {
        float t0 = smoothstep(0.0, 0.12, ny);
        float t1 = smoothstep(0.05, 0.65, ny);
        sky = mix(mix(hz, ms, t0), ze, t1 * t1);
    }
    sky *= (1.0 - uNightFactor * 0.80);
    sky += vec3(0.018, 0.025, 0.055) * uNightFactor;
    float sunAbove = clamp(uSunDir.y * 3.0, 0.0, 1.0) * (1.0 - uNightFactor);
    if(sunAbove > 0.0){
        vec3 sunN = normalize(uSunDir);
        float sd  = dot(dir, sunN);
        sky += uSunCol * pow(max(sd, 0.0), 8.0)  * 0.5 * sunAbove;
        sky += uSunCol * pow(max(sd, 0.0), 48.0) * 1.2 * sunAbove;
        if(sd > 0.9995)
            sky = mix(sky, uSunCol * 4.0, smoothstep(0.9995, 0.9999, sd) * sunAbove);
    }
    return sky;
}

// Improved SSR: 32 linear steps + 8 binary refinement steps
vec4 ssrSample(vec3 wp, vec3 wn, float rough){
    if(rough > 0.25) return vec4(0.);
    vec3 vP = vViewPos;
    vec3 vN = normalize((uView * vec4(wn, 0.)).xyz);
    vec3 vR = normalize(reflect(normalize(vP), vN));
    if(vR.z > -0.01) return vec4(0.);

    // Linear march — 32 steps with constant stride
    const int LINEAR_STEPS = 16;
    float stride = 3.5;
    vec3 vRay = vP;
    float hit = 0.0; vec2 huv = vec2(0.);
    vec3 prevRay = vP;

    for(int i = 1; i <= LINEAR_STEPS; i++){
        prevRay = vRay;
        vRay += vR * stride;
        vec4 cR = uProj * vec4(vRay, 1.); if(cR.w <= 0.) break;
        vec3 nd = cR.xyz / cR.w;
        vec2 su = nd.xy * 0.5 + 0.5;
        if(su.x < 0. || su.x > 1. || su.y < 0. || su.y > 1.) break;
        float sceneDepth = texture(uDepthTex, su).r;
        float rayDepth   = nd.z * 0.5 + 0.5;
        if(rayDepth > sceneDepth && rayDepth - sceneDepth < 0.4){
            // Binary refinement — bisect between prevRay and vRay
            vec3 lo = prevRay, hi = vRay;
            for(int b = 0; b < 4; b++){
                vec3 mid = (lo + hi) * 0.5;
                vec4 mc = uProj * vec4(mid, 1.); if(mc.w <= 0.) break;
                vec3 mn = mc.xyz / mc.w;
                vec2 mu = mn.xy * 0.5 + 0.5;
                float md = texture(uDepthTex, mu).r;
                float mr = mn.z * 0.5 + 0.5;
                if(mr > md) hi = mid; else lo = mid;
                huv = mu;
            }
            hit = 1.0; break;
        }
    }
    if(hit < 0.5) return vec4(0.);
    vec2 ef = 1. - pow(abs(huv * 2. - 1.), vec2(6.));
    return vec4(texture(uSceneTex, huv).rgb, clamp(ef.x * ef.y * (1. - rough * 4.), 0., 1.));
}

void main(){
    vec3 N=normalize(vNorm), V=normalize(uCamPos-vWorld), L=normalize(uSunDir), H=normalize(L+V);
    float NdL=max(dot(N,L),0.), NdV=max(dot(N,V),.001), NdH=max(dot(N,H),0.), VdH=max(dot(V,H),0.);

    float anx=abs(N.x), any=abs(N.y), anz=abs(N.z);
    bool isH=(any>0.5);
    vec2 uv;
    if(isH)          uv=vec2(vWorld.x, vWorld.z);
    else if(anx>anz) uv=vec2(vWorld.z, vWorld.y);
    else             uv=vec2(vWorld.x, vWorld.y);

    // Road/sidewalk detected from world position (no separate geometry needed)
    float _dX = abs(mod(vWorld.z + 48., 96.) - 48.);
    float _dZ = abs(mod(vWorld.x + 48., 96.) - 48.);
    bool isRoad   = isH && (_dX < 4.4 || _dZ < 4.4);
    bool isSW     = isH && !isRoad && (_dX < 5.9 || _dZ < 5.9);
    bool isFacade = (vMat>0.45&&vMat<0.55)&&!isH;

    vec3  alb  = vec3(0.28,0.26,0.24);
    float rough= 0.84;
    float metal= 0.0;
    vec3  f0   = vec3(0.04);
    bool  isWin=false, isLW=false;
    vec3  wEmit=vec3(0.);

    if(isRoad){
        float agg  = vnoise(uv*3.5)*.5 + vnoise(uv*9.)*.3 + vnoise(uv*22.)*.2;
        float wear = vnoise(uv*.8)*.5  + vnoise(uv*2.5)*.5;
        alb = mix(vec3(.07,.07,.08), vec3(.17,.16,.15), agg*.5+wear*.3);
        float oil = smoothstep(.55,.72,wear)*.35;
        alb = mix(alb, vec3(.11,.10,.13), oil);
        // Blend in road texture
        vec3 roadTex = texture(uRoadTex, vTexUV / uRoadScale).rgb;
        roadTex = pow(roadTex, vec3(2.2)); // sRGB -> linear
        alb = mix(alb, alb * roadTex * 2.0, uTexBlend);
        // Lane markings — reuse road distances already computed above
        float dFromXRoad = _dX;
        float dFromZRoad = _dZ;
        bool onXRoad = dFromXRoad < 4.4;
        bool onZRoad = dFromZRoad < 4.4;
        bool atIntersection = onXRoad && onZRoad;
        // Centre dashes: yellow, suppress at intersections
        float dashX = mod(vWorld.x, 6.) < 3. ? 1. : 0.;
        float dashZ = mod(vWorld.z, 6.) < 3. ? 1. : 0.;
        float cL = 0.;
        if(onXRoad && !atIntersection) cL = smoothstep(.20,.05,dFromXRoad) * dashX;
        if(onZRoad && !atIntersection) cL = smoothstep(.20,.05,dFromZRoad) * dashZ;
        // Edge lines: white, suppress at intersections
        float eL = 0.;
        if(onXRoad && !atIntersection) eL = smoothstep(.18,.04,abs(dFromXRoad - 3.5));
        if(onZRoad && !atIntersection) eL = smoothstep(.18,.04,abs(dFromZRoad - 3.5));
        alb=mix(alb,vec3(.88,.80,.22),cL*.85);
        alb=mix(alb,vec3(.80,.80,.78),eL*.70);
        // Wet road: noise-driven roughness puddles
        float wetMask = clamp(wear * 1.3, 0.0, 1.0) * uRoadWetAmount;
        wetMask *= (1.0 - cL) * (1.0 - eL);  // lane markings stay dry-looking
        rough = mix(0.96, uRoadWetRoughness, wetMask);
        f0    = mix(vec3(0.008), vec3(0.04), wetMask);
        metal = 0.0;
    }
    else if(isSW){
        vec2 pF=fract(uv*.08);  // ~12 world-unit slabs, visible from standing height
        vec2 gr=smoothstep(vec2(0.),vec2(.03),pF)*(1.-smoothstep(vec2(.97),vec2(1.),pF));
        float gM=1.-min(gr.x,gr.y);
        float pv=hash2(floor(uv*.08))*.15;
        vec3 pb=(vMat>.165)
            ? vec3(.42,.39,.35)+pv
            : vec3(.28,.26,.23)+pv*.4;
        pb+=vnoise(uv*0.35)*.05-.025;
        pb=clamp(pb-vnoise(uv*.06)*.07*vec3(.7,.75,.8),0.,1.);
        alb=mix(pb,pb*.30,gM*.85);  // grout 70% darker, high contrast
        // Blend in sidewalk texture
        vec3 swTex = texture(uSidewalkTex, vTexUV / uSidewalkScale).rgb;
        swTex = pow(swTex, vec3(2.2));
        alb = mix(alb, alb * swTex * 2.0, uTexBlend * (1.0 - gM * 0.5));
        rough=0.90; f0=vec3(0.015);
        N = perturbNormal(N, uv * 0.08, 0.003);
    }
    else if(isFacade){
        float hf=clamp(vWorld.y/300.,0.,1.), cv=clamp(vMat*2.,0.,1.);
        vec3 c=mix(vec3(.09,.08,.07), mix(vec3(.38,.32,.26), vec3(.22,.24,.28), hf*.8), cv);
        c+=vnoise(uv*.12)*.06-.03; c+=vnoise(uv*1.4)*.025-.012;
        c=clamp(c-vnoise(vec2(uv.x*.35,uv.y*.04))*.12*vec3(.75,.8,.9),0.,1.);
        // Blend wall texture on facade surfaces only (not windows)
        vec3 wallTex = texture(uWallTex, vTexUV / uWallScale).rgb;
        wallTex = pow(wallTex, vec3(2.2));
        c = mix(c, c * wallTex * 1.8, uTexBlend);
        alb=c; rough=0.84;
        // Procedural normal bump on concrete facade
        N = perturbNormal(N, uv * 0.15, 0.004);
        float fy=mod(uv.y,3.4)/3.4, fx=mod(uv.x,2.6)/2.6;
        if(fx>.15&&fx<.85&&fy>.10&&fy<.80){
            isWin=true;
            float lr=hash2(floor(vec2(uv.x/2.6,uv.y/3.4)));
            isLW=(lr>1.-uNightFactor*.65);
            float t=hash2(floor(uv*.025)), t2=hash2(floor(uv*.015)+vec2(5.3,2.1));
            vec3 glass=mix(vec3(.03,.06,.13), vec3(.04,.09,.08), t2);
            alb  =isLW?mix(vec3(.28,.22,.10), vec3(.20,.30,.12), t):glass;
            rough=isLW?.35:.03; f0=vec3(.05);
            // Night: boost emission significantly so bloom fires at lower threshold
            float emitScale = mix(1.4, 5.5, uNightFactor);
            if(isLW){ wEmit=mix(vec3(.90,.65,.22), vec3(.60,.85,.35), t)*emitScale*(1.+.03*sin(uTime*3.7+lr*40.)); }
        } else {
            alb=c*.78;
        }
    }
    else {
        float hf=clamp(vWorld.y/300.,0.,1.), cv=clamp(vMat*2.,0.,1.);
        vec3 c=mix(vec3(.09,.08,.07), mix(vec3(.38,.32,.26), vec3(.22,.24,.28), hf*.8), cv);
        c+=vnoise(uv*.12)*.06-.03; c+=vnoise(uv*1.4)*.025-.012;
        alb=clamp(c-vnoise(vec2(uv.x*.35,uv.y*.04))*.12*vec3(.75,.8,.9),0.,1.);
        // Light normal bump on roofs/walls
        N = perturbNormal(N, uv * 0.2, 0.003);
    }

    // Recompute dot products after normal perturbation
    NdL=max(dot(N,L),0.); NdV=max(dot(N,V),.001); NdH=max(dot(N,H=normalize(L+V)),0.); VdH=max(dot(V,H),0.);

    // PBR
    f0=mix(f0,alb,metal);
    vec3 diff=alb/3.14159;
    float D=D_GGX(NdH,rough), G=G_smith(NdV,clamp(NdL,.001,1.),rough);
    vec3 Fv=F_s(VdH,f0), spec=(NdL>0.)?(D*G*Fv)/max(4.*NdV*NdL,.001):vec3(0.);
    vec3 kd=(1.-Fv)*(1.-metal);

    // Hemisphere ambient
    float hemi=dot(N,vec3(0,1,0))*.5+.5, hW=1.-abs(dot(N,vec3(0,1,0)));
    vec3 groundColDay  = vec3(0.05,0.04,0.04);
    vec3 groundColNight= vec3(0.02,0.022,0.04);
    vec3 groundCol = mix(groundColDay, groundColNight, uNightFactor);
    vec3 envA = mix(mix(groundCol, mix(uFogCol,uSkyAmb,.4)*1.1, hW), uSkyAmb*1.5, hemi*hemi);
    vec3 amb = envA * uSkyAmbI * alb;

    // City glow
    float cityHemi = clamp(dot(N, vec3(0,1,0)) * 0.5 + 0.7, 0., 1.);
    vec3 cityAmb = uCityGlow * cityHemi * alb * (isWin ? 0.3 : 1.0);

    // Moonlight
    vec3 moonDir = normalize(vec3(-uSunDir.x, abs(uSunDir.y)+0.15, -uSunDir.z));
    float moonNdL = max(dot(N, moonDir), 0.0);
    vec3 moonLit = vec3(0.16,0.20,0.38)*0.045*moonNdL*uNightFactor*alb;

    vec3 nightFloor = vec3(0.007,0.007,0.011)*alb;

    float cavity = 1. - hash3v(floor(vWorld*.5))*0.06;
    cavity *= mix(0.70,1.0,clamp(dot(N,vec3(0,1,0))*.5+.5,0.,1.));

    // Shadow
    vec4 lsPos = uLightVP * vec4(vWorld, 1.0);
    float shadowFactor = shadowPCF(lsPos);
    // Fade shadow at night (moon is too dim to cast hard shadows)
    shadowFactor = mix(shadowFactor, 1.0, uNightFactor * 0.85);

    float sunAboveHorizon = clamp(uSunDir.y * 8.0, 0.0, 1.0);
    float wrap = clamp((dot(N,L) + uSunWrap) / (1.0 + uSunWrap), 0., 1.);
    vec3 sunLit = uSunCol * uSunI * mix(0.08, 1.0, wrap) * (kd*diff+spec) * NdL * sunAboveHorizon * shadowFactor;

    // Rim fill
    vec3 dayRimDir   = normalize(-uSunDir + vec3(0,0.3,0));
    vec3 rimDir      = normalize(mix(dayRimDir, moonDir, uNightFactor));
    vec3 dayRimCol   = uSkyAmb * 0.20;
    vec3 nightRimCol = vec3(0.10,0.14,0.28)*0.08*uNightFactor;
    vec3 rimCol      = mix(dayRimCol, nightRimCol, uNightFactor);
    vec3 rimLit      = rimCol * clamp(dot(N, rimDir), 0., 1.) * alb;

    vec3 lit = amb*cavity + sunLit + rimLit + wEmit + cityAmb + moonLit + nightFloor;

    // Window glass
    if(isWin){
        vec3 reflDir = reflect(-V, N);
        float cosTheta = max(dot(N, V), 0.0);
        vec3  schlick  = f0 + (vec3(1.0)-f0)*pow(1.0-cosTheta, 5.0);
        float fresnelW = max(max(schlick.r,schlick.g),schlick.b);
        fresnelW = mix(fresnelW, fresnelW*0.4, float(isLW));
        vec3 skyRefl = sampleEnv(reflDir);
        float sunSpec = pow(max(dot(reflDir, normalize(uSunDir)), 0.0), 64.0);
        skyRefl += uSunCol * sunSpec * (1.0-uNightFactor) * 1.5;
        vec4 ssr = vec4(0.0);
        if(rough < 0.25) ssr = ssrSample(vWorld, N, rough);
        vec3 reflCol = skyRefl;
        if(ssr.a > 0.01) reflCol = mix(skyRefl, ssr.rgb, clamp(ssr.a*1.2,0.,1.));
        lit = mix(lit, reflCol, clamp(fresnelW*1.1,0.,0.92));
        float sg = pow(max(dot(reflect(-L,N),V),0.0),220.0)*2.5*clamp(NdL*2.,0.,1.);
        lit += uSunCol*uSunI*sg*(1.0-uNightFactor)*0.6;
    }
    // Wet road reflections: also do SSR and sky probe on wet patches
    if(isRoad && rough < 0.25){
        vec3 reflDir = reflect(-V, N);
        float cosTheta = max(dot(N,V),0.0);
        vec3  schlick  = f0 + (vec3(1.0)-f0)*pow(1.0-cosTheta,5.0);
        float fresnelW = max(max(schlick.r,schlick.g),schlick.b) * uRoadWetAmount;
        vec3 skyRefl = sampleEnv(reflDir);
        vec4 ssr = ssrSample(vWorld, N, rough);
        vec3 reflCol = (ssr.a > 0.01) ? mix(skyRefl, ssr.rgb, clamp(ssr.a*1.5,0.,1.)) : skyRefl;
        lit = mix(lit, reflCol * 0.7, clamp(fresnelW, 0.0, 0.5));
    }

    // Pre-composite fog: only applied at distance > 400 units to avoid washing out nearby ground
    float fogD=exp(-max(vDist-400.,0.)*uFogDensity*0.3);
    vec3 aerFog=mix(uFogCol, uFogCol+vec3(.04,.06,.12), clamp(1.-fogD,0.,1.)*.4);
    lit=mix(aerFog, lit, clamp(fogD, 0.1, 1.));

    fragColor=vec4(lit, 1.);
}
)GLSL";

static const char* SKY_VS = R"GLSL(
#version 330 core
out vec2 vUV;
void main(){
    vec2 p=vec2((gl_VertexID==1)?4.:-1.,(gl_VertexID==2)?4.:-1.);
    vUV=p*.5+.5; gl_Position=vec4(p,.9999,1.);
}
)GLSL";

static const char* SKY_FS = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor;
uniform vec3 uHorizon,uMidSky,uZenith,uBelow,uSunDir;
uniform vec3 uCamFwd,uCamRgt,uCamUp;
uniform float uTanHFov,uAspect;
uniform float uTime;
uniform float uNightFactor;

vec3 aces(vec3 x){ float a=2.51,b=0.03,c=2.43,d=0.59,e=0.14; return clamp((x*(a*x+b))/(x*(c*x+d)+e),0.,1.); }
float ss(float e0,float e1,float x){ float t=clamp((x-e0)/(e1-e0),0.,1.); return t*t*(3.-2.*t); }
float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5); }
float hash3(vec3 p){ return fract(sin(dot(p,vec3(127.1,311.7,74.7)))*43758.5); }

// Value noise
float vnoise2(vec2 p){
    vec2 i=floor(p), f=fract(p); f=f*f*(3.-2.*f);
    return mix(mix(hash(i),hash(i+vec2(1,0)),f.x), mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);
}

// FBM cloud noise — multiple octaves of value noise
float cloudFBM(vec2 uv){
    float v=0., a=0.5, f=1.;
    for(int i=0;i<5;i++){ v+=vnoise2(uv*f)*a; a*=0.5; f*=2.1; }
    return v;
}

// Stratus-style cloud layer — ray hits a flat cloud sheet at height H
float sampleCloud(vec3 rayDir, float H, float baseY){
    // Only upward-ish rays hit clouds
    if(rayDir.y < 0.005) return 0.;
    float t = H / rayDir.y;  // parametric hit distance
    // World XZ at cloud layer
    vec2 cUV = rayDir.xz * t * 0.00012 + vec2(uTime * 0.0012, 0.);
    float cloud = cloudFBM(cUV * vec2(1.4, 1.0));
    cloud = ss(0.38, 0.68, cloud);  // threshold + soft edge
    return cloud;
}

// Cirrus wispy clouds (higher, thinner)
float sampleCirrus(vec3 rayDir){
    if(rayDir.y < 0.04) return 0.;
    float t = 5000. / rayDir.y;
    vec2 cUV = rayDir.xz * t * 0.00005 + vec2(uTime * 0.0006, uTime * 0.0003);
    float c = vnoise2(cUV * 3.0) * 0.5 + vnoise2(cUV * 7.0) * 0.3 + vnoise2(cUV * 15.0) * 0.2;
    return ss(0.55, 0.75, c) * 0.7;
}

void main(){
    vec2 ndc = vUV*2.-1.;
    vec3 ray = normalize(uCamFwd + uCamRgt*ndc.x*uAspect*uTanHFov + uCamUp*ndc.y*uTanHFov);
    float ny = ray.y;

        vec3 sky;
    if(ny < 0.0){
        // Below horizon blends from horizon colour to a deep version of it
        // (not hardcoded black — tracks the day/night colour palette)
        vec3 groundSky = uHorizon * 0.18;  // dark but same hue as horizon
        sky = mix(uHorizon, groundSky, ss(0., -0.25, ny));
    } else {
        vec3 low  = mix(uHorizon, uMidSky, ss(0., 0.10, ny));
        vec3 high = mix(low, uZenith, ss(0.05, 0.70, ny)*ss(0.05,0.70,ny));
        sky = high;
    }

        float horizBand = exp(-abs(ny)*16.);
    float sunHoriz  = max(dot(ray, normalize(vec3(uSunDir.x,0.,uSunDir.z))), 0.);
    sunHoriz = pow(sunHoriz, 5.);
    sky = mix(sky, vec3(1.0,0.55,0.18)*1.6, horizBand * sunHoriz * 0.65 * (1.0 - uNightFactor));

        float hazeStr = 0.30 * (1.0 - uNightFactor * 0.85); // nearly gone at night
    float haze = ss(-.06, .06, ny) * hazeStr;
    vec3 hazeCol = mix(vec3(0.68,0.70,0.74), uHorizon * 0.6, uNightFactor);
    sky = mix(sky, hazeCol, haze);

        float sunVis = 1.0 - uNightFactor;
    float sd = dot(ray, normalize(uSunDir));
    // Scattered corona (wide, orange)
    sky += vec3(1.0,0.72,0.38) * pow(max(sd,0.),10.) * 0.8 * sunVis;
    // Inner glow
    sky += vec3(1.0,0.90,0.65) * pow(max(sd,0.),50.) * 1.8 * sunVis;
    // Disk edge
    if(sd > 0.9992) sky = mix(sky, vec3(1.8,1.5,1.1), ss(0.9992,0.9997,sd) * sunVis);
    // Disk centre (overbright for bloom)
    if(sd > 0.9997) sky = mix(sky, vec3(4.5,4.0,3.0), ss(0.9997,0.9999,sd) * sunVis);

        // March toward sun in screen space and accumulate occlusion
    if(ny > -0.05){
        vec2 sunScreen = vec2(
            dot(ray, uCamRgt) / (uAspect * uTanHFov),
            dot(ray, uCamUp)  / uTanHFov
        );
        // Sample a few steps from pixel toward sun direction in screen space
        const int GOD_STEPS = 6;
        float godAcc = 0.;
        for(int gi = 1; gi <= GOD_STEPS; gi++){
            float fgi = float(gi) / float(GOD_STEPS);
            vec2 sampleDir = mix(ndc, sunScreen, fgi * 0.5);
            // Rough cloud density at this direction
            vec3 sampleRay = normalize(uCamFwd + uCamRgt*sampleDir.x*uAspect*uTanHFov + uCamUp*sampleDir.y*uTanHFov);
            float cDens = sampleCloud(sampleRay, 1200., 0.) * 0.6
                        + sampleCirrus(sampleRay) * 0.3;
            godAcc += (1. - cDens) / float(GOD_STEPS);
        }
        godAcc = clamp(godAcc, 0., 1.);
        // Sun must be above horizon for god rays
        float sunAbove = clamp(uSunDir.y * 8., 0., 1.);
        float godRay = pow(max(sd, 0.), 4.) * 0.35 * godAcc * sunAbove * (1.0 - uNightFactor);
        sky += vec3(1.0, 0.80, 0.55) * godRay;
    }

        float cloud1 = sampleCloud(ray, 1200., 0.);
    if(cloud1 > 0.001 && ny > 0.0){
        // Lit from above by sun, dark below
        float sunLit  = max(dot(vec3(0,1,0), normalize(uSunDir)), 0.2);
        vec3 cloudCol = mix(vec3(0.35,0.32,0.30), vec3(1.0,0.97,0.93), sunLit);
        // Silver lining near sun
        float silverT = max(sd, 0.) * 0.5;
        cloudCol = mix(cloudCol, vec3(1.2,1.1,1.0), silverT * cloud1);
        sky = mix(sky, cloudCol, cloud1 * clamp(ny * 6., 0., 1.));
    }

        float cirrus = sampleCirrus(ray);
    if(cirrus > 0.001 && ny > 0.0){
        float sunLit = max(dot(vec3(0,1,0), normalize(uSunDir)), 0.3);
        vec3  cirCol = mix(vec3(0.6,0.62,0.68), vec3(0.97,0.95,0.92), sunLit);
        sky = mix(sky, cirCol, cirrus * clamp(ny * 10., 0., 1.) * 0.85);
    }

        if(uNightFactor > 0.01 && ny > 0.0){
        // Hash-based stars — stable per direction
        vec3 starDir = ray * 200.0;
        vec2 starCell = floor(starDir.xz / (1.0 - starDir.y * 0.3)) * 0.07;
        float starH = hash(starCell + vec2(3.1, 7.9));
        float starB = hash(starCell);
        // Only show brightest ~2% as stars
        float starMask = smoothstep(0.982, 0.998, starH) * clamp(ny * 5., 0., 1.);
        // Twinkle
        float twinkle = 0.7 + 0.3 * sin(uTime * (3.0 + starB * 7.0) + starB * 100.0);
        vec3 starCol = mix(vec3(0.8,0.9,1.0), vec3(1.0,0.95,0.8), starB) * 1.4;
        sky += starCol * starMask * twinkle * uNightFactor;

        // Faint milky-way band
        float mwBand = exp(-abs(dot(ray, vec3(0.3,0.1,0.95))) * 6.0);
        float mwNoise = vnoise2(ray.xz * 8.0 + vec2(1.3,2.7)) * 0.5
                      + vnoise2(ray.xz * 18.0 + vec2(5.1,3.2)) * 0.3;
        sky += vec3(0.15,0.18,0.28) * mwBand * mwNoise * 0.25 * uNightFactor * clamp(ny*3.,0.,1.);
    }

        if(uNightFactor > 0.05){
        // Moon is opposite to sun direction
        vec3 moonDir = normalize(-uSunDir + vec3(0., 0.15, 0.));
        float md = dot(ray, moonDir);
        // Moon disk
        if(md > 0.9985){
            float moonEdge = smoothstep(0.9985, 0.9993, md);
            vec3 moonCol = vec3(0.85, 0.88, 0.95) * 1.8;
            sky = mix(sky, moonCol, moonEdge * uNightFactor);
        }
        // Moon glow halo
        sky += vec3(0.3,0.35,0.5) * pow(max(md,0.), 80.) * 0.25 * uNightFactor;
    }

    // Output linear HDR — tonemapping happens in composite
    fragColor = vec4(sky, 1.);
}
)GLSL";

// POST-PROCESS SHADERS
// Fullscreen quad VS (reused for all post passes)
static const char* QUAD_VS = R"GLSL(
#version 330 core
out vec2 vUV;
void main(){
    vec2 p=vec2((gl_VertexID==1)?4.:-1.,(gl_VertexID==2)?4.:-1.);
    vUV=p*.5+.5; gl_Position=vec4(p,0.,1.);
}
)GLSL";

// Brightness threshold extract for bloom
static const char* BLOOM_THRESH_FS = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor;
uniform sampler2D uTex;
uniform float uThreshold;
void main(){
    vec3 c=texture(uTex,vUV).rgb;
    float lum=dot(c,vec3(0.2126,0.7152,0.0722));
    float w=max(lum-uThreshold,0.0)/(lum+0.001);
    fragColor=vec4(c*w,1.0);
}
)GLSL";

// Dual Kawase blur pass
static const char* BLOOM_BLUR_FS = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexelSize;
uniform float uOffset;
void main(){
    vec3 sum=texture(uTex,vUV).rgb*4.0;
    sum+=texture(uTex,vUV+vec2( uOffset+0.5, uOffset+0.5)*uTexelSize).rgb;
    sum+=texture(uTex,vUV+vec2(-uOffset-0.5, uOffset+0.5)*uTexelSize).rgb;
    sum+=texture(uTex,vUV+vec2( uOffset+0.5,-uOffset-0.5)*uTexelSize).rgb;
    sum+=texture(uTex,vUV+vec2(-uOffset-0.5,-uOffset-0.5)*uTexelSize).rgb;
    fragColor=vec4(sum/8.0,1.0);
}
)GLSL";

// Renders scene depth from sun's point of view into a depth texture.
static const char* SHADOW_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=2) in float aMat;
uniform mat4 uLightVP;
void main(){
    gl_Position = uLightVP * vec4(aPos, 1.0);
}
)GLSL";

static const char* SHADOW_FS = R"GLSL(
#version 330 core
void main(){ }
)GLSL";

// Hemisphere sampling in view space, output occlusion factor 0-1.
static const char* SSAO_FS = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor;
uniform sampler2D uDepth;
uniform sampler2D uNormal;
uniform mat4 uProj;
uniform mat4 uInvProj;
uniform float uNearP, uFarP;
uniform float uRadius;
uniform float uBias;
uniform float uStrength;
uniform vec2  uNoiseScale;
uniform float uTime;
uniform int   uSamples;

float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5); }
vec2 hash2(vec2 p){ return vec2(hash(p), hash(p+vec2(43.3,17.7))); }

float linearDepth(float d){
    float ndc = d*2.0-1.0;
    return (2.0*uNearP*uFarP)/(uFarP+uNearP-ndc*(uFarP-uNearP));
}

vec3 viewPos(vec2 uv){
    float d = texture(uDepth, uv).r;
    vec4 ndc = vec4(uv*2.0-1.0, d*2.0-1.0, 1.0);
    vec4 vp  = uInvProj * ndc;
    return vp.xyz / vp.w;
}

void main(){
    vec3 vP = viewPos(vUV);
    vec3 vN = normalize(texture(uNormal, vUV).xyz * 2.0 - 1.0);

    float angle = hash(vUV * 512.0 + fract(uTime * 0.07)) * 6.2831853;
    vec2 rot = vec2(cos(angle), sin(angle));

    vec3 T = normalize(vec3(rot, 0.0) - vN * dot(vec3(rot, 0.0), vN));
    vec3 B = cross(vN, T);
    mat3 TBN = mat3(T, B, vN);

    float occlusion = 0.0;
    int SAMPLES = max(uSamples, 1);
    for(int i = 0; i < SAMPLES; i++){
        float fi = float(i) / float(SAMPLES);
        float theta = fi * 6.2831853 * 2.39996323;
        float r     = sqrt(fi + 0.5/float(SAMPLES));
        float sinT  = sqrt(1.0 - r*r);
        vec3 hemi   = vec3(cos(theta)*sinT, sin(theta)*sinT, r);
        hemi.xy += (hash2(vUV * float(i+1) * 3.7) - 0.5) * 0.3;
        vec3 sampleDir = normalize(TBN * hemi);
        vec3 samplePos = vP + sampleDir * uRadius;

        vec4 sampleClip = uProj * vec4(samplePos, 1.0);
        sampleClip.xyz /= sampleClip.w;
        vec2 sampleUV = sampleClip.xy * 0.5 + 0.5;
        if(sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) continue;

        float sampleDepth = linearDepth(texture(uDepth, sampleUV).r);
        float sampleZ     = -samplePos.z;

        float rangeCheck = smoothstep(0.0, 1.0, uRadius / abs(-vP.z - sampleDepth));
        occlusion += (sampleDepth <= sampleZ - uBias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(SAMPLES)) * uStrength;
    fragColor = vec4(vec3(occlusion), 1.0);
}
)GLSL";

static const char* SSAO_BLUR_FS = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor;
uniform sampler2D uSSAO;
uniform vec2 uTexelSize;
void main(){
    float ao = 0.0;
    for(int x=-1;x<=2;x++) for(int y=-1;y<=2;y++)
        ao += texture(uSSAO, vUV + vec2(x,y)*uTexelSize).r;
    fragColor = vec4(vec3(ao / 16.0), 1.0);
}
)GLSL";

// Circle of confusion from linearised depth, two-pass box blur.
static const char* DOF_FS = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform float uNearP, uFarP;
uniform float uFocusDist;
uniform float uFocusRange;
uniform float uBlurStrength;
uniform vec2  uTexelSize;
uniform int   uHorizontal;

float linearDepth(float d){
    float ndc=d*2.0-1.0;
    return (2.0*uNearP*uFarP)/(uFarP+uNearP-ndc*(uFarP-uNearP));
}

void main(){
    float depth   = linearDepth(texture(uDepth, vUV).r);
    float coc     = clamp((depth - (uFocusDist + uFocusRange)) / uFocusRange, 0.0, 1.0);  // far-only DoF
    float blurRad = coc * uBlurStrength;

    // Only blur far objects (near focus stays sharp)
    vec3 col = texture(uScene, vUV).rgb;
    if(blurRad < 0.5){ fragColor = vec4(col, 1.0); return; }

    // 9-tap Gaussian in one axis
    vec3 acc = col * 0.2270270270;
    vec2 dir = uHorizontal==1 ? vec2(uTexelSize.x, 0.0) : vec2(0.0, uTexelSize.y);
    vec2 step1 = dir * blurRad;
    acc += texture(uScene, vUV + step1 * 1.3846153846).rgb * 0.3162162162;
    acc += texture(uScene, vUV - step1 * 1.3846153846).rgb * 0.3162162162;
    acc += texture(uScene, vUV + step1 * 3.2307692308).rgb * 0.0702702703;
    acc += texture(uScene, vUV - step1 * 3.2307692308).rgb * 0.0702702703;
    fragColor = vec4(acc, 1.0);
}
)GLSL";

// Renders view-space normals to a colour buffer for SSAO.
static const char* NORMAL_FS = R"GLSL(
#version 330 core
in vec3 vNorm;
in vec3 vViewPos;
out vec4 fragColor;
uniform mat4 uView;
void main(){
    vec3 vN = normalize(mat3(uView) * normalize(vNorm));
    fragColor = vec4(vN * 0.5 + 0.5, 1.0);
}
)GLSL";

// Final composite: AgX tonemap + depth fog + dual bloom + cinematic grade
static const char* COMPOSITE_FS = R"GLSL(
#version 330 core
in vec2 vUV; out vec4 fragColor;
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uBloom;
uniform sampler2D uBloomWide;
uniform sampler2D uSSAO;
uniform float uBloomStr;
uniform float uBloomTightMix;
uniform float uBloomWideMix;
uniform float uTime;
uniform float uNightFactor;
uniform vec3  uSunDir;
uniform vec3  uFogCol;
uniform vec2  uSunScreen;
uniform vec3  uCamFwd;
uniform float uNearP, uFarP;
// Fog
uniform float uFogNearDensity;
uniform float uFogDistDensity;
uniform float uFogHeightDensity;
uniform float uFogSkyFloor;
uniform float uFogMax;
uniform vec3  uFogColorDay;
uniform vec3  uFogColorNight;
// Tonemap / camera
uniform float uTonemapExposure;
uniform float uTonemapGamma;
uniform float uBarrelDistortion;
uniform float uChromAberBase;
uniform float uChromAberEdge;
// Juice
uniform vec2  uSquishOffset;   // xy screen-space shake + y squish
// Flare
uniform float uFlareStrength;
// Grade
uniform vec3  uGradeShadowTint;
uniform vec3  uGradeHighlightTint;
uniform float uGradeContrast;
uniform float uGradeSaturation;
uniform float uGradeVignette;
uniform float uGrainStrength;
// Atmospheric scattering
uniform float uAtmoBlueshift;
uniform float uAtmoStart;

float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5); }

// Linearise OpenGL depth buffer value to view-space distance
float linearDepth(float d, float near, float far){
    float ndc = d * 2.0 - 1.0;
    return (2.0 * near * far) / (far + near - ndc * (far - near));
}

vec3 agxDefaultContrastApprox(vec3 x){
    vec3 x2=x*x,x4=x2*x2;
    return 15.5*x4*x2 - 40.14*x4*x + 31.96*x4 - 6.868*x2*x + 0.4298*x2 + 0.1191*x - 0.00232;
}
vec3 agx(vec3 col){
    const mat3 m = mat3(
        0.842479062253094, 0.0784335999999992, 0.0792237451477643,
        0.0423282422610123,0.878468636469772,  0.0791661274605434,
        0.0423756549057051,0.0784336,          0.879142973793104);
    const float minEV=-12.47393, maxEV=4.026069;
    col=m*col; col=clamp(col,0.,1.);
    col=log2(col+1e-10); col=(col-minEV)/(maxEV-minEV); col=clamp(col,0.,1.);
    return agxDefaultContrastApprox(col);
}
vec3 agxEotf(vec3 col){
    const mat3 mi = mat3(
         1.19687900512017,  -0.0980208811401368,-0.0990297440797205,
        -0.0528968517574562, 1.13512395905546,  -0.0989611400896577,
        -0.0529716355144937,-0.0980434501171241, 1.15107367264116);
    col=pow(max(col,vec3(0.)),vec3(2.2)); col=mi*col; return clamp(col,0.,1.);
}

// Lens ghost
vec3 lensGhost(vec2 uv,vec2 sp,float off,vec3 tint,float rad,float str){
    vec2 gp=sp+(vec2(0.5)-sp)*(1.+off);
    float d=length(uv-gp);
    float ring=smoothstep(rad,rad*.5,d)*smoothstep(0.,rad*.3,d);
    float disk=smoothstep(rad*.4,0.,d);
    return tint*(ring*.55+disk*.25)*str;
}

void main(){
    vec2 uv=vUV + uSquishOffset;  // shake + squish offset
    uv = clamp(uv, 0.0, 1.0);
    vec2 center=uv-0.5;
    float dist2=dot(center,center);

    // Barrel distortion
    vec2 uvD=uv+center*dist2*uBarrelDistortion;

    // Chromatic aberration
    float ca=uChromAberBase+dist2*uChromAberEdge;
    vec3 col=vec3(
        texture(uScene,uvD+center*ca).r,
        texture(uScene,uvD).g,
        texture(uScene,uvD-center*ca).b);

        vec2 ts = vec2(1.0) / vec2(textureSize(uDepth, 0));
    float dC  = texture(uDepth, uv).r;
    float dN  = texture(uDepth, uv + vec2( 0, ts.y)).r;
    float dS  = texture(uDepth, uv + vec2( 0,-ts.y)).r;
    float dE  = texture(uDepth, uv + vec2( ts.x, 0)).r;
    float dW  = texture(uDepth, uv + vec2(-ts.x, 0)).r;
    float rawDepth = min(dC, min(min(dN, dS), min(dE, dW)));
    bool isSky = (dC > 0.9999 && dN > 0.9999 && dS > 0.9999 && dE > 0.9999 && dW > 0.9999);
    float viewDist = isSky ? uFarP : linearDepth(rawDepth, uNearP, uFarP);

        float ao = isSky ? 1.0 : texture(uSSAO, uv).r;
    col *= ao;

        // Night: lower effective threshold so window glow halos out more.
    // Bloom texture was rendered with a night-adjusted threshold (see CPU side).
    col += texture(uBloom,     uv).rgb * uBloomStr * uBloomTightMix;
    col += texture(uBloomWide, uv).rgb * uBloomStr * uBloomWideMix;

        vec2 sunNDC=uSunScreen, sunUV=sunNDC*0.5+0.5;
    float sunVis=clamp(uSunDir.y*5.+0.4,0.,1.);
    float lookDownKill=clamp(uCamFwd.y*4.+0.5,0.,1.);
    // Occlusion: if any geometry covers the sun's screen position, kill the flare.
    // Sample a small 5-tap cross at sunUV — if all are near 1.0 the sun is in clear
    // sky; if any is occluded by a building the minimum drops below 0.9999 and we
    // suppress. This stops the flare from appearing on building walls.
    bool sunOnScreen = (sunUV.x > 0.01 && sunUV.x < 0.99 && sunUV.y > 0.01 && sunUV.y < 0.99);
    // Occlusion: sample a 3x3 grid of depth taps around the sun's screen position.
    // Average them — if ANY geometry overlaps the sun disk the average drops below
    // the all-sky value of 1.0. We compare average vs a per-tap sky detection:
    // a tap is "sky" if it equals the maximum of the 9 samples (i.e. not foreground).
    // sunClear = fraction of taps that are pure sky — 0 if sun is behind buildings.
    float sunClearSum = 0.0;
    float spread = 4.0; // texels to spread the gather — wider = catches thin spires
    for(int dy = -1; dy <= 1; dy++){
        for(int dx = -1; dx <= 1; dx++){
            vec2 off = vec2(float(dx), float(dy)) * ts * spread;
            float d = sunOnScreen ? texture(uDepth, sunUV + off).r : 1.0;
            // A tap is sky if its raw (un-min-filtered) depth is essentially 1.0.
            // Use a very tight threshold: anything with geometry maps to < 0.99998
            // on a 0.1/20000 projection, sky = exactly 1.0 (clear-colour depth).
            sunClearSum += step(0.99998, d);
        }
    }
    // sunClear: 1.0 only if ALL 9 taps are sky, fades as geometry enters the disk
    float sunClear = sunClearSum / 9.0;
    sunClear = smoothstep(0.55, 1.0, sunClear); // soft edge: starts killing at ~5/9 taps occluded
    float sunPresence=exp(-max(length(sunNDC)-0.65,0.)*3.)*sunVis*lookDownKill*sunClear*uFlareStrength;
    vec3 flare=vec3(0.);
    float gd=length(uv-sunUV);
    vec3 haloCol=mix(vec3(1.,0.90,.60),vec3(1.,.60,.22),clamp(1.-uSunDir.y*5.,0.,1.));
    flare+=haloCol*(exp(-gd*30.)+exp(-gd*12.)*.25+exp(-gd*3.)*.08)*sunPresence;
    vec2 streakD=uv-sunUV;
    // sun streak removed — exp(-x*1.1) was too wide, creating full-screen white shaft
    flare+=lensGhost(uv,sunUV,.28,vec3(.85,.60,.20),.032,.12*sunPresence);
    flare+=lensGhost(uv,sunUV,.52,vec3(.40,.52,.90),.050,.08*sunPresence);
    flare+=lensGhost(uv,sunUV,.82,vec3(.90,.40,.30),.022,.06*sunPresence);
    flare+=lensGhost(uv,sunUV,1.12,vec3(.50,.80,.40),.042,.05*sunPresence);
    flare+=lensGhost(uv,sunUV,1.44,vec3(.70,.50,.90),.016,.04*sunPresence);
    col+=flare;

        col=agx(col*uTonemapExposure); col=agxEotf(col);
    col=pow(max(col,vec3(0.)),vec3(1./uTonemapGamma));

    // ============================================================
    // POST-TONEMAP SCREEN-SPACE FOG  --  Silent Hill style

    // Key properties:
    //   * Very dense: geometry beyond ~150u is nearly invisible
    //   * Ground-hugging: fog thickest at floor level, thinner overhead
    //   * Sky = fog: sky pixels get same fog colour, no horizon seam
    //   * Cold desaturated grey, independent of day/night sun palette
    //   * Near-field haze: even close surfaces (20-80u) show scatter
    // ============================================================

    // --- Silent Hill fog colour (driven by uniforms) ---
    vec3 fogColTM = mix(uFogColorDay, uFogColorNight, uNightFactor);

    // --- Layer 1: Near-field scatter ---
    float nearFog = 1.0 - exp(-viewDist * uFogNearDensity);
    nearFog = clamp(nearFog, 0.0, 1.0);

    // --- Layer 2: Exponential distance fog ---
    float fogDensity = mix(uFogDistDensity, uFogDistDensity * 1.75, uNightFactor);
    float expFog = 1.0 - exp(-viewDist * fogDensity);
    expFog = clamp(expFog, 0.0, 1.0);

    // --- Layer 3: Ground-hugging height fog (depth-gated, nearby floor stays clear) ---
    float heightFog = 1.0 - exp(-max(viewDist - 80.0, 0.0) * uFogHeightDensity * 0.4);
    heightFog = clamp(heightFog, 0.0, 1.0);

    // --- Combine ---
    float geoFog  = clamp(nearFog * 0.10 + expFog * 0.70 + heightFog * 0.30, 0.0, uFogMax);
    float totalFog = isSky ? uFogSkyFloor : geoFog;

    // --- Atmospheric blue shift: Rayleigh scattering tints distant geometry blue ---
    // Applied before fog blend so it shows through even in moderate fog.
    float atmoT = clamp((viewDist - uAtmoStart) / (uFarP * 0.1), 0.0, 1.0) * (1.0 - totalFog);
    vec3 atmoTint = vec3(0.55, 0.72, 1.0) * uAtmoBlueshift;
    col = mix(col, col + atmoTint, atmoT * (1.0 - uNightFactor));

    col = mix(col, fogColTM, totalFog);

        float vig=1.-dist2*uGradeVignette; vig=clamp(vig,0.,1.); vig=vig*vig*(3.-2.*vig);
    col*=mix(0.55,1.,vig);

        col+=( hash(uv+fract(uTime*0.017))*uGrainStrength*2.0-uGrainStrength )*0.5;

        float lum=dot(col,vec3(0.2126,0.7152,0.0722));
    col+=uGradeShadowTint   *clamp(1.-lum*2.8,0.,1.);
    col+=uGradeHighlightTint*clamp(lum*1.8-0.6,0.,1.);
    col+=vec3(0.004,-0.002,0.006)*clamp(1.-clamp(1.-lum*2.8,0.,1.)-clamp(lum*1.8-0.6,0.,1.),0.,1.);
    col=mix(col,col*col*(3.-2.*col),uGradeContrast);
    float lumF=dot(col,vec3(0.2126,0.7152,0.0722));
    col=mix(vec3(lumF),col,uGradeSaturation);

    fragColor=vec4(clamp(col,0.,1.),1.);
}
)GLSL";
static const char* PS1_FS = R"GLSL(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uScene;
uniform float uTime;
uniform float uColorDepth;   // levels per channel (255 for 8-bit)
uniform float uDitherStr;
uniform float uWobbleStr;
uniform float uScanlineStr;
uniform int   uInterlace;
uniform vec2  uResolution;

// 4x4 Bayer ordered dither matrix (normalised to [-0.5, 0.5])
float bayer4[16] = float[16](
     0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
    12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
     3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
    15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
);

float hash(vec2 p){ return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5); }

void main(){
    vec2 uv = vUV;

        // PS2 GTE rounded vertex coords to fixed-point integers per triangle,
    // causing UV coordinates to swim as the camera moves.
    // We simulate this by snapping UV to a coarse grid and then applying
    // a small time-varying offset proportional to sub-pixel distance.
    float snapRes = 256.0;  // PS2 GTE was ~12-bit sub-pixel (1/4096 precision)
    vec2 snapUV   = floor(uv * snapRes + 0.5) / snapRes;
    vec2 subPixel = uv - snapUV;  // sub-pixel error
    float wobbleAmt = uWobbleStr * 0.0008;
    uv.x += sin(snapUV.y * 47.3 + uTime * 0.7) * wobbleAmt + subPixel.x * 0.3;
    uv.y += sin(snapUV.x * 39.1 + uTime * 0.6) * wobbleAmt + subPixel.y * 0.3;
    uv = clamp(uv, 0.0, 1.0);

    vec3 col = texture(uScene, uv).rgb;

        // PS2 had characteristic near-black crushing from limited DAC precision
    vec3 crushCol = vec3(0.005, 0.005, 0.012);
    col = mix(crushCol, col, smoothstep(0.0, 0.18, dot(col, vec3(0.299, 0.587, 0.114))));

        // PS2 used dithering to hide 8-bit quantisation artifacts.
    // More subtle than PS1 (which had 5-bit + heavy dither).
    ivec2 px = ivec2(gl_FragCoord.xy) % 4;
    float dither = bayer4[px.y * 4 + px.x] - 0.5;
    float ditherOffset = (dither * uDitherStr) / uColorDepth;
    col += ditherOffset;

        col = floor(col * uColorDepth + 0.5) / uColorDepth;
    col = clamp(col, 0.0, 1.0);

        // PS2 output was interlaced (480i) on CRTs. Simulate with alternating
    // field brightening: even/odd lines alternate between fields each frame.
    if(uInterlace == 1){
        float field    = mod(floor(uTime * 60.0), 2.0);  // alternates at 60fps
        float lineEven = mod(floor(gl_FragCoord.y), 2.0);
        // On even fields, darken even lines; on odd fields, darken odd lines
        if(lineEven == field){
            col *= (1.0 - uScanlineStr);
        }
    } else {
        // Non-interlace: static scanline darkening (every other row)
        float scanline = mod(floor(gl_FragCoord.y), 2.0) < 1.0 ? 1.0 : (1.0 - uScanlineStr);
        col *= scanline;
    }

    fragColor = vec4(col, 1.0);
}
)GLSL";

static const char* GHOST_VS = R"GLSL(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNorm;
out vec3 vWorld;
void main(){
    vec4 w = uModel * vec4(aPos,1.0);
    vWorld  = w.xyz;
    vNorm   = mat3(uModel) * aNorm;
    gl_Position = uMVP * vec4(aPos,1.0);
}
)GLSL";

static const char* GHOST_FS = R"GLSL(
#version 330 core
in  vec3 vNorm;
in  vec3 vWorld;
out vec4 fragColor;
uniform vec3 uColor;
uniform vec3 uSunDir;
uniform float uNightFactor;
void main(){
    vec3 N = normalize(vNorm);
    float NdL = max(dot(N, normalize(uSunDir)), 0.0);
    float ambient = mix(0.30, 0.10, uNightFactor);
    float lit = ambient + NdL * mix(0.70, 0.15, uNightFactor);
    fragColor = vec4(uColor * lit, 0.88);
}
)GLSL";

// ghost player globals
static GLuint gGhostProg = 0;
static GLuint gGhostVAO  = 0;
static GLuint gGhostVBO  = 0;
static GLuint gGhostEBO  = 0;
static GLsizei gGhostIdxCount = 0;

static void buildGhostMesh() {
    const int SLICES = 12, RINGS = 6;
    const float R = 0.35f;  // radius
    const float HALF_H = 0.7f;  // half cylinder height (total body = 1.4u)

    struct GV { float x,y,z, nx,ny,nz; };
    std::vector<GV>       verts;
    std::vector<uint32_t> idx;

    auto pushSphere = [&](float theta, float phi, float yOff) {
        float st = sinf(theta), ct = cosf(theta);
        float sp = sinf(phi),   cp = cosf(phi);
        float nx = st*cp, ny = sp, nz = ct*cp;
        verts.push_back({nx*R, ny*R + yOff, nz*R, nx, ny, nz});
    };

    for (int j = 0; j <= RINGS/2; j++) {
        float phi = (float)j / (RINGS/2) * PI * 0.5f;
        for (int i = 0; i <= SLICES; i++) {
            float theta = (float)i / SLICES * 2.f * PI;
            pushSphere(theta, phi, HALF_H);
        }
    }
    int topOff = 0;
    int topRows = RINGS/2 + 1;

    for (int j = 0; j <= RINGS/2; j++) {
        float phi = -(float)j / (RINGS/2) * PI * 0.5f;
        for (int i = 0; i <= SLICES; i++) {
            float theta = (float)i / SLICES * 2.f * PI;
            pushSphere(theta, phi, -HALF_H);
        }
    }
    int botOff = topRows * (SLICES+1);

    int cylOff = botOff + topRows * (SLICES+1);
    for (int j = 0; j <= 1; j++) {
        float yy = (j == 0) ? -HALF_H : HALF_H;
        for (int i = 0; i <= SLICES; i++) {
            float theta = (float)i / SLICES * 2.f * PI;
            float nx = sinf(theta), nz = cosf(theta);
            verts.push_back({nx*R, yy, nz*R, nx, 0, nz});
        }
    }

    auto quad = [&](int row0, int row1, int off, int s) {
        for (int i = 0; i < s; i++) {
            uint32_t a = off + row0*(s+1)+i, b = a+1;
            uint32_t c = off + row1*(s+1)+i, d = c+1;
            idx.insert(idx.end(), {a,b,c, b,d,c});
        }
    };

    for (int j = 0; j < RINGS/2; j++) quad(j, j+1, topOff, SLICES);
    for (int j = 0; j < RINGS/2; j++) quad(j, j+1, botOff, SLICES);
    quad(0, 1, cylOff, SLICES);

    glGenVertexArrays(1, &gGhostVAO);
    glGenBuffers(1, &gGhostVBO);
    glGenBuffers(1, &gGhostEBO);
    glBindVertexArray(gGhostVAO);
    glBindBuffer(GL_ARRAY_BUFFER, gGhostVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(GV), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gGhostEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(uint32_t), idx.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(GV),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(GV),(void*)(3*sizeof(float)));
    glBindVertexArray(0);
    gGhostIdxCount = (GLsizei)idx.size();
}

static Vec3 peerColor(int id) {
    float hues[4][3] = {
        {0.28f, 0.55f, 1.00f},   // blue
        {1.00f, 0.55f, 0.10f},   // orange
        {0.25f, 0.90f, 0.35f},   // green
        {0.90f, 0.25f, 0.85f},   // magenta
    };
    int i = id % 4;
    return {hues[i][0], hues[i][1], hues[i][2]};
}

// 8×8 pixel font covering printable ASCII 32-127 (public domain VGA font).
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 space
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 33 !
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // 34 "
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // 35 #
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // 36 $
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // 37 %
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // 38 &
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // 39 '
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // 40 (
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // 41 )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42 *
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // 43 +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // 44 ,
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // 45 -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // 46 .
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // 47 /
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 48 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 49 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 50 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 51 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 52 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 53 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 54 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 55 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 56 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 57 9
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // 58 :
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // 59 ;
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // 60 <
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // 61 =
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 62 >
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // 63 ?
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // 64 @
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 65 A
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 66 B
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 67 C
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 68 D
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 69 E
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 70 F
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 71 G
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 72 H
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 73 I
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 74 J
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 75 K
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 76 L
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 77 M
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 78 N
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 79 O
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 80 P
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 81 Q
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 82 R
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 83 S
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 84 T
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 85 U
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 86 V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 87 W
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 88 X
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 89 Y
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 90 Z
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // 91 [
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // 92 backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // 93 ]
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // 94 ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 95 _
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // 96 `
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 97 a
    {0x07,0x06,0x3E,0x66,0x66,0x66,0x3B,0x00}, // 98 b
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 99 c
    {0x38,0x30,0x3E,0x33,0x33,0x33,0x6E,0x00}, // 100 d
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // 101 e
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // 102 f
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 103 g
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 104 h
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 105 i
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 106 j
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 107 k
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 108 l
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 109 m
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 110 n
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 111 o
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 112 p
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 113 q
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 114 r
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 115 s
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 116 t
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 117 u
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 118 v
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 119 w
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 120 x
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 121 y
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 122 z
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // 123 {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // 124 |
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // 125 }
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // 126 ~
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, // 127 DEL
};

// builds a texture for a nametag
static GLuint makeNameTex(const char* name, const float* color3)
{
    int len = 0;
    while (name[len] && len < 31) len++;
    if (len == 0) { name = "?"; len = 1; }

    const int PAD = 3;
    const int GW = 8, GH = 8, GAP = 1;
    int tw = PAD + len * (GW + GAP) - GAP + PAD;
    int th = GH + PAD * 2;

    std::vector<uint8_t> px(tw * th * 4, 0);

    // Dark background
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            // Rounded corners via distance to corner squares
            bool corner = (x < 2 && y < 2) || (x < 2 && y >= th-2) ||
                          (x >= tw-2 && y < 2) || (x >= tw-2 && y >= th-2);
            if (!corner) {
                uint8_t* p = px.data() + (y * tw + x) * 4;
                p[0] = 20; p[1] = 20; p[2] = 20; p[3] = 180;
            }
        }

    // Glyph pixels
    for (int ci = 0; ci < len; ci++) {
        unsigned char c = (unsigned char)name[ci];
        if (c < 32 || c > 127) c = '?';
        const uint8_t* glyph = font8x8[c - 32];
        int ox = PAD + ci * (GW + GAP);
        int oy = PAD;
        for (int row = 0; row < GH; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < GW; col++) {
                if (bits & (0x80 >> col)) {
                    uint8_t* p = px.data() + ((oy + row) * tw + (ox + col)) * 4;
                    p[0] = (uint8_t)(color3[0] * 255);
                    p[1] = (uint8_t)(color3[1] * 255);
                    p[2] = (uint8_t)(color3[2] * 255);
                    p[3] = 255;
                }
            }
        }
    }

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static const char* NAMETAG_VS = R"GLSL(
#version 330 core
uniform mat4 uVP;
uniform vec3 uCenter;    // world-space anchor (above capsule head)
uniform vec2 uSize;      // world-space half-width, half-height of quad
uniform vec3 uCamRight;  // camera right vector (for billboarding)
uniform vec3 uCamUp;     // camera up vector
out vec2 vUV;
void main(){
    // Emit 6 vertices for a quad (two triangles) from gl_VertexID
    vec2 corners[6] = vec2[](
        vec2(-1,-1), vec2( 1,-1), vec2( 1, 1),
        vec2(-1,-1), vec2( 1, 1), vec2(-1, 1)
    );
    vec2 uvs[6] = vec2[](
        vec2(1,1), vec2(0,1), vec2(0,0),
        vec2(1,1), vec2(0,0), vec2(1,0)
    );
    vec2 c = corners[gl_VertexID];
    vec3 wpos = uCenter
        + uCamRight * c.x * uSize.x
        + uCamUp    * c.y * uSize.y;
    vUV = uvs[gl_VertexID];
    gl_Position = uVP * vec4(wpos, 1.0);
}
)GLSL";

static const char* NAMETAG_FS = R"GLSL(
#version 330 core
in  vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
void main(){
    fragColor = texture(uTex, vUV);
    if (fragColor.a < 0.05) discard;
}
)GLSL";

static GLuint gNametagProg  = 0;
static GLuint gNametagVAO   = 0;   // empty VAO — geometry is procedural in VS

static GLuint compileShader(GLenum t, const char* s) {
    GLuint sh = glCreateShader(t); glShaderSource(sh, 1, &s, nullptr); glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[2048]; glGetShaderInfoLog(sh, sizeof(l), nullptr, l); SDL_Log("Shader:\n%s", l); }
    return sh;
}
static GLuint makeProgram(const char* vs, const char* fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs), f = compileShader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram(); glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char l[2048]; glGetProgramInfoLog(p, sizeof(l), nullptr, l); SDL_Log("Link:\n%s", l); }
    glDeleteShader(v); glDeleteShader(f); return p;
}
static GLint U(GLuint p, const char* n) { return glGetUniformLocation(p, n); }
static void u3f(GLuint p, const char* n, Vec3 v) { glUniform3f(U(p, n), v.x, v.y, v.z); }
static void u1f(GLuint p, const char* n, float v) { glUniform1f(U(p, n), v); }
static void uM4(GLuint p, const char* n, const Mat4& m) { glUniformMatrix4fv(U(p, n), 1, GL_FALSE, m.m); }

// texture loader
static GLuint loadTexture(const char* path) {
    int w, h, ch;
    stbi_set_flip_vertically_on_load(1);
    unsigned char* data = stbi_load(path, &w, &h, &ch, 3);
    GLuint tex; glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (data) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        SDL_Log("Loaded texture: %s (%dx%d)", path, w, h);
        stbi_image_free(data);
    }
    else {
        // 1x1 fallback — no mipmaps needed
        uint8_t white[3] = { 200, 190, 180 };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        SDL_Log("Texture not found: %s — using fallback", path);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

// main
int main(int argc, char** argv) {
        bool        netIsHost = false;
    std::string netJoinIp = "";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0) netIsHost = true;
        if (strcmp(argv[i], "--join") == 0 && i + 1 < argc) netJoinIp = argv[++i];
    }
    bool netEnabled = netIsHost || !netJoinIp.empty();

        NetCtx gNet;
    if (netEnabled) {
        if (!netInit(gNet, netIsHost, netJoinIp.empty() ? nullptr : netJoinIp.c_str())) {
            fprintf(stderr, "[net] Failed to initialise networking — running offline.\n");
            netEnabled = false;
        }
    }

        {
        FILE* f = fopen("username.txt", "r");
        if (f) {
            char buf[64] = {};
            if (fgets(buf, sizeof(buf), f)) {
                // Strip newline
                int n = (int)strlen(buf);
                while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n] = 0;
                if (n > 0 && n <= 31) strncpy(gNet.myUsername, buf, 31);
            }
            fclose(f);
            fprintf(stdout, "[net] Username: %s\n", gNet.myUsername);
        } else {
            // Create a default file so the user knows it exists
            FILE* fw = fopen("username.txt", "w");
            if (fw) { fprintf(fw, "Player\n"); fclose(fw); }
            fprintf(stdout, "[net] username.txt not found — created with default 'Player'.\n");
        }
    }

    initPerlin();
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("SDL: %s", SDL_GetError()); return 1; }

    // Audio variables declared here so they're in scope for the whole main().
    // Actual init happens after the GL context is created (WASAPI needs the
    // message pump running, which SDL starts during window creation).
    ma_engine_config maEngineConfig;
    ma_engine        maEngine;
    ma_sound sndAmbient, sndJump, sndLand;
    bool audioOK = false;
    bool hasAmbient = false, hasJump = false, hasLand = false;

    // Footstep pool — drop in up to 8 files named footstep1.wav … footstep8.wav.
    // Any files that exist are loaded; missing ones are silently skipped.
    // Each step picks one at random so the sound never feels repetitive.
    // (Legacy footstep.wav is also tried as footstep1 if footstep1.wav is missing.)
    constexpr int MAX_FOOTSTEPS = 8;
    ma_sound sndFootsteps[MAX_FOOTSTEPS];
    int      numFootsteps = 0;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_Window* win = SDL_CreateWindow("engine", WINDOW_W, WINDOW_H, SDL_WINDOW_OPENGL);
    if (!win) { SDL_Log("Window: %s", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { SDL_Log("GL ctx: %s", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(1);
    glewExperimental = GL_TRUE; glewInit();
    SDL_Log("GL: %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));

        gGhostProg = makeProgram(GHOST_VS, GHOST_FS);
    buildGhostMesh();
    gNametagProg = makeProgram(NAMETAG_VS, NAMETAG_FS);
    glGenVertexArrays(1, &gNametagVAO); // empty — VS generates geometry

        maEngineConfig = ma_engine_config_init();
    maEngineConfig.channels = 2;
    maEngineConfig.sampleRate = 44100;
    ma_result maResult = ma_engine_init(&maEngineConfig, &maEngine);
    audioOK = (maResult == MA_SUCCESS);
    SDL_Log("miniaudio engine: %s (code %d)", audioOK ? "OK" : "FAILED", (int)maResult);
    if (audioOK) {
        // Log the working directory so we know where miniaudio is looking for files
        char cwdBuf[512] = {};
        if (!getcwd(cwdBuf, sizeof(cwdBuf))) cwdBuf[0] = '?';
        SDL_Log("Working directory: %s", cwdBuf);

        // Ambient: spatialized so it pans with head movement for a surround feel.
        // Load OGG (Vorbis) - spatialization enabled (no NO_SPATIALIZATION flag).
        ma_result ambientResult = ma_sound_init_from_file(&maEngine, "ambient.ogg",
            MA_SOUND_FLAG_STREAM,
            nullptr, nullptr, &sndAmbient);
        hasAmbient = (ambientResult == MA_SUCCESS);
        if (hasAmbient)
            SDL_Log("ambient.ogg loaded OK (spatialized)");
        else {
            SDL_Log("ambient.ogg failed (code %d), trying ambient.wav...", (int)ambientResult);
            ambientResult = ma_sound_init_from_file(&maEngine, "ambient.wav",
                MA_SOUND_FLAG_STREAM,
                nullptr, nullptr, &sndAmbient);
            hasAmbient = (ambientResult == MA_SUCCESS);
            SDL_Log("ambient.wav: %s (code %d)", hasAmbient ? "OK" : "FAIL", (int)ambientResult);
        }
        if (hasAmbient) {
            // Place ambient source far away so attenuation is minimal — we mainly
            // want the panning/HRTF effect, not distance muting.
            // Keep volume constant — we want panning from head movement, not
            // distance attenuation.  min_distance larger than orbit radius (600m)
            // means the source always sits inside the full-volume zone.
            ma_sound_set_min_distance(&sndAmbient, 800.f);   // full vol up to 800 m
            ma_sound_set_max_distance(&sndAmbient, 2000.f);
            ma_sound_set_rolloff(&sndAmbient, 0.0f);         // zero rolloff = constant volume
            // 0.0 = no stereo separation at all, 1.0 = full HRTF hard pan.
            // 0.15 = very gentle width — you feel the movement without it ever
            // swinging into one ear. Tweak upward if you want more presence.
            ma_sound_set_directional_attenuation_factor(&sndAmbient, 0.15f);
        }

        // Footstep pool: try footstep1.wav … footstep8.wav, then fall back to
        // the legacy footstep.wav name so existing projects still work.
        for (int i = 0; i < MAX_FOOTSTEPS; i++) {
            char fname[32];
            snprintf(fname, sizeof(fname), "footstep%d.wav", i + 1);
            // On the first slot, also try the legacy name if the numbered one fails
            ma_result r = ma_sound_init_from_file(&maEngine, fname,
                MA_SOUND_FLAG_NO_SPATIALIZATION,
                nullptr, nullptr, &sndFootsteps[numFootsteps]);
            if (r != MA_SUCCESS && i == 0) {
                r = ma_sound_init_from_file(&maEngine, "footstep.wav",
                    MA_SOUND_FLAG_NO_SPATIALIZATION,
                    nullptr, nullptr, &sndFootsteps[numFootsteps]);
            }
            if (r == MA_SUCCESS) {
                ma_sound_set_volume(&sndFootsteps[numFootsteps], SFX_VOLUME / 128.f);
                SDL_Log("Footstep[%d] loaded: %s", numFootsteps, fname);
                numFootsteps++;
            }
        }
        SDL_Log("Footstep sounds loaded: %d", numFootsteps);
        hasJump = (ma_sound_init_from_file(&maEngine, "jump.wav",
            MA_SOUND_FLAG_NO_SPATIALIZATION,
            nullptr, nullptr, &sndJump) == MA_SUCCESS);
        hasLand = (ma_sound_init_from_file(&maEngine, "land.wav",
            MA_SOUND_FLAG_NO_SPATIALIZATION,
            nullptr, nullptr, &sndLand) == MA_SUCCESS);
        SDL_Log("Audio files: ambient=%s footstep(s)=%d jump=%s land=%s",
            hasAmbient ? "OK" : "FAIL", numFootsteps,
            hasJump ? "OK" : "FAIL", hasLand ? "OK" : "FAIL");
        if (hasAmbient) {
            ma_sound_set_volume(&sndAmbient, AMBIENT_VOLUME / 128.f);
            ma_sound_set_looping(&sndAmbient, MA_TRUE);
            ma_result startResult = ma_sound_start(&sndAmbient);
            if (startResult != MA_SUCCESS)
                SDL_Log("ma_sound_start(ambient) FAILED: code %d — check file is valid Vorbis OGG (re-encode with: ffmpeg -i ambient.ogg -c:a libvorbis fixed.ogg)", (int)startResult);
            else
                SDL_Log("Ambient music started OK (volume=%.2f, looping=true)", AMBIENT_VOLUME / 128.f);
        }
        else {
            SDL_Log("ambient.ogg not loaded — ensure the file is in the same directory as engine.exe and is encoded as Vorbis OGG (not Opus).");
        }
        if (hasJump)     ma_sound_set_volume(&sndJump, SFX_VOLUME / 128.f);
        if (hasLand)     ma_sound_set_volume(&sndLand, SFX_VOLUME / 128.f);
    }

        // Place PNG/JPG files next to the executable.
    // If missing a grey fallback is used — the engine still runs fine.
    GLuint texWall = loadTexture("tex_concrete.png");
    GLuint texRoad = loadTexture("tex_road.png");
    GLuint texSidewalk = loadTexture("tex_sidewalk.png");
    GLuint normWall = loadTexture("norm_concrete.png");
    GLuint normRoad = loadTexture("norm_road.png");
    GLuint normSidewalk = loadTexture("norm_sidewalk.png");

    glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    glViewport(0, 0, SCREEN_W, SCREEN_H);

    GLuint cityProg = makeProgram(CITY_VS, CITY_FS);
    GLuint skyProg = makeProgram(SKY_VS, SKY_FS);
    GLuint threshProg = makeProgram(QUAD_VS, BLOOM_THRESH_FS);
    GLuint blurProg = makeProgram(QUAD_VS, BLOOM_BLUR_FS);
    GLuint compProg = makeProgram(QUAD_VS, COMPOSITE_FS);
    GLuint shadowProg = makeProgram(SHADOW_VS, SHADOW_FS);
    GLuint normalProg = makeProgram(CITY_VS, NORMAL_FS);
    GLuint ssaoProg = makeProgram(QUAD_VS, SSAO_FS);
    GLuint ssaoBlurProg = makeProgram(QUAD_VS, SSAO_BLUR_FS);
    GLuint dofProg = makeProgram(QUAD_VS, DOF_FS);  // uHorizontal selects axis
    GLuint ps1Prog = makeProgram(QUAD_VS, PS1_FS);
    GLuint skyVAO; glGenVertexArrays(1, &skyVAO);

        GLuint hdrFBO, hdrTex, hdrDepthTex;
    glGenFramebuffers(1, &hdrFBO);
    glGenTextures(1, &hdrTex);
    glBindTexture(GL_TEXTURE_2D, hdrTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, SCREEN_W, SCREEN_H, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Depth texture (sampled by CITY_FS for SSR)
    glGenTextures(1, &hdrDepthTex);
    glBindTexture(GL_TEXTURE_2D, hdrDepthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SCREEN_W, SCREEN_H, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, hdrDepthTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

        GLuint shadowFBO, shadowTex;
    glGenFramebuffers(1, &shadowFBO);
    glGenTextures(1, &shadowTex);
    glBindTexture(GL_TEXTURE_2D, shadowTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    { float bc[] = { 1,1,1,1 }; glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, bc); }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex, 0);
    glDrawBuffer(GL_NONE); glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Shares hdrDepthTex so we only need a colour attachment
    GLuint normalFBO, normalTex;
    glGenFramebuffers(1, &normalFBO);
    glGenTextures(1, &normalTex);
    glBindTexture(GL_TEXTURE_2D, normalTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, SCREEN_W, SCREEN_H, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, normalFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, normalTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, hdrDepthTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

        const int AW = SCREEN_W / 4, AH = SCREEN_H / 4;
    GLuint ssaoFBO, ssaoTex, ssaoBlurFBO, ssaoBlurTex;
    glGenFramebuffers(1, &ssaoFBO);    glGenTextures(1, &ssaoTex);
    glGenFramebuffers(1, &ssaoBlurFBO); glGenTextures(1, &ssaoBlurTex);
    for (int i = 0; i < 2; i++) {
        GLuint fbo = i ? ssaoBlurFBO : ssaoFBO;
        GLuint tex = i ? ssaoBlurTex : ssaoTex;
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, AW, AH, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

        GLuint dofFBO[2], dofTex[2];
    glGenFramebuffers(2, dofFBO); glGenTextures(2, dofTex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, dofTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, SCREEN_W, SCREEN_H, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, dofFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dofTex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

        const int BW = SCREEN_W / 2, BH = SCREEN_H / 2;
    const int QW = SCREEN_W / 4, QH = SCREEN_H / 4;
    GLuint bloomFBO[2], bloomTex[2];
    glGenFramebuffers(2, bloomFBO); glGenTextures(2, bloomTex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, bloomTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, BW, BH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomTex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

        GLuint wideBloomFBO[2], wideBloomTex[2];
    glGenFramebuffers(2, wideBloomFBO); glGenTextures(2, wideBloomTex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, wideBloomTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, QW, QH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, wideBloomFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, wideBloomTex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GLuint quadVAO; glGenVertexArrays(1, &quadVAO);

        GLuint ps1FBO, ps1Tex;
    glGenFramebuffers(1, &ps1FBO); glGenTextures(1, &ps1Tex);
    glBindTexture(GL_TEXTURE_2D, ps1Tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SCREEN_W, SCREEN_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);  // nearest = chunky PS1 pixels
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, ps1FBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ps1Tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    std::unordered_map<CK, Chunk, CKH> chunks;
    chunks.reserve(256);
    g_chunks = &chunks;

        constexpr float DAY_DUR = DAY_MINUTES * 60.f;
    constexpr float NIGHT_DUR = NIGHT_MINUTES * 60.f;
    constexpr float CYCLE_DUR = DAY_DUR + NIGHT_DUR;
    float cycleTime = DAY_DUR * 0.35f;  // start at afternoon, not noon

    // These are computed each frame from cycleTime:
    Vec3 sunDir = { 0,1,0 };
    Vec3 sunCol = { 1,1,1 }, skyAmb = { 0,0,0 };
    Vec3 fogCol = { 0,0,0 }, horizon = { 0,0,0 };
    Vec3 midSky = { 0,0,0 }, zenith = { 0,0,0 }, below = hexLin(0x100D0A);
    float sunIntensity = 1.f, ambIntensity = 0.f, nightFactor = 0.f;

    Camera cam = { {4.f,EYE_H,4.f},0.f,0.f };
    Vec3 vel = { 0,0,0 };
    bool onGround = true;
    bool mW = 0, mA = 0, mS = 0, mD = 0, crouch = 0, captured = true;
    bool jumpHeld = false;
    bool jumpQueued = false;
    float jumpBufferT = 0.f;
    float coyoteT = 0.f;
    float bhopGraceT = 0.f;  // suppresses friction for 0.8s after a missed bhop landing

    float dashCooldown = 0.f;
    bool  dashing = false;
    float dashTimer = 0.f;
    Vec3  dashDir = { 0,0,0 };

    bool  sliding = false;
    float slideTimer = 0.f;
    Vec3  slideDir = { 0,0,0 };

    float fovShake = 0.f;       // current smoothed FOV bonus (spring output)
    float fovShakeTarget = 0.f; // target the spring pulls toward
    float fovShakeVel = 0.f;    // spring velocity
    float fovSmooth = FOV_DEG;

        float stepTimer = 0.f;  // counts down; fires footstep when <= 0 while moving

        float camRollSmooth = 0.f;    // smoothed strafe lean applied to view
    float landSquish = 0.f;       // spring squish on landing (0=none, 1=full)
    float landSquishVel = 0.f;    // spring velocity for squish bounce
    float shakeX = 0.f, shakeY = 0.f; // landing screen shake offsets
    float shakeMag = 0.f;         // current shake magnitude decays over time
    float prevVelY = 0.f;         // track vertical velocity for landing impact

    SDL_SetWindowRelativeMouseMode(win, true);

    uint64_t prev = SDL_GetTicks(), fpsT = prev; int fpsN = 0;
    bool running = true;

    while (running) {
        uint64_t now = SDL_GetTicks();
        float dt = std::min((float)(now - prev) / 1000.f, .05f);
        prev = now; fpsN++;
        if (now - fpsT >= 1000) {
            char buf[256];
            const char* netMode = !netEnabled ? "" : (netIsHost ? " [HOST]" : " [CLIENT]");
            snprintf(buf, sizeof(buf), "engine %d FPS | chunks:%d | pos(%.0f, %.0f)%s",
                fpsN, (int)chunks.size(), cam.pos.x, cam.pos.z, netMode);
            SDL_SetWindowTitle(win, buf); fpsN = 0; fpsT = now;
        }

        float mdx = 0, mdy = 0;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            else if (ev.type == SDL_EVENT_KEY_DOWN) switch (ev.key.scancode) {
            case SDL_SCANCODE_ESCAPE: captured = !captured; SDL_SetWindowRelativeMouseMode(win, captured); break;
            case SDL_SCANCODE_W: mW = 1; break; case SDL_SCANCODE_S: mS = 1; break;
            case SDL_SCANCODE_A: mA = 1; break; case SDL_SCANCODE_D: mD = 1; break;
            case SDL_SCANCODE_LCTRL:
            case SDL_SCANCODE_LSHIFT: crouch = 1; break;
            case SDL_SCANCODE_SPACE:
                jumpQueued = true; jumpBufferT = 0.14f;
                break;
            default: break;
            }
            else if (ev.type == SDL_EVENT_KEY_UP) switch (ev.key.scancode) {
            case SDL_SCANCODE_W: mW = 0; break; case SDL_SCANCODE_S: mS = 0; break;
            case SDL_SCANCODE_A: mA = 0; break; case SDL_SCANCODE_D: mD = 0; break;
            case SDL_SCANCODE_LCTRL:
            case SDL_SCANCODE_LSHIFT: crouch = 0; break;
            case SDL_SCANCODE_SPACE: jumpHeld = false; break;
            default: break;
            }
            else if (ev.type == SDL_EVENT_MOUSE_MOTION && captured) {
                mdx += ev.motion.xrel; mdy += ev.motion.yrel;
            }
            else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (ev.button.button == SDL_BUTTON_RIGHT && captured) {
                    if (dashCooldown <= 0.f) {
                        Vec3 f = camFwd(cam), r = camRight(cam);
                        Vec3 wd = { 0,0,0 };
                        if (mW)wd = v3add(wd, f); if (mS)wd = v3add(wd, v3mul(f, -1));
                        if (mD)wd = v3add(wd, r); if (mA)wd = v3add(wd, v3mul(r, -1));
                        float wl = sqrtf(wd.x * wd.x + wd.z * wd.z);
                        if (wl < 0.01f) { wd = f; wl = 1.f; }
                        if (!onGround) {
                            Vec3 ld = norm3({ cosf(cam.pitch) * sinf(cam.yaw), sinf(cam.pitch), -cosf(cam.pitch) * cosf(cam.yaw) });
                            vel.x = ld.x * DASH_SPEED; vel.y = ld.y * DASH_SPEED; vel.z = ld.z * DASH_SPEED;
                        }
                        else {
                            dashDir = { wd.x / wl, 0, wd.z / wl };
                            vel.x = dashDir.x * DASH_SPEED; vel.z = dashDir.z * DASH_SPEED; vel.y = 3.f;
                        }
                        dashing = true; dashTimer = 0.12f;
                        dashCooldown = DASH_CD;
                        sliding = false; fovShakeTarget = 5.f;
                    }
                }
                else if (ev.button.button == SDL_BUTTON_LEFT && !captured) {
                    captured = 1; SDL_SetWindowRelativeMouseMode(win, 1);
                }
            }
        }

        // Look
        cam.yaw += mdx * MOUSE_SENS * DEG2RAD;
        cam.pitch -= mdy * MOUSE_SENS * DEG2RAD;
        cam.pitch = clampf(cam.pitch, -89.f * DEG2RAD, 89.f * DEG2RAD);

        // Tick timers
        dashCooldown = std::max(0.f, dashCooldown - dt);
        if (dashTimer > 0.f) dashTimer -= dt; else dashing = false;
        if (jumpBufferT > 0.f) jumpBufferT -= dt; else jumpQueued = false;
        if (coyoteT > 0.f) coyoteT -= dt;
        if (bhopGraceT > 0.f) bhopGraceT -= dt;
        if (slideTimer > 0.f) { slideTimer -= dt; if (slideTimer <= 0.f) sliding = false; }
        fovShakeTarget = std::max(0.f, fovShakeTarget - dt * 6.f); // target decays slowly
        // Damped spring: snappy rise, smooth fall — stiffness 140, damping 14
        fovShakeVel += (fovShakeTarget - fovShake) * 140.f * dt;
        fovShakeVel *= 1.f - 14.f * dt;
        fovShake += fovShakeVel * dt;
        fovShake = std::max(0.f, fovShake);

                // Full 3D look direction (used for dash + wall hop)
        Vec3 lookDir = norm3({
            cosf(cam.pitch) * sinf(cam.yaw),
            sinf(cam.pitch),
           -cosf(cam.pitch) * cosf(cam.yaw)
            });
        Vec3 fwd = camFwd(cam), rgt = camRight(cam);

        // Wish direction — horizontal only for normal movement
        Vec3 wishDir = { 0,0,0 };
        float wl = 0.f;
        if (!sliding) {
            if (mW) wishDir = v3add(wishDir, fwd);
            if (mS) wishDir = v3add(wishDir, v3mul(fwd, -1.f));
            if (mD) wishDir = v3add(wishDir, rgt);
            if (mA) wishDir = v3add(wishDir, v3mul(rgt, -1.f));
            wl = sqrtf(wishDir.x * wishDir.x + wishDir.z * wishDir.z);
            if (wl > 0.01f) { wishDir.x /= wl; wishDir.z /= wl; }
        }
        float wishSpeed = MOVE_SPEED;

        if (onGround && !dashing) {
            if (sliding) {
                float steer = 0.15f;
                if (mA) vel.x += v3mul(rgt, -1.f).x * wishSpeed * steer * dt;
                if (mD) vel.x += rgt.x * wishSpeed * steer * dt;
                vel.x *= 1.f - FRICTION * 0.4f * dt;
                vel.z *= 1.f - FRICTION * 0.4f * dt;
            }
            else {
                // Quake-style ground accel: friction first, then accelerate
                // During bhop grace window, skip friction so a missed hop doesn't kill the chain
                float spd = sqrtf(vel.x * vel.x + vel.z * vel.z);
                if (spd > 0.01f && bhopGraceT <= 0.f) {
                    float drop = spd * FRICTION * dt;
                    float newSpd = std::max(0.f, spd - drop);
                    vel.x *= newSpd / spd; vel.z *= newSpd / spd;
                }
                float curSpd = vel.x * wishDir.x + vel.z * wishDir.z;
                float addSpd = wishSpeed - curSpd;
                if (addSpd > 0.f) {
                    float accelSpd = GROUND_ACCEL * dt;   // NOT multiplied by wishSpeed
                    if (accelSpd > addSpd) accelSpd = addSpd;
                    vel.x += accelSpd * wishDir.x;
                    vel.z += accelSpd * wishDir.z;
                }
                // Hard cap — prevents any speed creep
                float spd2 = sqrtf(vel.x * vel.x + vel.z * vel.z);
                if (spd2 > MAX_SPEED) {
                    vel.x = vel.x / spd2 * MAX_SPEED;
                    vel.z = vel.z / spd2 * MAX_SPEED;
                }
            }
        }
        else if (!dashing) {
            float wishLen = sqrtf(wishDir.x * wishDir.x + wishDir.z * wishDir.z);
            if (wishLen > 0.01f) {
                // Quake-style air accel: only add speed if not already moving fast
                // in the wish direction. W+A / W+D strafing still works because the
                // projection onto a perpendicular wish direction stays low.
                // Hard ceiling: air accel can never push total speed above MAX_SPEED
                // unless you were already above it when you left the ground (bhop chain).
                // This closes the exploit where holding W + spam-space slowly climbs
                // toward BHOP_CAP one hop at a time with no strafing required.
                float curSpd = vel.x * wishDir.x + vel.z * wishDir.z;
                float totalSpd = sqrtf(vel.x * vel.x + vel.z * vel.z);
                float airCap = std::max(totalSpd, MAX_SPEED); // preserve existing speed, don't add to it straight
                float addSpd = AIR_ACCEL * dt;
                if (curSpd + addSpd > airCap) addSpd = std::max(0.f, airCap - curSpd);
                vel.x += wishDir.x * addSpd;
                vel.z += wishDir.z * addSpd;
            }
        }

                // physY tracks the true floor-relative position independently of camera
        // cam.pos.y is ONLY used for rendering — never fed back into physics

        // Gravity
        if (!onGround) vel.y += GRAVITY * dt;

        // Integrate XZ + physics Y separately from camera Y
        Vec3 oldPos = cam.pos;
        cam.pos.x += vel.x * dt;
        cam.pos.z += vel.z * dt;

        static float physY = EYE_H;   // true player foot+eye height above ground
        physY += vel.y * dt;

        // Ground collision — floor is always at EYE_H, crouch never affects this
        bool justLanded = false;
        if (physY <= EYE_H) {
            physY = EYE_H;
            if (vel.y < 0.f) {
                // Landing impact — scale with how fast we were falling
                if (!onGround) {
                    justLanded = true; coyoteT = 0.f;
                    float impact = clampf(-prevVelY / 28.f, 0.f, 1.f); // 0=gentle, 1=max fall
                    if (impact > 0.05f) {
                        landSquish = impact * LAND_SQUISH_STR;
                        landSquishVel = 0.f;
                        shakeMag = impact * 0.018f;  // shake radius in UV space
                        // Landing sound — louder for harder impacts
                        if (hasLand) {
                            ma_sound_set_volume(&sndLand, (SFX_VOLUME / 128.f) * clampf(impact * 1.4f, 0.2f, 1.f));
                            ma_sound_seek_to_pcm_frame(&sndLand, 0);
                            ma_sound_start(&sndLand);
                        }
                    }
                }
                vel.y = 0.f;
            }
            onGround = true;
        }
        else {
            if (onGround) coyoteT = 0.12f;
            onGround = false;
        }
        prevVelY = vel.y;

                // If space is buffered and we just landed OR we're on ground, jump immediately.
        // No friction penalty if you jump the same frame you land.
        bool canJump = onGround || coyoteT > 0.f;
        if (jumpQueued && canJump) {
            // Bhop speed chain: only apply the boost multiplier when already
            // above MAX_SPEED — that speed was earned through air strafing and
            // should be preserved. Below MAX_SPEED, just jump at current speed
            // with no bonus, so spamming space on flat ground doesn't stack speed.
            float hspd = sqrtf(vel.x * vel.x + vel.z * vel.z);
            if (hspd > MAX_SPEED) {
                float boost = crouch ? BHOP_BOOST * 1.05f : BHOP_BOOST;
                vel.x *= boost; vel.z *= boost;
                float nh = sqrtf(vel.x * vel.x + vel.z * vel.z);
                if (nh > BHOP_CAP) { vel.x = vel.x / nh * BHOP_CAP; vel.z = vel.z / nh * BHOP_CAP; }
            }
            vel.y = JUMP_IMP;
            physY = EYE_H + 0.01f;
            onGround = false; coyoteT = 0.f;
            jumpQueued = false; jumpBufferT = 0.f;
            sliding = false;
            if (hasJump) { ma_sound_seek_to_pcm_frame(&sndJump, 0); ma_sound_start(&sndJump); }
        }
        else if (justLanded) {
            // Landed without jumping: set bhop grace window to suppress friction
            // briefly, giving the player a small window to still chain the bhop
            bhopGraceT = 0.08f;
        }

                // Guard: don't start a slide if we're in a bhop chain (speed > MAX_SPEED).
        // At bhop speeds the slide cap (SLIDE_SPEED) would kill momentum, which
        // is exactly the "shift ruins bhop" problem. Let the player hold shift
        // mid-air for the crouch-boost on the next jump without punishing them.
        if (crouch && onGround && !sliding) {
            float hspd = sqrtf(vel.x * vel.x + vel.z * vel.z);
            if (hspd > MOVE_SPEED * 0.7f && hspd <= MAX_SPEED * 1.1f) {
                sliding = true; slideTimer = SLIDE_DUR;
                float n = hspd > 0.01f ? 1.f / hspd : 0.f;
                slideDir = { vel.x * n, 0, vel.z * n };
                float slideBoost = std::min(hspd * 1.15f, SLIDE_SPEED);
                vel.x = slideDir.x * slideBoost;
                vel.z = slideDir.z * slideBoost;
            }
        }
        if (!crouch && sliding) sliding = false;

                static float smoothCrouch = 0.f; // 0=standing, 1=fully crouched
        float crouchTarget = (crouch || sliding) ? 1.f : 0.f;
        smoothCrouch = lerpf(smoothCrouch, crouchTarget, clampf(dt * 14.f, 0.f, 1.f));
        float crouchDrop = smoothCrouch * (EYE_H - EYE_CROUCH); // max drop in units
        cam.pos.y = physY - crouchDrop; // camera Y = physics Y minus visual crouch

                {
            Vec3 physPos = { cam.pos.x, physY, cam.pos.z };
            Vec3 physOld = { oldPos.x, physY, oldPos.z };
            Vec3 resolved = resolveXZ(physOld, physPos, 0.35f);
            bool blockedX = fabsf(resolved.x - physPos.x) > 0.001f;
            bool blockedZ = fabsf(resolved.z - physPos.z) > 0.001f;

            if ((blockedX || blockedZ) && !onGround && jumpBufferT > 0.f) {
                float curSpd = sqrtf(vel.x * vel.x + vel.z * vel.z);
                float launchSpd = std::min(curSpd * 1.1f, MAX_SPEED * 1.3f);
                vel.x = lookDir.x * launchSpd;
                vel.y = lookDir.y * launchSpd * 0.5f + WALLHOP_UP * 0.6f;
                vel.z = lookDir.z * launchSpd;
                jumpQueued = false; jumpBufferT = 0.f;
                fovShakeTarget = 3.f; sliding = false;
            }
            else {
                if (blockedX) vel.x = 0.f;
                if (blockedZ) vel.z = 0.f;
            }
            cam.pos.x = resolved.x;
            cam.pos.z = resolved.z;
        }

        // Speed-based FOV — scales up to BHOP_CAP
        float hspd = sqrtf(vel.x * vel.x + vel.z * vel.z);
        float fovTarget = FOV_DEG + clampf(hspd * FOV_SPEED_SCALE, 0.f, FOV_SPEED_MAX);
        fovSmooth = lerpf(fovSmooth, fovTarget, clampf(dt * FOV_LERP_RATE, 0.f, 1.f));
        float dynFov = fovSmooth + fovShake;

                // Project velocity onto right vector to get lateral speed
        Vec3 rgtNow = camRight(cam);
        float lateralSpd = vel.x * rgtNow.x + vel.z * rgtNow.z;
        float rollTarget = clampf(lateralSpd / BHOP_CAP, -1.f, 1.f) * STRAFE_LEAN_MAX;
        camRollSmooth = lerpf(camRollSmooth, rollTarget, clampf(dt * STRAFE_LEAN_RATE, 0.f, 1.f));

                // Simple damped spring: squish decays and bounces back
        landSquishVel += (-LAND_SQUISH_STIFF * landSquish - LAND_SQUISH_DAMP * landSquishVel) * dt;
        landSquish += landSquishVel * dt;
        landSquish = clampf(landSquish, -0.15f, 1.f); // allow small upward bounce
        shakeX = shakeY = 0.f;

                {
            float hspd = sqrtf(vel.x * vel.x + vel.z * vel.z);
            if (onGround && hspd > FOOTSTEP_MIN_SPEED) {
                stepTimer -= dt;
                if (stepTimer <= 0.f) {
                    stepTimer = FOOTSTEP_INTERVAL * clampf(MOVE_SPEED / hspd, 0.5f, 1.2f);
                    if (numFootsteps > 0) {
                        // Pick a random footstep from the pool each step
                        static uint32_t rngState = 0x12345678u;
                        rngState = rngState * 1664525u + 1013904223u;
                        int idx = (int)((rngState >> 16) % (uint32_t)numFootsteps);
                        ma_sound_seek_to_pcm_frame(&sndFootsteps[idx], 0);
                        ma_sound_start(&sndFootsteps[idx]);
                    }
                }
            }
            else {
                // Reset timer so first step fires immediately when you start moving
                stepTimer = 0.f;
            }
        }

                if (netEnabled) {
            double nowSec = (double)now / 1000.0;
            // Receive all incoming packets (and relay if host)
            netPoll(gNet, nowSec);
            // Build flags byte from current player state
            uint8_t netFlags = 0;
            if (crouch)   netFlags |= 1;
            if (sliding)  netFlags |= 2;
            if (!onGround)netFlags |= 4;
            // Send our position at NET_SEND_HZ (rate-limited inside netSendState)
            netSendState(gNet,
                cam.pos.x, cam.pos.y, cam.pos.z,
                cam.yaw, cam.pitch,
                netFlags, nowSec);
            // Broadcast our username every ~3 s so late joiners learn it quickly
            netSendName(gNet, nowSec);
        }

        // Chunk streaming — load ONE new chunk per frame max to avoid stutter
        int pcx = (int)floorf(cam.pos.x / CHUNK_SIZE);
        int pcz = (int)floorf(cam.pos.z / CHUNK_SIZE);

        int chunksLoaded = 0;
        for (int dz = -VIEW_CHUNKS; dz <= VIEW_CHUNKS && chunksLoaded < 4; dz++)
            for (int dx = -VIEW_CHUNKS; dx <= VIEW_CHUNKS && chunksLoaded < 4; dx++) {
                CK k{ pcx + dx,pcz + dz };
                if (chunks.find(k) == chunks.end()) {
                    Chunk ch; ch.cx = k.cx; ch.cz = k.cz; ch.vao = ch.vbo = 0; ch.count = 0;
                    buildChunk(ch);
                    chunks[k] = std::move(ch);
                    ++chunksLoaded;
                }
            }

        // Evict distant chunks
        for (auto it = chunks.begin(); it != chunks.end();) {
            int dx = it->first.cx - pcx, dz = it->first.cz - pcz;
            if (std::abs(dx) > VIEW_CHUNKS + 2 || std::abs(dz) > VIEW_CHUNKS + 2) {
                freeChunk(it->second); it = chunks.erase(it);
            }
            else ++it;
        }

                cycleTime += dt;
        if (cycleTime >= CYCLE_DUR) cycleTime -= CYCLE_DUR;

        // sunAngle: 0=noon, PI=midnight
        // Day phase  [0, DAY_DUR)   -> angle 0..PI  (sun crosses sky)
        // Night phase [DAY_DUR, CYCLE_DUR) -> angle PI..2PI (below horizon)
        float sunAngle;
        if (cycleTime < DAY_DUR)
            sunAngle = (cycleTime / DAY_DUR) * PI;
        else
            sunAngle = PI + ((cycleTime - DAY_DUR) / NIGHT_DUR) * PI;

        float sunElev = cosf(sunAngle);          // +1=noon, -1=midnight
        float sunHorizS = sinf(sunAngle);          // horizontal component
        sunDir = norm3({ sunHorizS * 0.6f, sunElev, sunHorizS * 0.4f });

        // nightFactor: 0=full day, 1=full night
        float nf = clampf(1.f - (sunElev + 0.15f) / 0.30f, 0.f, 1.f);
        nightFactor = nf * nf * (3.f - 2.f * nf);

        // Helper lambdas
        auto ssc = [](float a, float b, float x) -> float {
            float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t);
            };
        auto lc3 = [](Vec3 a, Vec3 b, float t) -> Vec3 {
            return { lerpf(a.x,b.x,t), lerpf(a.y,b.y,t), lerpf(a.z,b.z,t) };
            };

        // Colour palettes
        Vec3 zenithDay = hexLin(SKY_ZENITH_DAY), zenithDusk = hexLin(SKY_ZENITH_DUSK), zenithNight = hexLin(SKY_ZENITH_NIGHT);
        Vec3 midSkyDay = hexLin(SKY_MIDSKY_DAY), midSkyDusk = hexLin(SKY_MIDSKY_DUSK), midSkyNight = hexLin(SKY_MIDSKY_NIGHT);
        Vec3 horizDay = hexLin(SKY_HORIZON_DAY), horizDusk = hexLin(SKY_HORIZON_DUSK), horizNight = hexLin(SKY_HORIZON_NIGHT);
        Vec3 fogDay = hexLin(SKY_FOG_DAY), fogDusk = hexLin(SKY_FOG_DUSK), fogNight = hexLin(SKY_FOG_NIGHT);
        Vec3 skyAmbDay = hexLin(SKY_AMB_DAY), skyAmbDusk = hexLin(SKY_AMB_DUSK), skyAmbNight = hexLin(SKY_AMB_NIGHT);
        Vec3 sunColDay = hexLin(SUN_COL_DAY), sunColDusk = hexLin(SUN_COL_DUSK), sunColNight = hexLin(SUN_COL_NIGHT);

        float duskT = ssc(0.35f, -0.10f, sunElev);
        float nightT = ssc(0.05f, -0.25f, sunElev);

        zenith = lc3(lc3(zenithDay, zenithDusk, duskT), zenithNight, nightT);
        midSky = lc3(lc3(midSkyDay, midSkyDusk, duskT), midSkyNight, nightT);
        horizon = lc3(lc3(horizDay, horizDusk, duskT), horizNight, nightT);
        fogCol = lc3(lc3(fogDay, fogDusk, duskT), fogNight, nightT);
        skyAmb = lc3(lc3(skyAmbDay, skyAmbDusk, duskT), skyAmbNight, nightT);
        sunCol = lc3(lc3(sunColDay, sunColDusk, duskT), sunColNight, nightT);

        sunIntensity = clampf(sunElev * 1.4f, 0.f, 1.1f) + nightFactor * 0.06f;
        ambIntensity = lerpf(AMBIENT_DAY, AMBIENT_NIGHT, nightFactor);
        float fogDensity = lerpf(0.00028f, 0.00055f, nightFactor);  // pre-tonemap in-shader fog (kept for city FS compatibility)

        Mat4 proj = perspMat(dynFov, (float)SCREEN_W / SCREEN_H, NEAR_P, FAR_P);
        // Apply roll to view: rotate the up vector around the forward axis
        Mat4 view = viewMat(cam.pos, cam.yaw, cam.pitch);
        // Apply roll to view: rotate right and up rows around forward axis
        // Column-major: row r of all 4 columns = m[0*4+r], m[1*4+r], m[2*4+r], m[3*4+r]
        {
            float cr = cosf(camRollSmooth), sr = sinf(camRollSmooth);
            // right = row 0, up = row 1 of view matrix (column-major)
            for (int c = 0; c < 4; c++) {
                float r = view.m[c * 4 + 0], u = view.m[c * 4 + 1];
                view.m[c * 4 + 0] = r * cr + u * sr;
                view.m[c * 4 + 1] = -r * sr + u * cr;
            }
        }
        Mat4 vp = mat4Mul(proj, view);

        Vec3 camF = norm3({ cosf(cam.pitch) * sinf(cam.yaw), sinf(cam.pitch), -cosf(cam.pitch) * cosf(cam.yaw) });
        Vec3 camR = norm3(cross3(camF, { 0,1,0 }));
        Vec3 camU = cross3(camR, camF);

        Vec3 cityGlow = {
            lerpf(0.f, CITY_GLOW_NIGHT[0], nightFactor),
            lerpf(0.f, CITY_GLOW_NIGHT[1], nightFactor),
            lerpf(0.f, CITY_GLOW_NIGHT[2], nightFactor)
        };

        // =========================================================
        // PASS 1: Shadow map — render depth from sun POV
        // =========================================================
        // Orthographic sun camera covering SHADOW_DISTANCE around player
        float sd2 = SHADOW_DISTANCE;
        // Sun view: position camera high up along sun direction above player
        Vec3 sunEye = v3add(cam.pos, v3mul(sunDir, sd2));
        // Build sun view matrix (look from sunEye toward player, up = world up with fallback)
        Vec3 sunFwd = norm3({ -sunDir.x, -sunDir.y, -sunDir.z });
        Vec3 upVec = (fabsf(sunDir.y) > 0.99f) ? Vec3{ 1,0,0 } : Vec3{ 0,1,0 };
        Vec3 sunRgt = norm3(cross3(sunFwd, upVec));
        Vec3 sunUp = cross3(sunRgt, sunFwd);
        // Manual orthographic + look-at into lightVP
        // View rows
        Mat4 lightView = {};
        lightView.m[0] = sunRgt.x; lightView.m[4] = sunRgt.y; lightView.m[8] = sunRgt.z;  lightView.m[12] = -dot3(sunRgt, sunEye);
        lightView.m[1] = sunUp.x;  lightView.m[5] = sunUp.y;  lightView.m[9] = sunUp.z;   lightView.m[13] = -dot3(sunUp, sunEye);
        lightView.m[2] = -sunFwd.x; lightView.m[6] = -sunFwd.y; lightView.m[10] = -sunFwd.z; lightView.m[14] = dot3(sunFwd, sunEye);
        lightView.m[15] = 1.f;
        // Ortho projection
        float hn = sd2, vn = sd2, zn = -sd2 * 2.f, zf = sd2 * 2.f;
        Mat4 lightProj = {};
        lightProj.m[0] = 1.f / hn; lightProj.m[5] = 1.f / vn;
        lightProj.m[10] = -2.f / (zf - zn); lightProj.m[14] = -(zf + zn) / (zf - zn);
        lightProj.m[15] = 1.f;
        Mat4 lightVP = mat4Mul(lightProj, lightView);

        glViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFBO);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE); glCullFace(GL_FRONT); // peter-panning prevention
        glUseProgram(shadowProg);
        uM4(shadowProg, "uLightVP", lightVP);
        for (auto& [key, ch] : chunks)
            if (ch.count > 0) { glBindVertexArray(ch.vao); glDrawArrays(GL_TRIANGLES, 0, ch.count); }
        glCullFace(GL_BACK);
        glEnable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // =========================================================
        // PASS 2: Normal buffer pass (view-space normals for SSAO)
        // =========================================================
        glViewport(0, 0, SCREEN_W, SCREEN_H);
        glBindFramebuffer(GL_FRAMEBUFFER, normalFBO);
        glClearColor(0.5f, 0.5f, 1.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(normalProg);
        uM4(normalProg, "uVP", vp);
        uM4(normalProg, "uView", view);
        glActiveTexture(GL_TEXTURE0);
        for (auto& [key, ch] : chunks)
            if (ch.count > 0) { glBindVertexArray(ch.vao); glDrawArrays(GL_TRIANGLES, 0, ch.count); }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // =========================================================
        // PASS 3: Main HDR scene render
        // =========================================================
        glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);
        glViewport(0, 0, SCREEN_W, SCREEN_H);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glDepthMask(GL_FALSE);
        glUseProgram(skyProg);
        u3f(skyProg, "uHorizon", horizon); u3f(skyProg, "uMidSky", midSky); u3f(skyProg, "uZenith", zenith);
        Vec3 belowCol = { horizon.x * 0.12f, horizon.y * 0.10f, horizon.z * 0.10f };
        u3f(skyProg, "uBelow", belowCol);
        u3f(skyProg, "uSunDir", sunDir);
        u3f(skyProg, "uCamFwd", camF); u3f(skyProg, "uCamRgt", camR); u3f(skyProg, "uCamUp", camU);
        u1f(skyProg, "uTanHFov", tanf(dynFov * DEG2RAD * .5f));
        u1f(skyProg, "uAspect", (float)SCREEN_W / SCREEN_H);
        u1f(skyProg, "uTime", (float)(now / 1000.0));
        u1f(skyProg, "uNightFactor", nightFactor);
        glBindVertexArray(skyVAO); glDrawArrays(GL_TRIANGLES, 0, 3);
        glDepthMask(GL_TRUE);

        glUseProgram(cityProg);
        uM4(cityProg, "uVP", vp); uM4(cityProg, "uView", view); uM4(cityProg, "uProj", proj);
        u3f(cityProg, "uCamPos", cam.pos);
        u3f(cityProg, "uSunDir", sunDir);
        u3f(cityProg, "uSunCol", sunCol);   u1f(cityProg, "uSunI", sunIntensity);
        u3f(cityProg, "uSkyAmb", skyAmb);   u1f(cityProg, "uSkyAmbI", ambIntensity);
        u3f(cityProg, "uFogCol", fogCol);   u1f(cityProg, "uFogDensity", fogDensity);
        u1f(cityProg, "uSunWrap", SUN_WRAP);
        u1f(cityProg, "uTime", (float)(now / 1000.0));
        u1f(cityProg, "uNightFactor", nightFactor);
        u3f(cityProg, "uCityGlow", cityGlow);
        glUniform2f(glGetUniformLocation(cityProg, "uResolution"), (float)SCREEN_W, (float)SCREEN_H);
        // Shadow map
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, shadowTex);
        glUniform1i(glGetUniformLocation(cityProg, "uShadowMap"), 2);
        uM4(cityProg, "uLightVP", lightVP);
        u1f(cityProg, "uShadowBias", SHADOW_BIAS);
        u1f(cityProg, "uShadowSoftness", SHADOW_SOFTNESS);
        // Wet road
        u1f(cityProg, "uRoadWetAmount", ROAD_WET_AMOUNT);
        u1f(cityProg, "uRoadWetRoughness", ROAD_WET_ROUGHNESS);
        // SSR textures
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, hdrTex);
        glUniform1i(glGetUniformLocation(cityProg, "uSceneTex"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, hdrDepthTex);
        glUniform1i(glGetUniformLocation(cityProg, "uDepthTex"), 1);
        // Surface textures on slots 5, 6, 7
        glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, texWall);
        glUniform1i(glGetUniformLocation(cityProg, "uWallTex"), 5);
        glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, texRoad);
        glUniform1i(glGetUniformLocation(cityProg, "uRoadTex"), 6);
        glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, texSidewalk);
        glUniform1i(glGetUniformLocation(cityProg, "uSidewalkTex"), 7);
        u1f(cityProg, "uWallScale", WALL_TEX_SCALE);
        u1f(cityProg, "uRoadScale", ROAD_TEX_SCALE);
        u1f(cityProg, "uSidewalkScale", SIDEWALK_TEX_SCALE);
        u1f(cityProg, "uTexBlend", TEX_BLEND);
        glActiveTexture(GL_TEXTURE0);

        for (auto& [key, ch] : chunks)
            if (ch.count > 0) { glBindVertexArray(ch.vao); glDrawArrays(GL_TRIANGLES, 0, ch.count); }

                if (netEnabled && gGhostProg && gGhostVAO) {
            double nowSec = (double)now / 1000.0;
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(gGhostProg);
            glBindVertexArray(gGhostVAO);
            for (const auto& peer : gNet.peers) {
                if (!peer.active) continue;

                    float px, py, pz;
                netGetPeerPos(peer, nowSec, px, py, pz);

                    // rotate by peer yaw around Y axis.
                float cy = cosf(peer.yaw), sy = sinf(peer.yaw);
                    float footY = py - EYE_H;
                    Mat4 model = {};
                model.m[0]  =  cy; model.m[4]  = 0; model.m[8]  =  sy; model.m[12] = px;
                model.m[1]  =  0;  model.m[5]  = 1; model.m[9]  =  0;  model.m[13] = footY + EYE_H * 0.5f;
                model.m[2]  = -sy; model.m[6]  = 0; model.m[10] =  cy; model.m[14] = pz;
                model.m[15] = 1.f;

                Mat4 mvp = mat4Mul(vp, model);
                uM4(gGhostProg, "uMVP",   mvp);
                uM4(gGhostProg, "uModel", model);
                Vec3 col = peerColor(peer.id);
                u3f(gGhostProg, "uColor",      col);
                u3f(gGhostProg, "uSunDir",     sunDir);
                u1f(gGhostProg, "uNightFactor",nightFactor);
                glDrawElements(GL_TRIANGLES, gGhostIdxCount, GL_UNSIGNED_INT, 0);
            }
            glDisable(GL_BLEND);

                        if (gNametagProg && gNametagVAO) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                glUseProgram(gNametagProg);
                glBindVertexArray(gNametagVAO);
                        Vec3 billRight = camR;
                Vec3 billUp    = camU;
                glUniform3f(glGetUniformLocation(gNametagProg,"uCamRight"), billRight.x, billRight.y, billRight.z);
                glUniform3f(glGetUniformLocation(gNametagProg,"uCamUp"),    billUp.x,    billUp.y,    billUp.z);
                uM4(gNametagProg, "uVP", vp);

                for (auto& peer : gNet.peers) {
                    if (!peer.active) continue;

                            if (peer.nameTex == 0) {
                        if (peer.nameTex != 0) glDeleteTextures(1, &peer.nameTex);
                        float col[3];
                        Vec3 c = peerColor(peer.id);
                        col[0]=c.x; col[1]=c.y; col[2]=c.z;
                        peer.nameTex = makeNameTex(peer.username, col);
                    }

                    float px2, py2, pz2;
                    netGetPeerPos(peer, nowSec, px2, py2, pz2);

                            float headY = py2 - EYE_H + EYE_H * 0.5f + 1.1f + 0.6f; // top of capsule + gap
                    Vec3 anchor = { px2, headY, pz2 };

                            int nameLen = (int)strlen(peer.username);
                    if (nameLen == 0) nameLen = 1;
                    float hw = nameLen * 0.09f + 0.15f;
                    float hh = 0.22f;

                    glUniform3f(glGetUniformLocation(gNametagProg,"uCenter"), anchor.x, anchor.y, anchor.z);
                    glUniform2f(glGetUniformLocation(gNametagProg,"uSize"),   hw, hh);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, peer.nameTex);
                    glUniform1i(glGetUniformLocation(gNametagProg,"uTex"), 0);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                }
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_DEPTH_TEST);

        // =========================================================
        // PASS 4: SSAO
        // =========================================================
        // Build inverse projection matrix for SSAO view-space reconstruction
        // Column-major layout: m[col*4+row]
        // invProj: column-major layout m[col*4 + row]
        Mat4 invProj = {};
        invProj.m[0] = 1.f / proj.m[0];           // col0,row0 = asp/f
        invProj.m[5] = 1.f / proj.m[5];           // col1,row1 = 1/f
        invProj.m[11] = -1.f;                       // col2,row3 = -1 (forward)
        invProj.m[14] = 1.f / proj.m[14];           // col3,row2
        invProj.m[15] = proj.m[10] / proj.m[14];    // col3,row3

        glViewport(0, 0, AW, AH);
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoFBO);
        glUseProgram(ssaoProg);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, hdrDepthTex);
        glUniform1i(glGetUniformLocation(ssaoProg, "uDepth"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, normalTex);
        glUniform1i(glGetUniformLocation(ssaoProg, "uNormal"), 1);
        uM4(ssaoProg, "uProj", proj);
        uM4(ssaoProg, "uInvProj", invProj);
        u1f(ssaoProg, "uNearP", NEAR_P); u1f(ssaoProg, "uFarP", FAR_P);
        u1f(ssaoProg, "uRadius", SSAO_RADIUS);
        u1f(ssaoProg, "uBias", SSAO_BIAS);
        u1f(ssaoProg, "uStrength", SSAO_STRENGTH);
        glUniform1i(glGetUniformLocation(ssaoProg, "uSamples"), SSAO_SAMPLES);
        u1f(ssaoProg, "uTime", (float)(now / 1000.0));
        glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);

        // Blur SSAO
        glBindFramebuffer(GL_FRAMEBUFFER, ssaoBlurFBO);
        glUseProgram(ssaoBlurProg);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, ssaoTex);
        glUniform1i(glGetUniformLocation(ssaoBlurProg, "uSSAO"), 0);
        glUniform2f(glGetUniformLocation(ssaoBlurProg, "uTexelSize"), 1.f / AW, 1.f / AH);
        glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // =========================================================
        // PASS 5: Bloom
        // =========================================================
        // Night: lower bloom threshold so window glow halos more
        float bloomThreshNight = lerpf(BLOOM_THRESHOLD, BLOOM_THRESHOLD * 0.38f, nightFactor);

        glViewport(0, 0, BW, BH);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO[0]);
        glUseProgram(threshProg);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, hdrTex);
        glUniform1i(glGetUniformLocation(threshProg, "uTex"), 0);
        u1f(threshProg, "uThreshold", bloomThreshNight);
        glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);

        for (int pass = 0; pass < 3; pass++) {
            int src = pass % 2, dst = 1 - src;
            glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO[dst]);
            glUseProgram(blurProg);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, bloomTex[src]);
            glUniform1i(glGetUniformLocation(blurProg, "uTex"), 0);
            glUniform2f(glGetUniformLocation(blurProg, "uTexelSize"), 1.f / BW, 1.f / BH);
            u1f(blurProg, "uOffset", (float)pass);
            glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        glViewport(0, 0, QW, QH);
        glBindFramebuffer(GL_FRAMEBUFFER, wideBloomFBO[0]);
        glUseProgram(threshProg);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, hdrTex);
        glUniform1i(glGetUniformLocation(threshProg, "uTex"), 0);
        u1f(threshProg, "uThreshold", bloomThreshNight * 0.76f);
        glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);
        for (int pass = 0; pass < 4; pass++) {
            int src = pass % 2, dst = 1 - src;
            glBindFramebuffer(GL_FRAMEBUFFER, wideBloomFBO[dst]);
            glUseProgram(blurProg);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, wideBloomTex[src]);
            glUniform1i(glGetUniformLocation(blurProg, "uTex"), 0);
            glUniform2f(glGetUniformLocation(blurProg, "uTexelSize"), 1.f / QW, 1.f / QH);
            u1f(blurProg, "uOffset", (float)pass);
            glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        // =========================================================
        // PASS 6: Depth of Field (two-pass separable)
        // =========================================================
        glViewport(0, 0, SCREEN_W, SCREEN_H);
        // Horizontal pass: hdrTex -> dofTex[0]
        glBindFramebuffer(GL_FRAMEBUFFER, dofFBO[0]);
        glUseProgram(dofProg);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, hdrTex);
        glUniform1i(glGetUniformLocation(dofProg, "uScene"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, hdrDepthTex);
        glUniform1i(glGetUniformLocation(dofProg, "uDepth"), 1);
        u1f(dofProg, "uNearP", NEAR_P); u1f(dofProg, "uFarP", FAR_P);
        u1f(dofProg, "uFocusDist", DOF_FOCUS_DIST);
        u1f(dofProg, "uFocusRange", DOF_FOCUS_RANGE);
        u1f(dofProg, "uBlurStrength", DOF_BLUR_STRENGTH);
        glUniform2f(glGetUniformLocation(dofProg, "uTexelSize"), 1.f / SCREEN_W, 1.f / SCREEN_H);
        glUniform1i(glGetUniformLocation(dofProg, "uHorizontal"), 1);
        glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);

        // Vertical pass: dofTex[0] -> dofTex[1]
        glBindFramebuffer(GL_FRAMEBUFFER, dofFBO[1]);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, dofTex[0]);
        glUniform1i(glGetUniformLocation(dofProg, "uScene"), 0);
        glUniform1i(glGetUniformLocation(dofProg, "uHorizontal"), 0);
        glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // =========================================================
        // PASS 7: Composite to screen
        // =========================================================
        Vec3 sunPt = v3add(cam.pos, v3mul(sunDir, 5000.f));
        float sx4[4];
        for (int r = 0; r < 4; r++) sx4[r] = vp.m[r] * sunPt.x + vp.m[4 + r] * sunPt.y + vp.m[8 + r] * sunPt.z + vp.m[12 + r];
        float sunNDCx = sx4[3] > 0.f ? sx4[0] / sx4[3] : 0.f;
        float sunNDCy = sx4[3] > 0.f ? sx4[1] / sx4[3] : 0.f;

        glViewport(0, 0, SCREEN_W, SCREEN_H);
        glBindFramebuffer(GL_FRAMEBUFFER, ps1FBO);   // render composite into PS1 FBO (640x480)
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(compProg);
        // Scene texture: use DoF output (dofTex[1])
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, dofTex[1]);
        glUniform1i(glGetUniformLocation(compProg, "uScene"), 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, bloomTex[0]);
        glUniform1i(glGetUniformLocation(compProg, "uBloom"), 1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, wideBloomTex[0]);
        glUniform1i(glGetUniformLocation(compProg, "uBloomWide"), 2);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, hdrDepthTex);
        glUniform1i(glGetUniformLocation(compProg, "uDepth"), 3);
        glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, ssaoBlurTex);
        glUniform1i(glGetUniformLocation(compProg, "uSSAO"), 4);
        u1f(compProg, "uBloomStr", BLOOM_STRENGTH);
        u1f(compProg, "uBloomTightMix", BLOOM_TIGHT_MIX);
        u1f(compProg, "uBloomWideMix", BLOOM_WIDE_MIX);
        u1f(compProg, "uTime", (float)(now / 1000.0));
        u1f(compProg, "uNightFactor", nightFactor);
        u1f(compProg, "uNearP", NEAR_P);
        u1f(compProg, "uFarP", FAR_P);
        u3f(compProg, "uSunDir", sunDir);
        u3f(compProg, "uFogCol", fogCol);
        u3f(compProg, "uCamFwd", camF);
        glUniform2f(glGetUniformLocation(compProg, "uSunScreen"), sunNDCx, sunNDCy);
        u1f(compProg, "uFogNearDensity", FOG_NEAR_DENSITY);
        u1f(compProg, "uFogDistDensity", lerpf(FOG_DIST_DENSITY, FOG_DIST_DENSITY_NIGHT, nightFactor));
        u1f(compProg, "uFogHeightDensity", FOG_HEIGHT_DENSITY);
        u1f(compProg, "uFogSkyFloor", FOG_SKY_FLOOR);
        u1f(compProg, "uFogMax", FOG_MAX);
        glUniform3f(glGetUniformLocation(compProg, "uFogColorDay"), FOG_COLOR_DAY[0], FOG_COLOR_DAY[1], FOG_COLOR_DAY[2]);
        glUniform3f(glGetUniformLocation(compProg, "uFogColorNight"), FOG_COLOR_NIGHT[0], FOG_COLOR_NIGHT[1], FOG_COLOR_NIGHT[2]);
        u1f(compProg, "uTonemapExposure", TONEMAP_EXPOSURE);
        u1f(compProg, "uTonemapGamma", TONEMAP_GAMMA);
        u1f(compProg, "uBarrelDistortion", BARREL_DISTORTION + landSquish * 0.04f);
        u1f(compProg, "uChromAberBase", CHROM_ABER_BASE + landSquish * 0.003f);
        u1f(compProg, "uChromAberEdge", CHROM_ABER_EDGE + landSquish * 0.008f);
        u1f(compProg, "uFlareStrength", (float)FLARE_ON * FLARE_STRENGTH);
        glUniform3f(glGetUniformLocation(compProg, "uGradeShadowTint"), GRADE_SHADOW_TINT[0], GRADE_SHADOW_TINT[1], GRADE_SHADOW_TINT[2]);
        glUniform3f(glGetUniformLocation(compProg, "uGradeHighlightTint"), GRADE_HIGHLIGHT_TINT[0], GRADE_HIGHLIGHT_TINT[1], GRADE_HIGHLIGHT_TINT[2]);
        u1f(compProg, "uGradeContrast", GRADE_CONTRAST);
        u1f(compProg, "uGradeSaturation", GRADE_SATURATION);
        u1f(compProg, "uGradeVignette", GRADE_VIGNETTE + landSquish * 0.8f);
        u1f(compProg, "uGrainStrength", GRAIN_STRENGTH + landSquish * 0.04f);
        u1f(compProg, "uAtmoBlueshift", ATMO_BLUESHIFT);
        u1f(compProg, "uAtmoStart", ATMO_BLUESHIFT_START);
        glUniform2f(glGetUniformLocation(compProg, "uSquishOffset"), 0.f, -landSquish * 0.018f);
        glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);

        // =========================================================
        // PASS 8: PS1 filter — quantise + dither + scanlines,
        // =========================================================
        glViewport(0, 0, WINDOW_W, WINDOW_H);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(ps1Prog);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, ps1Tex);
        glUniform1i(glGetUniformLocation(ps1Prog, "uScene"), 0);
        u1f(ps1Prog, "uTime", (float)(now / 1000.0));
        u1f(ps1Prog, "uColorDepth", (float)((1 << PS1_COLOR_DEPTH) - 1));
        u1f(ps1Prog, "uDitherStr", PS1_DITHER_STR);
        u1f(ps1Prog, "uWobbleStr", PS1_WOBBLE_STR);
        u1f(ps1Prog, "uScanlineStr", PS1_SCANLINE_STR);
        glUniform1i(glGetUniformLocation(ps1Prog, "uInterlace"), PS2_INTERLACE);
        glUniform2f(glGetUniformLocation(ps1Prog, "uResolution"), (float)SCREEN_W, (float)SCREEN_H);
        glBindVertexArray(quadVAO); glDrawArrays(GL_TRIANGLES, 0, 3);

        // --- 3D audio listener update (every frame) ---
        // Update listener position AND orientation every frame.
        // miniaudio's spatialization engine uses these to compute left/right
        // panning and distance attenuation automatically — we must NOT fight it
        // by re-anchoring the source to cam.yaw (that cancels the effect).
        if (audioOK) {
            // Listener position = camera world position
            ma_engine_listener_set_position(&maEngine, 0,
                cam.pos.x, cam.pos.y, cam.pos.z);

            // Listener forward = full 3D look direction (yaw + pitch).
            // This is what makes sounds pan left/right AND up/down with head movement.
            float fwdX = cosf(cam.pitch) * sinf(cam.yaw);
            float fwdY = sinf(cam.pitch);
            float fwdZ = -cosf(cam.pitch) * cosf(cam.yaw);
            ma_engine_listener_set_direction(&maEngine, 0, fwdX, fwdY, fwdZ);

            // Listener up = world up.
            // Keeping this fixed means roll doesn't affect panning (intended).
            ma_engine_listener_set_world_up(&maEngine, 0, 0.f, 1.f, 0.f);

            // Place ambient source at a FIXED world-space position that slowly
            // drifts on a large circle in world space (NOT relative to cam.yaw).
            // The listener direction update above is what produces all the panning
            // — the source just needs to be somewhere in the world around the player.
            // A slow world-space orbit (period ~60s) keeps it from feeling pinned
            // to one ear while still letting head-turns shift the stereo image.
            if (hasAmbient) {
                // Large radius + slow orbit = very gentle, smooth panning.
                // The further away the source, the smaller the left/right angle
                // as you turn — 600m radius keeps it well within a ~15 degree
                // arc from centre so it never feels like a hard left/right ping.
                // Period ~4 minutes so it drifts imperceptibly rather than spinning.
                float ambTime = (float)(now / 1000.0);
                float ambR = 600.f;                  // far away = subtle angle
                float ambAngle = ambTime * 0.026f;       // ~240 s full orbit
                float ambX = cam.pos.x + ambR * cosf(ambAngle);
                float ambZ = cam.pos.z + ambR * sinf(ambAngle);
                ma_sound_set_position(&sndAmbient, ambX, cam.pos.y, ambZ);
            }
        }

        glEnable(GL_DEPTH_TEST);
        SDL_GL_SwapWindow(win);
    }

    for (auto& [k, ch] : chunks) freeChunk(ch);
        if (netEnabled) {
        // Free nametag textures before killing GL context
        for (auto& p : gNet.peers)
            if (p.nameTex) { glDeleteTextures(1, &p.nameTex); p.nameTex = 0; }
        netShutdown(gNet);
    }
        if (gGhostVAO)   { glDeleteVertexArrays(1, &gGhostVAO); glDeleteBuffers(1, &gGhostVBO); glDeleteBuffers(1, &gGhostEBO); }
    if (gGhostProg)  glDeleteProgram(gGhostProg);
    if (gNametagVAO) glDeleteVertexArrays(1, &gNametagVAO);
    if (gNametagProg)glDeleteProgram(gNametagProg);
    glDeleteTextures(1, &texWall);
    glDeleteTextures(1, &texRoad);
    glDeleteTextures(1, &texSidewalk);
    glDeleteFramebuffers(1, &hdrFBO);   glDeleteTextures(1, &hdrTex);   glDeleteTextures(1, &hdrDepthTex);
    glDeleteFramebuffers(1, &shadowFBO); glDeleteTextures(1, &shadowTex);
    glDeleteFramebuffers(1, &normalFBO); glDeleteTextures(1, &normalTex);
    glDeleteFramebuffers(1, &ssaoFBO);   glDeleteTextures(1, &ssaoTex);
    glDeleteFramebuffers(1, &ssaoBlurFBO); glDeleteTextures(1, &ssaoBlurTex);
    glDeleteFramebuffers(2, dofFBO);     glDeleteTextures(2, dofTex);
    glDeleteFramebuffers(2, bloomFBO);   glDeleteTextures(2, bloomTex);
    glDeleteFramebuffers(2, wideBloomFBO); glDeleteTextures(2, wideBloomTex);
    glDeleteFramebuffers(1, &ps1FBO);     glDeleteTextures(1, &ps1Tex);
    glDeleteVertexArrays(1, &skyVAO);  glDeleteVertexArrays(1, &quadVAO);
    glDeleteProgram(cityProg);   glDeleteProgram(skyProg);
    glDeleteProgram(threshProg); glDeleteProgram(blurProg);   glDeleteProgram(compProg);
    glDeleteProgram(shadowProg); glDeleteProgram(normalProg); glDeleteProgram(ssaoProg);
    glDeleteProgram(ssaoBlurProg); glDeleteProgram(dofProg);  glDeleteProgram(ps1Prog);
    SDL_GL_DestroyContext(ctx);
    SDL_DestroyWindow(win);
    // Audio cleanup
    if (audioOK) {
        if (hasAmbient) { ma_sound_stop(&sndAmbient);  ma_sound_uninit(&sndAmbient); }
        for (int i = 0; i < numFootsteps; i++) ma_sound_uninit(&sndFootsteps[i]);
        if (hasJump)     ma_sound_uninit(&sndJump);
        if (hasLand)     ma_sound_uninit(&sndLand);
        ma_engine_uninit(&maEngine);
    }
    SDL_Quit();
    return 0;
}
