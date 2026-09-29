// TOSFIXPLUS: in-between frames and pacing.
//
// The game updates its world 30 times a second. After it finishes frame N, this file:
//   1. pairs frame N's draws with frame N-1's (see buildPlan);
//   2. draws frame N again once per display refresh, with every paired object placed partway
//      between where it was in N-1 and where it is in N, according to the moment that refresh
//      reaches the screen;
//   3. hands control back to the game just in time for it to produce frame N+1 on schedule, so
//      the game keeps running at exactly 30 updates a second (TOSFIXPLUS is the frame limiter).
//
// What moves between frames, and how it is blended:
//   - Objects placed by matrices (scenery, the camera): the shader constants c0-c11 hold each
//     draw's world/view/projection matrices (the game's shaders keep their names: mMatrixWVP,
//     mMatrixWV, mMatrixW), and are blended.
//   - Characters: posed on the CPU, their vertices rewritten each frame; positions and normals
//     are blended.
//   - Sprites (effects, grass, shadows, the target marker): camera-facing quads rebuilt every
//     frame in one shared buffer, drawn one triangle per draw. Their shared camera matrix is
//     blended. The battle target marker and the round ground shadows also move with what they
//     follow; they are recognised by the part of the texture they show.
//   - 2D (HUD, menus, text) and videos are never blended.
// Nothing is blended across a camera cut.
#include "recorder.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <dwmapi.h>
#include <mmsystem.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

static const double GAME_PERIOD_MS = 1000.0 / 30.0;   // the game's update rate
static const double MARGIN_MS = 1.0;                  // extra time left for the game each frame

static bool gEnabled = true;       // F9
static bool gF9WasDown;
static double gMaxInterval = 0;    // [TOSFIXPLUS] MaxFPS as a minimum time between frames, 0 = none

void configureInterpolation(const char* ini) {
    int maxFps = GetPrivateProfileIntA("TOSFIXPLUS", "MaxFPS", 0, ini);
    gMaxInterval = maxFps > 0 ? 1000.0 / maxFps : 0;
    timeBeginPeriod(1);   // 1 ms sleep resolution for pacing
    log("MaxFPS %d (%s)", maxFps, maxFps > 0 ? "cap" : "the display's refresh rate");
}

// ---------------------------------------------------------------- sprite signatures

// A sprite type, by the part of its texture it shows (texture coordinates u0 v0 u1 v1). These
// stay the same with or without the 4K texture pack.
struct TextureArea { float lo[2], hi[2]; };

// The battle target marker: its left and right halves in the UI texture. Moved with the enemy it
// points at. (The same shape drawn flat beside the enemy icons is 2D and never blends.)
static const TextureArea MARKER[] = {{{0.0f, 0.5f}, {0.0469f, 0.625f}}, {{0.0469f, 0.5f}, {0.0938f, 0.625f}}};

// Round ground shadows: a ring of about 16 separate triangles around a shared centre vertex.
// Grouped into whole shadows, each moved as one piece.
static const TextureArea SHADOW[] = {{{0.0156f, 0.0156f}, {0.0156f, 0.9844f}}};

// ---------------------------------------------------------------- per-draw information

enum { REGISTERS = 20 };   // c0-c19 hold the matrices and light vectors

struct DrawInfo {
    uint64_t key;    // identity: target, shaders, buffers and offsets, draw arguments, first texture
    uint64_t key2;   // the same without buffer and offsets (sprites move around the shared buffer)
    size_t command;
    float reg[REGISTERS][4];
    uint32_t flags;   // shaderFlags of the vertex shader
    void* vertexBuffer; UINT offset, stride;
    void* indexBuffer;
    IDirect3DVertexDeclaration9* declaration;
};

static uint64_t mix(uint64_t h, uint64_t v) { return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)); }

// Walks a recorded frame and describes each draw with the state it was made with.
static void describeDraws(const Frame& f, std::vector<DrawInfo>& out) {
    out.clear();
    const DeviceState& s = *f.start;
    void* target = s.renderTarget[0]; void* vs = s.vertexShader; void* ps = s.pixelShader;
    void* ib = s.indices; void* tex0 = s.texture[0];
    void* vb = s.stream[0].buffer; UINT offset = s.stream[0].offset, stride = s.stream[0].stride;
    IDirect3DVertexDeclaration9* declaration = s.declaration;
    float reg[REGISTERS][4];
    for (int i = 0; i < REGISTERS; i++) memcpy(reg[i], s.vsFloat[i], 16);
    const uint8_t* bytes = f.bytes.data();
    for (size_t i = 0; i < f.commands.size(); i++) {
        const Command& c = f.commands[i];
        switch (c.op) {
        case OP_RENDER_TARGET: if (c.a == 0) target = c.p; break;
        case OP_VERTEX_SHADER: vs = c.p; break;
        case OP_PIXEL_SHADER: ps = c.p; break;
        case OP_INDICES: ib = c.p; break;
        case OP_TEXTURE: if (c.a == 0) tex0 = c.p; break;
        case OP_DECLARATION: declaration = (IDirect3DVertexDeclaration9*)c.p; break;
        case OP_STREAM: if (c.a == 0) { vb = c.p; offset = c.b; stride = c.c; } break;
        case OP_CONSTANTS:
            if (c.kind == VS_FLOAT)
                for (UINT k = 0; k < c.b; k++)
                    if (c.a + k < REGISTERS) memcpy(reg[c.a + k], bytes + c.data + 16 * k, 16);
            break;
        case OP_DRAW: case OP_DRAW_INDEXED: {
            DrawInfo d;
            uint64_t h = mix(0, (uintptr_t)target);
            h = mix(h, (uintptr_t)vs); h = mix(h, (uintptr_t)ps); h = mix(h, (uintptr_t)ib);
            h = mix(h, (uintptr_t)vb); h = mix(h, offset); h = mix(h, (uintptr_t)tex0);
            h = mix(h, c.op); h = mix(h, c.a); h = mix(h, c.b); h = mix(h, c.c); h = mix(h, c.d); h = mix(h, c.e); h = mix(h, c.f);
            d.key = h;
            uint64_t h2 = mix(0, (uintptr_t)target);
            h2 = mix(h2, (uintptr_t)vs); h2 = mix(h2, (uintptr_t)ps); h2 = mix(h2, (uintptr_t)tex0);
            h2 = mix(h2, c.op); h2 = mix(h2, c.a); h2 = mix(h2, c.op == OP_DRAW_INDEXED ? c.f : c.c); h2 = mix(h2, stride);
            d.key2 = h2;
            d.command = i;
            memcpy(d.reg, reg, sizeof reg);
            d.flags = shaderFlags(vs);
            d.vertexBuffer = vb; d.offset = offset; d.stride = stride; d.indexBuffer = ib; d.declaration = declaration;
            out.push_back(d);
            break;
        }
        default: break;
        }
    }
}

// Where position, normal and texture coordinates sit in a vertex of stream 0.
struct VertexLayout { UINT stride = 0; int position = -1, normal = -1, texcoord = -1; };

static std::unordered_map<void*, VertexLayout> gLayouts;   // per vertex declaration

static VertexLayout layoutOf(IDirect3DVertexDeclaration9* declaration, UINT stride) {
    VertexLayout L; L.stride = stride;
    if (!declaration) return L;
    auto it = gLayouts.find(declaration);
    if (it == gLayouts.end()) {
        VertexLayout x;
        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1]; UINT n = MAXD3DDECLLENGTH + 1;
        if (SUCCEEDED(declaration->lpVtbl->GetDeclaration(declaration, el, &n)))
            for (UINT i = 0; i + 1 < n; i++) {
                if (el[i].Stream != 0 || el[i].UsageIndex != 0) continue;
                if (el[i].Type == D3DDECLTYPE_FLOAT2 && el[i].Usage == D3DDECLUSAGE_TEXCOORD) x.texcoord = el[i].Offset;
                if (el[i].Type != D3DDECLTYPE_FLOAT3) continue;
                if (el[i].Usage == D3DDECLUSAGE_POSITION) x.position = el[i].Offset;
                if (el[i].Usage == D3DDECLUSAGE_NORMAL) x.normal = el[i].Offset;
            }
        it = gLayouts.emplace(declaration, x).first;
    }
    L.position = it->second.position; L.normal = it->second.normal; L.texcoord = it->second.texcoord;
    return L;
}

// How much a draw's matrices changed between frames, relative to their size, over the first
// `registers` constants. A camera move changes every matrix a little (walking ~0.07, battle
// camera swings ~0.3); a camera cut changes most of them completely.
static float matrixChange(const DrawInfo& a, const DrawInfo& b, int registers) {
    double diff = 0, size = 0;
    for (int i = 0; i < registers; i++)
        for (int k = 0; k < 4; k++) {
            double x = a.reg[i][k], y = b.reg[i][k];
            diff += (y - x) * (y - x); size += y * y;
        }
    return size > 0 ? (float)sqrt(diff / size) : 0.0f;
}

// True for a flat (orthographic) projection in c0-c3, whose 4th column is (0, 0, 0, 1): the HUD,
// menus and on-screen text. Those never blend.
static bool flat(const DrawInfo& d) {
    return d.reg[0][3] == 0 && d.reg[1][3] == 0 && d.reg[2][3] == 0 && d.reg[3][3] == 1;
}

static float distance2(const float* a, const float* b) {
    float s = 0;
    for (int k = 0; k < 3; k++) s += (a[k] - b[k]) * (a[k] - b[k]);
    return s;
}

// ---------------------------------------------------------------- sprites

// A sprite draw's vertices: where the frame wrote them, their centre, size and texture area.
struct Sprite {
    const DrawInfo* d = nullptr;
    const uint8_t* data = nullptr;   // the first vertex the draw uses, in the frame's recorded bytes
    uint32_t start = 0, bytes = 0;   // byte range in the vertex buffer
    VertexLayout L;
    float centre[3] = {}, size2 = 0;
    float uvLo[2] = {1e30f, 1e30f}, uvHi[2] = {-1e30f, -1e30f};
};

// Per vertex buffer, the frame's write commands in order.
typedef std::unordered_map<void*, std::vector<size_t>> WriteIndex;
static void indexWrites(const Frame& f, WriteIndex& out) {
    out.clear();
    for (size_t i = 0; i < f.commands.size(); i++)
        if (f.commands[i].op == OP_VERTEX_WRITE) out[f.commands[i].p].push_back(i);
}

static Sprite spriteOf(const Frame& f, const WriteIndex& writes, const DrawInfo& d) {
    Sprite s; s.d = &d;
    auto info = gBuffers.find(d.vertexBuffer);
    if (info == gBuffers.end() || !(info->second.usage & D3DUSAGE_DYNAMIC) || !d.stride) return s;
    s.L = layoutOf(d.declaration, d.stride);
    if (s.L.position < 0) return s;
    // Which vertices the draw uses. An indexed draw's arguments can't be trusted for this: the
    // game's sprite draws say "vertices 0-2" while their index lists point thousands further on.
    const Command& c = f.commands[d.command];
    UINT first, count;
    if (c.op == OP_DRAW_INDEXED) {
        auto copy = gIndexCopies.find(d.indexBuffer);
        auto ib = gBuffers.find(d.indexBuffer);
        if (copy == gIndexCopies.end() || ib == gBuffers.end()) return s;
        UINT n = verticesFor((D3DPRIMITIVETYPE)c.a, c.f), size = ib->second.format == D3DFMT_INDEX32 ? 4 : 2;
        if (!n || (uint64_t)(c.e + n) * size > copy->second.size()) return s;
        UINT lo = UINT_MAX, hi = 0;
        for (UINT k = 0; k < n; k++) {
            const uint8_t* p = copy->second.data() + (size_t)(c.e + k) * size;
            UINT v = (UINT)((INT)c.b + (INT)(size == 4 ? *(const uint32_t*)p : *(const uint16_t*)p));
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        if (hi - lo > 4096) return s;
        first = lo; count = hi - lo + 1;
    } else {
        first = c.b; count = verticesFor((D3DPRIMITIVETYPE)c.a, c.c);
    }
    UINT start = d.offset + first * d.stride, bytes = count * d.stride;
    // The last write to the buffer before the draw that holds all of those vertices.
    auto list = writes.find(d.vertexBuffer);
    if (list == writes.end()) return s;
    const std::vector<size_t>& ws = list->second;
    for (size_t k = std::lower_bound(ws.begin(), ws.end(), d.command) - ws.begin(); k-- > 0;) {
        const Command& w = f.commands[ws[k]];
        if (w.a > start || start + bytes > w.a + w.b) continue;
        s.start = start; s.bytes = bytes;
        s.data = f.bytes.data() + w.data + (start - w.a);
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (UINT v = 0; v < count; v++) {
            const float* p = (const float*)(s.data + v * d.stride + s.L.position);
            for (int j = 0; j < 3; j++) { s.centre[j] += p[j] / count; lo[j] = std::min(lo[j], p[j]); hi[j] = std::max(hi[j], p[j]); }
            if (s.L.texcoord >= 0) {
                const float* t = (const float*)(s.data + v * d.stride + s.L.texcoord);
                for (int j = 0; j < 2; j++) { s.uvLo[j] = std::min(s.uvLo[j], t[j]); s.uvHi[j] = std::max(s.uvHi[j], t[j]); }
            }
        }
        s.size2 = distance2(lo, hi);
        break;
    }
    return s;
}

template <size_t N> static bool shows(const Sprite& s, const TextureArea (&areas)[N]) {
    for (const TextureArea& a : areas) {
        bool same = true;
        for (int k = 0; k < 2; k++)
            same = same && fabsf(s.uvLo[k] - a.lo[k]) < 0.002f && fabsf(s.uvHi[k] - a.hi[k]) < 0.002f;
        if (same) return true;
    }
    return false;
}

// A whole shape made of several sprite draws that share vertices (a shadow's ring of triangles).
struct Shape { std::vector<int> parts; float centre[3] = {}; float size2 = 0; };

static std::vector<Shape> groupShapes(const std::vector<Sprite>& v) {
    std::vector<int> up(v.size());
    for (size_t i = 0; i < v.size(); i++) up[i] = (int)i;
    auto root = [&](int i) { while (up[i] != i) i = up[i] = up[up[i]]; return i; };
    std::unordered_map<uint64_t, int> seen;   // vertex position -> first sprite with it
    for (size_t i = 0; i < v.size(); i++)
        for (UINT o = 0; o + v[i].L.stride <= v[i].bytes; o += v[i].L.stride) {
            const float* p = (const float*)(v[i].data + o + v[i].L.position);
            uint64_t k = mix(mix(mix(0, (int64_t)lroundf(p[0] * 1000)), (int64_t)lroundf(p[1] * 1000)), (int64_t)lroundf(p[2] * 1000));
            auto it = seen.emplace(k, (int)i).first;
            up[root((int)i)] = root(it->second);
        }
    std::unordered_map<int, int> index;
    std::vector<Shape> out;
    for (size_t i = 0; i < v.size(); i++) {
        auto it = index.emplace(root((int)i), (int)out.size()).first;
        if (it->second == (int)out.size()) out.emplace_back();
        out[it->second].parts.push_back((int)i);
    }
    for (Shape& sh : out) {
        for (int i : sh.parts) for (int k = 0; k < 3; k++) sh.centre[k] += v[i].centre[k] / sh.parts.size();
        for (int i : sh.parts) sh.size2 = std::max(sh.size2, 4 * distance2(sh.centre, v[i].centre) + v[i].size2);
    }
    return out;
}

// ---------------------------------------------------------------- the blend plan for a frame

struct Plan : ReplayHooks {
    float alpha = 1;                          // 0 = frame N-1, 1 = frame N
    std::vector<DrawInfo> current, previous;  // draws of frame N and N-1
    uint64_t currentNumber = 0, previousNumber = 0;
    // Per command of frame N:
    std::vector<int> pair;                    // the paired draw of frame N-1, -1 = none
    std::vector<const DrawInfo*> draw;        // its DrawInfo
    std::vector<uint8_t> registers;           // constants to blend from c0: 12, or 4 for sprites
    std::vector<const uint8_t*> writeFrom;    // character buffer writes: frame N-1's bytes
    std::vector<VertexLayout> writeLayout;
    std::vector<int> spriteMove;              // index into moves, -1 = none

    // A sprite that moves with what it follows (the marker, shadows). Its vertices live in the
    // game's shared buffer, which the game writes with NOOVERWRITE: a promise not to touch data
    // the GPU may still be reading. So the moved copy goes into TOSFIXPLUS's own buffer, renewed
    // for every in-between frame, at the same byte position, and the draw reads from there.
    struct Move { uint32_t start, bytes; const uint8_t* vertices; float by[3]; VertexLayout L;
                  IDirect3DVertexBuffer9* sharedBuffer; UINT offset, stride; };
    std::vector<Move> moves;
    IDirect3DVertexBuffer9* ownBuffer = nullptr;
    UINT ownSize = 0;
    bool ownRenewed = false;                  // within the current in-between frame
    std::vector<uint8_t> scratch;

    bool cut = false;
    int blended = 0, characters = 0, sprites = 0;

    void beforeDraw(IDirect3DDevice9Ex* d, size_t cmd) override {
        int m = pair[cmd];
        if (m < 0 || alpha >= 1) return;
        const DrawInfo& a = previous[m]; const DrawInfo& b = *draw[cmd];
        float out[REGISTERS][4];
        for (int i = 0; i < REGISTERS; i++)
            for (int k = 0; k < 4; k++) out[i][k] = a.reg[i][k] + (b.reg[i][k] - a.reg[i][k]) * alpha;
        int n = registers[cmd];
        R.SetVertexShaderConstantF(d, 0, &out[0][0], n);
        if (n == 12 && (b.flags & SHADER_HAS_LIGHTS)) R.SetVertexShaderConstantF(d, 16, &out[16][0], 3);
        if (spriteMove[cmd] >= 0) moveSprite(d, moves[spriteMove[cmd]]);
    }

    void afterDraw(IDirect3DDevice9Ex* d, size_t cmd) override {
        // Put back what the rest of the frame expects.
        int m = pair[cmd];
        if (m < 0 || alpha >= 1) return;
        const DrawInfo& b = *draw[cmd];
        int n = registers[cmd];
        R.SetVertexShaderConstantF(d, 0, &b.reg[0][0], n);
        if (n == 12 && (b.flags & SHADER_HAS_LIGHTS)) R.SetVertexShaderConstantF(d, 16, &b.reg[16][0], 3);
        if (spriteMove[cmd] >= 0) {
            const Move& mv = moves[spriteMove[cmd]];
            R.SetStreamSource(d, 0, mv.sharedBuffer, mv.offset, mv.stride);
        }
    }

    void moveSprite(IDirect3DDevice9Ex* d, const Move& mv) {
        UINT need = mv.start + mv.bytes;
        if (!ownBuffer || ownSize < need) {
            if (ownBuffer) ownBuffer->lpVtbl->Release(ownBuffer);
            ownBuffer = nullptr;
            ownSize = std::max<UINT>(need, 4u << 20);
            if (FAILED(R.CreateVertexBuffer(d, ownSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &ownBuffer, nullptr))) {
                ownBuffer = nullptr; ownSize = 0;
                return;
            }
            ownRenewed = false;
        }
        void* v = nullptr;
        DWORD flags = ownRenewed ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD;
        ownRenewed = true;
        if (FAILED(realLock(ownBuffer, mv.start, mv.bytes, &v, flags)) || !v) return;
        memcpy(v, mv.vertices, mv.bytes);
        float back = 1.0f - alpha;
        for (uint32_t o = 0; o + mv.L.stride <= mv.bytes; o += mv.L.stride) {
            float* p = (float*)((uint8_t*)v + o + mv.L.position);
            for (int k = 0; k < 3; k++) p[k] -= mv.by[k] * back;
        }
        realUnlock(ownBuffer);
        R.SetStreamSource(d, 0, ownBuffer, mv.offset, mv.stride);
    }

    const uint8_t* bufferData(size_t cmd, const Command& c, const uint8_t* recorded) override {
        // Character vertices: blend position and normal (the shaders renormalise normals).
        const uint8_t* from = writeFrom[cmd];
        if (!from || alpha >= 1) return recorded;
        const VertexLayout& L = writeLayout[cmd];
        scratch.assign(recorded, recorded + c.b);
        for (UINT v = 0; v + L.stride <= c.b; v += L.stride)
            for (int off : {L.position, L.normal}) {
                if (off < 0) continue;
                const float* p = (const float*)(from + v + off);
                const float* q = (const float*)(recorded + v + off);
                float* o = (float*)(scratch.data() + v + off);
                for (int k = 0; k < 3; k++) o[k] = p[k] + (q[k] - p[k]) * alpha;
            }
        return scratch.data();
    }
};

static Plan gPlan;

static void addMove(Plan& P, const Sprite& s, const float* by, int pairWith) {
    P.spriteMove[s.d->command] = (int)P.moves.size();
    Plan::Move mv{s.start, s.bytes, s.data, {by[0], by[1], by[2]}, s.L,
                  (IDirect3DVertexBuffer9*)s.d->vertexBuffer, s.d->offset, s.d->stride};
    P.moves.push_back(mv);
    P.pair[s.d->command] = pairWith;
    P.registers[s.d->command] = 4;
}

// Pairs frame N's draws with frame N-1's and decides what blends.
static void buildPlan() {
    Locked lock;   // shader flags and buffer information are shared with loading threads
    Plan& P = gPlan;
    P.previousNumber = P.currentNumber;
    std::swap(P.previous, P.current);
    describeDraws(gLastFrame, P.current);
    P.currentNumber = gLastFrame.number;
    size_t n = gLastFrame.commands.size();
    P.pair.assign(n, -1); P.draw.assign(n, nullptr); P.registers.assign(n, 12);
    P.writeFrom.assign(n, nullptr); P.writeLayout.assign(n, VertexLayout{});
    P.spriteMove.assign(n, -1); P.moves.clear();
    P.cut = false; P.blended = P.characters = P.sprites = 0;
    for (const DrawInfo& d : P.current) P.draw[d.command] = &d;
    // Needs frame N-1 to be the frame right before N.
    if (!gFrameBefore.complete || P.previousNumber + 1 != P.currentNumber || P.previous.empty()) return;

    // 1. Objects: pair draws that are identical but for their constants, in order.
    std::unordered_map<uint64_t, std::vector<int>> byKey;
    for (int i = (int)P.previous.size() - 1; i >= 0; i--) byKey[P.previous[i].key].push_back(i);
    std::vector<bool> used(P.previous.size());
    std::unordered_map<void*, VertexLayout> characterBuffers;   // dynamic buffers of paired objects
    int placed = 0, jumped = 0;
    for (const DrawInfo& d : P.current) {
        auto it = byKey.find(d.key);
        if (it == byKey.end() || it->second.empty()) continue;
        int m = it->second.back(); it->second.pop_back();
        used[m] = true;
        if (!(d.flags & SHADER_PLACES_OBJECT)) continue;
        placed++;
        if (matrixChange(P.previous[m], d, 8) > 0.8f) jumped++;
        P.pair[d.command] = m;
        P.blended++;
        auto info = gBuffers.find(d.vertexBuffer);
        if (info != gBuffers.end() && (info->second.usage & D3DUSAGE_DYNAMIC)) characterBuffers[d.vertexBuffer] = layoutOf(d.declaration, d.stride);
    }

    // 2. Sprites: their shared camera matrix (c0-c3) blends. They are paired in order within
    //    each kind (same shader, texture and size); the matrix is the same for all of them, so
    //    order is enough. Flat 2D and big matrix changes (animations) don't blend.
    {
        std::unordered_map<uint64_t, std::vector<int>> byKind;
        for (int i = (int)P.previous.size() - 1; i >= 0; i--)
            if (!used[i] && (P.previous[i].flags & SHADER_PLACES_OBJECT)) byKind[P.previous[i].key2].push_back(i);
        for (const DrawInfo& d : P.current) {
            if (P.pair[d.command] >= 0 || !(d.flags & SHADER_PLACES_OBJECT) || flat(d)) continue;
            auto it = byKind.find(d.key2);
            if (it == byKind.end() || it->second.empty()) continue;
            int m = it->second.back(); it->second.pop_back();
            const DrawInfo& p = P.previous[m];
            if (flat(p) || !memcmp(p.reg, d.reg, 4 * 16) || matrixChange(p, d, 4) > 0.6f) continue;
            P.pair[d.command] = m;
            P.registers[d.command] = 4;
            P.sprites++;
        }
    }

    // 3. The target marker and the shadows also move with what they follow.
    {
        static WriteIndex writesNow, writesBefore;
        indexWrites(gLastFrame, writesNow); indexWrites(gFrameBefore, writesBefore);
        std::vector<Sprite> markersNow, markersBefore, shadowsNow, shadowsBefore;
        for (const DrawInfo& d : P.current) {
            if (!(d.flags & SHADER_PLACES_OBJECT) || flat(d) || (P.pair[d.command] >= 0 && P.registers[d.command] != 4)) continue;
            Sprite s = spriteOf(gLastFrame, writesNow, d);
            if (!s.data) continue;
            if (shows(s, MARKER)) markersNow.push_back(s);
            else if (shows(s, SHADOW)) shadowsNow.push_back(s);
        }
        for (size_t i = 0; i < P.previous.size(); i++) {
            const DrawInfo& d = P.previous[i];
            if (used[i] || !(d.flags & SHADER_PLACES_OBJECT) || flat(d)) continue;
            Sprite s = spriteOf(gFrameBefore, writesBefore, d);
            if (!s.data) continue;
            if (shows(s, MARKER)) markersBefore.push_back(s);
            else if (shows(s, SHADOW)) shadowsBefore.push_back(s);
        }
        // Each half of the marker pairs with the nearest of the same kind; one that moved more
        // than twice its size isn't moved.
        std::vector<bool> taken(markersBefore.size());
        for (const Sprite& s : markersNow) {
            int best = -1; float bestD = 1e30f;
            for (size_t j = 0; j < markersBefore.size(); j++) {
                if (taken[j] || markersBefore[j].d->key2 != s.d->key2) continue;
                float dd = distance2(s.centre, markersBefore[j].centre);
                if (dd < bestD) { bestD = dd; best = (int)j; }
            }
            if (best < 0) continue;
            taken[best] = true;
            const Sprite& p = markersBefore[best];
            if (matrixChange(*p.d, *s.d, 4) > 0.6f || s.bytes != p.bytes || bestD > 4 * std::max(s.size2, p.size2)) continue;
            float by[3] = {s.centre[0] - p.centre[0], s.centre[1] - p.centre[1], s.centre[2] - p.centre[2]};
            addMove(P, s, by, (int)(p.d - P.previous.data()));
            P.sprites++;
        }
        // Shadows: whole shapes pair with the nearest; one that moved more than its size isn't moved.
        std::vector<Shape> now = groupShapes(shadowsNow), before = groupShapes(shadowsBefore);
        std::vector<bool> gone(before.size());
        for (const Shape& a : now) {
            int best = -1; float bestD = 1e30f;
            for (size_t j = 0; j < before.size(); j++) {
                if (gone[j]) continue;
                float dd = distance2(a.centre, before[j].centre);
                if (dd < bestD) { bestD = dd; best = (int)j; }
            }
            if (best < 0 || bestD > std::max(a.size2, before[best].size2)) continue;
            gone[best] = true;
            const Sprite& ref = shadowsBefore[before[best].parts[0]];
            float by[3] = {a.centre[0] - before[best].centre[0], a.centre[1] - before[best].centre[1], a.centre[2] - before[best].centre[2]};
            for (int i : a.parts) {
                if (matrixChange(*ref.d, *shadowsNow[i].d, 4) > 0.6f) continue;
                addMove(P, shadowsNow[i], by, (int)(ref.d - P.previous.data()));
                P.sprites++;
            }
        }
    }

    // A camera cut moves most objects at once: blend nothing.
    if (placed && jumped * 2 > placed) {
        P.cut = true;
        std::fill(P.pair.begin(), P.pair.end(), -1);
        std::fill(P.spriteMove.begin(), P.spriteMove.end(), -1);
        return;
    }

    // 4. Characters: blend a vertex write when frame N-1 wrote the same buffer range, unless the
    //    mesh moved further than its own size (a teleport).
    std::unordered_map<uint64_t, const uint8_t*> writesBefore;
    auto writeKey = [](const Command& c) { return mix(mix(mix(0, (uintptr_t)c.p), c.a), c.b); };
    for (const Command& c : gFrameBefore.commands)
        if (c.op == OP_VERTEX_WRITE) writesBefore[writeKey(c)] = gFrameBefore.bytes.data() + c.data;
    for (size_t i = 0; i < n; i++) {
        const Command& c = gLastFrame.commands[i];
        if (c.op != OP_VERTEX_WRITE) continue;
        auto layout = characterBuffers.find(c.p);
        if (layout == characterBuffers.end() || layout->second.position < 0 || !layout->second.stride) continue;
        auto before = writesBefore.find(writeKey(c));
        if (before == writesBefore.end()) continue;
        const VertexLayout& L = layout->second;
        const uint8_t* now = gLastFrame.bytes.data() + c.data;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, furthest = 0;
        for (UINT v = 0; v + L.stride <= c.b; v += L.stride) {
            const float* p = (const float*)(before->second + v + L.position);
            const float* q = (const float*)(now + v + L.position);
            for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], q[k]); hi[k] = std::max(hi[k], q[k]); }
            furthest = std::max(furthest, distance2(p, q));
        }
        if (furthest > distance2(lo, hi)) continue;
        P.writeFrom[i] = before->second;
        P.writeLayout[i] = L;
        P.characters++;
    }
}

// Puts the device back exactly as the game left it after frame N: its character vertices and
// its state.
static void restore(IDirect3DDevice9Ex* d) {
    std::unordered_set<void*> done;
    for (size_t i = gLastFrame.commands.size(); i-- > 0;) {
        const Command& c = gLastFrame.commands[i];
        if (c.op != OP_VERTEX_WRITE || !gPlan.writeFrom[i] || !done.insert(c.p).second) continue;
        void* p = nullptr;
        if (SUCCEEDED(realLock(c.p, c.a, c.b, &p, c.c & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) && p) {
            memcpy(p, gLastFrame.bytes.data() + c.data, c.b);
            realUnlock(c.p);
        }
    }
    applyState(d, gState);
}

void resetInterpolation() {
    if (gPlan.ownBuffer) { gPlan.ownBuffer->lpVtbl->Release(gPlan.ownBuffer); gPlan.ownBuffer = nullptr; gPlan.ownSize = 0; }
    gPlan.currentNumber = 0;   // the recorded draws refer to the old device's objects
}

// ---------------------------------------------------------------- pacing

static double nowMs() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)f.QuadPart;
}

static void waitUntil(double t) {
    for (;;) {
        double left = t - nowMs();
        if (left <= 0) return;
        if (left > 2.0) Sleep((DWORD)(left - 1.5));
        else YieldProcessor();
    }
}

// In a window (TSFix's borderless mode) the desktop compositor puts a new image on screen once
// per refresh. It reports when the last refresh happened and the time between refreshes.
struct Refresh { double last, period; bool known; };

static Refresh refresh() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    DWM_TIMING_INFO ti = {}; ti.cbSize = sizeof ti;
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod)
        return {1000.0 * (double)ti.qpcVBlank / (double)f.QuadPart, 1000.0 * (double)ti.qpcRefreshPeriod / (double)f.QuadPart, true};
    return {0, 1000.0 / 60.0, false};
}
static double nextRefresh(const Refresh& r, double t) {
    if (!r.known) return t + r.period;
    return r.last + ceil((t - r.last) / r.period) * r.period;
}

static double gStart = 0;      // when frame N's share of the timeline began
static double gWork = 5.0;     // how long the game needs from our return to its next Present
static double gCost = 1.0;     // one in-between frame: replay and Present
static double gLastExit = 0;
static double gLastShown = 0;  // the refresh the last Present appears at
static uint64_t gStatFrames, gStatPresents;
static double gStatStart, gStatRefresh;

HRESULT presentFrame(IDirect3DDevice9Ex* d, HRESULT (*present)(void*), void* context) {
    double entry = nowMs();
    // The game's own time per frame. Slow frames (loading) are left out, so the estimate doesn't
    // stay high afterwards.
    if (gLastExit > 0 && entry - gLastExit <= GAME_PERIOD_MS) gWork = gWork * 0.9 + (entry - gLastExit) * 0.1;

    // F9 turns blending on and off (not with Alt, Ctrl or Shift held: NVIDIA's recorder uses Alt+F9).
    bool modifiers = (GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT)) & 0x8000;
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) && !modifiers;
    if (f9 && !gF9WasDown) { gEnabled = !gEnabled; log("TOSFIXPLUS %s", gEnabled ? "on" : "off"); }
    gF9WasDown = f9;

    // The schedule: one game frame per 1/30 s, restarted after a stall (loading, a hitch).
    if (gStart == 0 || entry - gStart > 2 * GAME_PERIOD_MS) gStart = entry;
    else gStart += GAME_PERIOD_MS;
    double deadline = gStart + GAME_PERIOD_MS - gWork - MARGIN_MS;

    bool blend = gEnabled && gLastFrame.complete;
    if (blend) buildPlan();
    blend = blend && !gPlan.cut && (gPlan.blended || gPlan.characters || gPlan.sprites);

    HRESULT hr = S_OK;
    int presents = 0;
    Refresh r = refresh();
    if (!blend) {
        hr = present(context);
        presents = 1;
        gLastShown = nextRefresh(r, nowMs());
    } else {
        // One in-between frame per refresh (or per MaxFPS interval). A Present appears at the
        // first refresh after it completes, and a second Present before then would replace it,
        // so each frame is drawn after the previous one's refresh has passed, for the next one.
        // Its blend is taken for that refresh's time: one continuous timeline across game frames.
        double interval = std::max(r.period, gMaxInterval);
        for (;;) {
            r = refresh();
            interval = std::max(r.period, gMaxInterval);
            double target = nextRefresh(r, std::max(nowMs() + gCost + 0.5, gLastShown + interval - r.period * 0.5));
            waitUntil(target - r.period + 0.3);
            gPlan.alpha = (float)std::min(1.0, std::max(0.0, (target - gStart - r.period * 0.5) / GAME_PERIOD_MS));
            double t = nowMs();
            gPlan.ownRenewed = false;
            replay(d, gLastFrame, &gPlan);
            hr = present(context);
            gCost = gCost * 0.9 + (nowMs() - t) * 0.1;
            gLastShown = target;
            presents++;
            if (FAILED(hr) || presents >= 16) break;
            // Stop if the next frame would miss the deadline or run past this game frame's share.
            double nextTarget = target + interval;
            if (nextTarget - r.period + 0.3 + gCost > deadline || nextTarget > gStart + GAME_PERIOD_MS + r.period * 0.5) break;
        }
        restore(d);
    }
    waitUntil(deadline);
    gLastExit = nowMs();

    // A line in the log every minute, for bug reports.
    gStatFrames++; gStatPresents += presents; gStatRefresh = r.period;
    if (entry - gStatStart > 60000) {
        if (gStatStart > 0)
            log("%.2f game frames/s, %.1f frames shown/s, display %.1f Hz, blending %s", gStatFrames * 1000.0 / (entry - gStatStart),
                gStatPresents * 1000.0 / (entry - gStatStart), 1000.0 / gStatRefresh, gEnabled ? "on" : "off");
        gStatStart = entry; gStatFrames = gStatPresents = 0;
    }
    return hr;
}
