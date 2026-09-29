// TOSFIXPLUS: frame recorder and replay.
//
// Every call the game makes to change device state or draw is recorded, frame by frame, together
// with the full device state at the frame's start and a copy of everything it writes into
// dynamic vertex and index buffers. replay() issues a recorded frame to the device again, which
// is what the in-between frames are drawn from.
//
// A recorded frame keeps a reference to every object it uses, so nothing it needs is freed before
// a replay. resetRecorder() drops them all; it runs before a device Reset, which requires that.
#include "recorder.h"
#include <cstring>

DeviceState gState;
Frame gLastFrame, gFrameBefore;
static Frame gCurrent;           // the frame being recorded (starts at the first Present)
static uint64_t gFrameNumber;

static void addRef(void* o) { if (o) ((IUnknown*)o)->lpVtbl->AddRef((IUnknown*)o); }
static void release(void* o) { if (o) ((IUnknown*)o)->lpVtbl->Release((IUnknown*)o); }

// Texture and sampler numbers: 0-15 for pixel shaders, D3DVERTEXTEXTURESAMPLER0-3 for vertex
// shaders; stored as slots 0-19.
static int slotOf(DWORD sampler) {
    if (sampler < 16) return (int)sampler;
    if (sampler >= D3DVERTEXTEXTURESAMPLER0 && sampler <= D3DVERTEXTEXTURESAMPLER3)
        return 16 + (int)(sampler - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}
static DWORD samplerOf(int slot) { return slot < 16 ? (DWORD)slot : D3DVERTEXTEXTURESAMPLER0 + (slot - 16); }

template <class F> static void forEachObject(const DeviceState& s, F f) {
    for (auto* p : s.renderTarget) f(p);
    f(s.depthStencil);
    for (auto* p : s.texture) f(p);
    f(s.vertexShader); f(s.pixelShader); f(s.declaration); f(s.indices);
    for (auto& x : s.stream) f(x.buffer);
}

static void dropFrame(Frame& f) {
    for (void* o : f.references) release(o);
    if (f.start) { forEachObject(*f.start, release); delete f.start; }
    f = Frame();
}

void resetRecorder() {
    dropFrame(gCurrent); dropFrame(gLastFrame); dropFrame(gFrameBefore);
    gState = DeviceState();   // a Reset puts every state back to its default
}

void endRecordedFrame() {
    dropFrame(gFrameBefore);
    gFrameBefore = std::move(gLastFrame);
    gLastFrame = std::move(gCurrent);
    gCurrent = Frame();
    gCurrent.start = new DeviceState(gState);
    forEachObject(*gCurrent.start, addRef);
    gCurrent.number = ++gFrameNumber;
    gCurrent.complete = true;   // recording always starts at a frame boundary
}

// ---------------------------------------------------------------- recording

static Command* add(Op op) {
    if (!gCurrent.start) return nullptr;   // before the first frame boundary
    gCurrent.commands.push_back(Command{});
    Command& c = gCurrent.commands.back();
    c.op = op;
    return &c;
}
static void keep(Command& c, void* p, void* q = nullptr) {
    c.p = p; c.q = q;
    if (p) { addRef(p); gCurrent.references.push_back(p); }
    if (q) { addRef(q); gCurrent.references.push_back(q); }
}
static void store(Command& c, const void* data, size_t length) {
    c.data = (uint32_t)gCurrent.bytes.size();
    c.length = (uint32_t)length;
    if (length) gCurrent.bytes.insert(gCurrent.bytes.end(), (const uint8_t*)data, (const uint8_t*)data + length);
}

UINT verticesFor(D3DPRIMITIVETYPE type, UINT primitives) {
    switch (type) {
    case D3DPT_POINTLIST: return primitives;
    case D3DPT_LINELIST: return primitives * 2;
    case D3DPT_LINESTRIP: return primitives + 1;
    case D3DPT_TRIANGLELIST: return primitives * 3;
    default: return primitives + 2;   // strips and fans
    }
}

void recordRenderTarget(DWORD index, IDirect3DSurface9* s) {
    if (index < 4) gState.renderTarget[index] = s;
    if (Command* c = add(OP_RENDER_TARGET)) { c->a = index; keep(*c, s); }
}
void recordDepthStencil(IDirect3DSurface9* s) {
    gState.depthStencil = s;
    if (Command* c = add(OP_DEPTH_STENCIL)) keep(*c, s);
}
void recordClear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    Command* c = add(OP_CLEAR);
    if (!c) return;
    c->a = rects ? count : 0; c->b = flags; c->c = color; memcpy(&c->d, &z, 4); c->e = stencil;
    if (rects && count) store(*c, rects, count * sizeof(D3DRECT));
}
void recordViewport(const D3DVIEWPORT9* v) {
    if (!v) return;
    gState.viewport = *v; gState.viewportSet = true;
    if (Command* c = add(OP_VIEWPORT)) store(*c, v, sizeof *v);
}
void recordScissor(const RECT* r) {
    if (!r) return;
    gState.scissor = *r; gState.scissorSet = true;
    if (Command* c = add(OP_SCISSOR)) store(*c, r, sizeof *r);
}
void recordTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* m) {
    if (!m) return;
    if ((UINT)type < TRANSFORMS) { gState.transform[type] = *m; gState.transformSet[type] = true; }
    if (Command* c = add(OP_TRANSFORM)) { c->a = type; store(*c, m, sizeof *m); }
}
void recordClipPlane(DWORD index, const float* plane) {
    if (!plane) return;
    if (index < CLIP_PLANES) { memcpy(gState.clipPlane[index], plane, 16); gState.clipPlaneSet[index] = true; }
    if (Command* c = add(OP_CLIP_PLANE)) { c->a = index; store(*c, plane, 16); }
}
void recordMaterial(const D3DMATERIAL9* m) {
    if (!m) return;
    if (Command* c = add(OP_MATERIAL)) store(*c, m, sizeof *m);
}
void recordLight(DWORD index, const D3DLIGHT9* l) {
    if (!l) return;
    if (Command* c = add(OP_LIGHT)) { c->a = index; store(*c, l, sizeof *l); }
}
void recordLightEnable(DWORD index, BOOL enable) {
    if (Command* c = add(OP_LIGHT_ENABLE)) { c->a = index; c->b = enable; }
}
void recordRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    if ((UINT)state < RENDER_STATES) { gState.renderState[state] = value; gState.renderStateSet[state] = true; }
    if (Command* c = add(OP_RENDER_STATE)) { c->a = state; c->b = value; }
}
void recordTexture(DWORD stage, IDirect3DBaseTexture9* t) {
    int slot = slotOf(stage);
    if (slot >= 0) gState.texture[slot] = t;
    if (Command* c = add(OP_TEXTURE)) { c->a = stage; keep(*c, t); }
}
void recordTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    if (stage < 8 && (UINT)type < STAGE_STATES) { gState.stageState[stage][type] = value; gState.stageStateSet[stage][type] = true; }
    if (Command* c = add(OP_STAGE_STATE)) { c->a = stage; c->b = type; c->c = value; }
}
void recordSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    int slot = slotOf(sampler);
    if (slot >= 0 && (UINT)type < SAMPLER_STATES) { gState.samplerState[slot][type] = value; gState.samplerStateSet[slot][type] = true; }
    if (Command* c = add(OP_SAMPLER_STATE)) { c->a = sampler; c->b = type; c->c = value; }
}
void recordVertexShader(IDirect3DVertexShader9* s) {
    gState.vertexShader = s;
    if (Command* c = add(OP_VERTEX_SHADER)) keep(*c, s);
}
void recordPixelShader(IDirect3DPixelShader9* s) {
    gState.pixelShader = s;
    if (Command* c = add(OP_PIXEL_SHADER)) keep(*c, s);
}
void recordConstants(ConstantKind kind, UINT start, const void* data, UINT count) {
    if (!data) return;
    for (UINT i = 0; i < count; i++) {
        UINT r = start + i;
        switch (kind) {
        case VS_FLOAT: if (r < VS_FLOAT_REGISTERS) { memcpy(gState.vsFloat[r], (const float*)data + 4 * i, 16); gState.vsFloatSet[r] = true; } break;
        case PS_FLOAT: if (r < PS_FLOAT_REGISTERS) { memcpy(gState.psFloat[r], (const float*)data + 4 * i, 16); gState.psFloatSet[r] = true; } break;
        case VS_INT: if (r < INT_BOOL_REGISTERS) { memcpy(gState.vsInt[r], (const int*)data + 4 * i, 16); gState.vsIntSet[r] = true; } break;
        case PS_INT: if (r < INT_BOOL_REGISTERS) { memcpy(gState.psInt[r], (const int*)data + 4 * i, 16); gState.psIntSet[r] = true; } break;
        case VS_BOOL: if (r < INT_BOOL_REGISTERS) { gState.vsBool[r] = ((const BOOL*)data)[i]; gState.vsBoolSet[r] = true; } break;
        case PS_BOOL: if (r < INT_BOOL_REGISTERS) { gState.psBool[r] = ((const BOOL*)data)[i]; gState.psBoolSet[r] = true; } break;
        }
    }
    Command* c = add(OP_CONSTANTS);
    if (!c) return;
    c->kind = (uint8_t)kind; c->a = start; c->b = count;
    store(*c, data, (kind == VS_BOOL || kind == PS_BOOL) ? count * sizeof(BOOL) : count * 16);
}
void recordVertexDeclaration(IDirect3DVertexDeclaration9* d) {
    gState.declaration = d;
    if (Command* c = add(OP_DECLARATION)) keep(*c, d);
}
void recordFVF(DWORD fvf) {
    gState.fvf = fvf; gState.fvfSet = true;
    if (Command* c = add(OP_FVF)) c->a = fvf;
}
void recordStreamSource(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) {
    if (stream < STREAMS) {
        gState.stream[stream].buffer = buffer; gState.stream[stream].offset = offset; gState.stream[stream].stride = stride;
        gState.streamSet[stream] = true;
    }
    if (Command* c = add(OP_STREAM)) { c->a = stream; c->b = offset; c->c = stride; keep(*c, buffer); }
}
void recordStreamFrequency(UINT stream, UINT value) {
    if (stream < STREAMS) { gState.stream[stream].frequency = value; gState.frequencySet[stream] = true; }
    if (Command* c = add(OP_STREAM_FREQUENCY)) { c->a = stream; c->b = value; }
}
void recordIndices(IDirect3DIndexBuffer9* buffer) {
    gState.indices = buffer;
    if (Command* c = add(OP_INDICES)) keep(*c, buffer);
}
void recordDraw(D3DPRIMITIVETYPE type, UINT start, UINT count) {
    if (Command* c = add(OP_DRAW)) { c->a = type; c->b = start; c->c = count; }
}
void recordDrawIndexed(D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count) {
    if (Command* c = add(OP_DRAW_INDEXED)) {
        c->a = type; c->b = (uint32_t)base; c->c = minIndex; c->d = vertices; c->e = start; c->f = count;
    }
}
void recordDrawUP(D3DPRIMITIVETYPE type, UINT count, const void* vertices, UINT stride) {
    if (Command* c = add(OP_DRAW_UP)) {
        c->a = type; c->b = count; c->c = stride;
        store(*c, vertices, (size_t)verticesFor(type, count) * stride);
    }
    gState.stream[0] = Stream{}; gState.indices = nullptr;   // DrawPrimitiveUP unbinds both
}
void recordDrawIndexedUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT count, const void* indices,
                         D3DFORMAT format, const void* vertexData, UINT stride) {
    if (Command* c = add(OP_DRAW_INDEXED_UP)) {
        c->a = type; c->b = minIndex; c->c = vertices; c->d = count; c->e = format; c->f = stride;
        size_t indexBytes = (size_t)verticesFor(type, count) * (format == D3DFMT_INDEX32 ? 4 : 2);
        size_t vertexBytes = (size_t)(minIndex + vertices) * stride;
        c->data = (uint32_t)gCurrent.bytes.size(); c->length = (uint32_t)(indexBytes + vertexBytes);
        gCurrent.bytes.insert(gCurrent.bytes.end(), (const uint8_t*)indices, (const uint8_t*)indices + indexBytes);
        gCurrent.bytes.insert(gCurrent.bytes.end(), (const uint8_t*)vertexData, (const uint8_t*)vertexData + vertexBytes);
    }
    gState.stream[0] = Stream{}; gState.indices = nullptr;
}
void recordScene(bool begin) {
    add(begin ? OP_BEGIN_SCENE : OP_END_SCENE);
}
void recordStretchRect(IDirect3DSurface9* from, const RECT* fromRect, IDirect3DSurface9* to, const RECT* toRect,
                       D3DTEXTUREFILTERTYPE filter) {
    Command* c = add(OP_STRETCH_RECT);
    if (!c) return;
    keep(*c, from, to); c->a = filter; c->b = fromRect != nullptr; c->c = toRect != nullptr;
    RECT r[2] = {};
    if (fromRect) r[0] = *fromRect;
    if (toRect) r[1] = *toRect;
    store(*c, r, sizeof r);
}
void recordColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color) {
    Command* c = add(OP_COLOR_FILL);
    if (!c) return;
    keep(*c, surface); c->a = color; c->b = rect != nullptr;
    RECT r = rect ? *rect : RECT{};
    store(*c, &r, sizeof r);
}
void recordUpdateSurface(IDirect3DSurface9* from, const RECT* fromRect, IDirect3DSurface9* to, const POINT* toPoint) {
    Command* c = add(OP_UPDATE_SURFACE);
    if (!c) return;
    keep(*c, from, to); c->b = fromRect != nullptr; c->c = toPoint != nullptr;
    struct { RECT r; POINT p; } x = {};
    if (fromRect) x.r = *fromRect;
    if (toPoint) x.p = *toPoint;
    store(*c, &x, sizeof x);
}
void recordUpdateTexture(IDirect3DBaseTexture9* from, IDirect3DBaseTexture9* to) {
    if (Command* c = add(OP_UPDATE_TEXTURE)) keep(*c, from, to);
}
void recordBufferWrite(bool index, void* buffer, UINT offset, UINT size, DWORD flags, const void* data) {
    Command* c = add(index ? OP_INDEX_WRITE : OP_VERTEX_WRITE);
    if (!c) return;
    keep(*c, buffer); c->a = offset; c->b = size; c->c = flags;
    store(*c, data, size);
}

// ---------------------------------------------------------------- replay

void applyState(IDirect3DDevice9Ex* d, const DeviceState& s) {
    for (DWORD i = 0; i < 4; i++)
        if (s.renderTarget[i] || i > 0) R.SetRenderTarget(d, i, s.renderTarget[i]);   // target 0 can't be null
    R.SetDepthStencilSurface(d, s.depthStencil);
    if (s.viewportSet) R.SetViewport(d, &s.viewport);
    if (s.scissorSet) R.SetScissorRect(d, &s.scissor);
    for (int i = 0; i < RENDER_STATES; i++)
        if (s.renderStateSet[i]) R.SetRenderState(d, (D3DRENDERSTATETYPE)i, s.renderState[i]);
    for (int stage = 0; stage < 8; stage++)
        for (int t = 0; t < STAGE_STATES; t++)
            if (s.stageStateSet[stage][t]) R.SetTextureStageState(d, stage, (D3DTEXTURESTAGESTATETYPE)t, s.stageState[stage][t]);
    for (int slot = 0; slot < TEXTURE_SLOTS; slot++) {
        for (int t = 0; t < SAMPLER_STATES; t++)
            if (s.samplerStateSet[slot][t]) R.SetSamplerState(d, samplerOf(slot), (D3DSAMPLERSTATETYPE)t, s.samplerState[slot][t]);
        R.SetTexture(d, samplerOf(slot), s.texture[slot]);
    }
    R.SetVertexShader(d, s.vertexShader);
    R.SetPixelShader(d, s.pixelShader);
    for (int r = 0; r < VS_FLOAT_REGISTERS; r++) if (s.vsFloatSet[r]) R.SetVertexShaderConstantF(d, r, s.vsFloat[r], 1);
    for (int r = 0; r < PS_FLOAT_REGISTERS; r++) if (s.psFloatSet[r]) R.SetPixelShaderConstantF(d, r, s.psFloat[r], 1);
    for (int r = 0; r < INT_BOOL_REGISTERS; r++) {
        if (s.vsIntSet[r]) R.SetVertexShaderConstantI(d, r, s.vsInt[r], 1);
        if (s.psIntSet[r]) R.SetPixelShaderConstantI(d, r, s.psInt[r], 1);
        if (s.vsBoolSet[r]) R.SetVertexShaderConstantB(d, r, &s.vsBool[r], 1);
        if (s.psBoolSet[r]) R.SetPixelShaderConstantB(d, r, &s.psBool[r], 1);
    }
    if (s.declaration) R.SetVertexDeclaration(d, s.declaration);
    else if (s.fvfSet) R.SetFVF(d, s.fvf);
    for (int i = 0; i < STREAMS; i++) {
        if (s.streamSet[i]) R.SetStreamSource(d, i, s.stream[i].buffer, s.stream[i].offset, s.stream[i].stride);
        if (s.frequencySet[i]) R.SetStreamSourceFreq(d, i, s.stream[i].frequency);
    }
    R.SetIndices(d, s.indices);
    for (int i = 0; i < CLIP_PLANES; i++) if (s.clipPlaneSet[i]) R.SetClipPlane(d, i, s.clipPlane[i]);
    for (int i = 0; i < TRANSFORMS; i++) if (s.transformSet[i]) R.SetTransform(d, (D3DTRANSFORMSTATETYPE)i, &s.transform[i]);
}

// Writes a recorded buffer write again, with the game's own DISCARD/NOOVERWRITE flags.
static void rewrite(void* buffer, const Command& c, const uint8_t* data) {
    void* p = nullptr;
    if (SUCCEEDED(realLock(buffer, c.a, c.b, &p, c.c & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))) && p) {
        memcpy(p, data, c.b);
        realUnlock(buffer);
    }
}

void replay(IDirect3DDevice9Ex* d, const Frame& frame, ReplayHooks* hooks) {
    static ReplayHooks none;
    ReplayHooks* h = hooks ? hooks : &none;
    applyState(d, *frame.start);
    const uint8_t* bytes = frame.bytes.data();
    for (size_t i = 0; i < frame.commands.size(); i++) {
        const Command& c = frame.commands[i];
        const uint8_t* data = bytes + c.data;
        switch (c.op) {
        case OP_RENDER_TARGET: R.SetRenderTarget(d, c.a, (IDirect3DSurface9*)c.p); break;
        case OP_DEPTH_STENCIL: R.SetDepthStencilSurface(d, (IDirect3DSurface9*)c.p); break;
        case OP_CLEAR: {
            float z; memcpy(&z, &c.d, 4);
            R.Clear(d, c.a, c.a ? (const D3DRECT*)data : nullptr, c.b, c.c, z, c.e);
            break;
        }
        case OP_VIEWPORT: R.SetViewport(d, (const D3DVIEWPORT9*)data); break;
        case OP_SCISSOR: R.SetScissorRect(d, (const RECT*)data); break;
        case OP_TRANSFORM: R.SetTransform(d, (D3DTRANSFORMSTATETYPE)c.a, (const D3DMATRIX*)data); break;
        case OP_CLIP_PLANE: R.SetClipPlane(d, c.a, (const float*)data); break;
        case OP_MATERIAL: R.SetMaterial(d, (const D3DMATERIAL9*)data); break;
        case OP_LIGHT: R.SetLight(d, c.a, (const D3DLIGHT9*)data); break;
        case OP_LIGHT_ENABLE: R.LightEnable(d, c.a, c.b); break;
        case OP_RENDER_STATE: R.SetRenderState(d, (D3DRENDERSTATETYPE)c.a, c.b); break;
        case OP_TEXTURE: R.SetTexture(d, c.a, (IDirect3DBaseTexture9*)c.p); break;
        case OP_STAGE_STATE: R.SetTextureStageState(d, c.a, (D3DTEXTURESTAGESTATETYPE)c.b, c.c); break;
        case OP_SAMPLER_STATE: R.SetSamplerState(d, c.a, (D3DSAMPLERSTATETYPE)c.b, c.c); break;
        case OP_VERTEX_SHADER: R.SetVertexShader(d, (IDirect3DVertexShader9*)c.p); break;
        case OP_PIXEL_SHADER: R.SetPixelShader(d, (IDirect3DPixelShader9*)c.p); break;
        case OP_CONSTANTS:
            switch (c.kind) {
            case VS_FLOAT: R.SetVertexShaderConstantF(d, c.a, (const float*)data, c.b); break;
            case VS_INT: R.SetVertexShaderConstantI(d, c.a, (const int*)data, c.b); break;
            case VS_BOOL: R.SetVertexShaderConstantB(d, c.a, (const BOOL*)data, c.b); break;
            case PS_FLOAT: R.SetPixelShaderConstantF(d, c.a, (const float*)data, c.b); break;
            case PS_INT: R.SetPixelShaderConstantI(d, c.a, (const int*)data, c.b); break;
            case PS_BOOL: R.SetPixelShaderConstantB(d, c.a, (const BOOL*)data, c.b); break;
            }
            break;
        case OP_DECLARATION: R.SetVertexDeclaration(d, (IDirect3DVertexDeclaration9*)c.p); break;
        case OP_FVF: R.SetFVF(d, c.a); break;
        case OP_STREAM: R.SetStreamSource(d, c.a, (IDirect3DVertexBuffer9*)c.p, c.b, c.c); break;
        case OP_STREAM_FREQUENCY: R.SetStreamSourceFreq(d, c.a, c.b); break;
        case OP_INDICES: R.SetIndices(d, (IDirect3DIndexBuffer9*)c.p); break;
        case OP_DRAW:
            h->beforeDraw(d, i);
            R.DrawPrimitive(d, (D3DPRIMITIVETYPE)c.a, c.b, c.c);
            h->afterDraw(d, i);
            break;
        case OP_DRAW_INDEXED:
            h->beforeDraw(d, i);
            R.DrawIndexedPrimitive(d, (D3DPRIMITIVETYPE)c.a, (INT)c.b, c.c, c.d, c.e, c.f);
            h->afterDraw(d, i);
            break;
        case OP_DRAW_UP: R.DrawPrimitiveUP(d, (D3DPRIMITIVETYPE)c.a, c.b, data, c.c); break;
        case OP_DRAW_INDEXED_UP: {
            size_t indexBytes = (size_t)verticesFor((D3DPRIMITIVETYPE)c.a, c.d) * (c.e == D3DFMT_INDEX32 ? 4 : 2);
            R.DrawIndexedPrimitiveUP(d, (D3DPRIMITIVETYPE)c.a, c.b, c.c, c.d, data, (D3DFORMAT)c.e, data + indexBytes, c.f);
            break;
        }
        case OP_BEGIN_SCENE: R.BeginScene(d); break;
        case OP_END_SCENE: R.EndScene(d); break;
        case OP_STRETCH_RECT: {
            const RECT* r = (const RECT*)data;
            R.StretchRect(d, (IDirect3DSurface9*)c.p, c.b ? &r[0] : nullptr, (IDirect3DSurface9*)c.q, c.c ? &r[1] : nullptr,
                          (D3DTEXTUREFILTERTYPE)c.a);
            break;
        }
        case OP_COLOR_FILL: R.ColorFill(d, (IDirect3DSurface9*)c.p, c.b ? (const RECT*)data : nullptr, c.a); break;
        case OP_UPDATE_SURFACE: {
            const RECT* r = (const RECT*)data;
            const POINT* p = (const POINT*)(data + sizeof(RECT));
            R.UpdateSurface(d, (IDirect3DSurface9*)c.p, c.b ? r : nullptr, (IDirect3DSurface9*)c.q, c.c ? p : nullptr);
            break;
        }
        case OP_UPDATE_TEXTURE: R.UpdateTexture(d, (IDirect3DBaseTexture9*)c.p, (IDirect3DBaseTexture9*)c.q); break;
        case OP_VERTEX_WRITE: rewrite(c.p, c, h->bufferData(i, c, data)); break;
        case OP_INDEX_WRITE: rewrite(c.p, c, data); break;
        }
    }
}
