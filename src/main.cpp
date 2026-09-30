// TSFix+: entry point and Direct3D 9 hooks.
//
// TSFix+ is the game's d3d9.dll: the game loads it from its own folder instead of Windows'
// Direct3D 9. It loads the real Direct3D 9 (dgVoodoo.dll next to it if present, otherwise the
// system's d3d9.dll), hands the game the real device, and replaces some of the device's methods
// (vtable entries) with the hooks below. Each hook tells recorder.cpp about the call and passes
// it on. Present goes to interpolate.cpp, which shows the in-between frames. (It also still works
// as Special K's "d3d9 proxy" with TSFix, as in version 0.9.)
#include "common.h"
#include <cstdarg>
#include <cstddef>
#include <cstring>

RealDevice R;
CRITICAL_SECTION gLock;
std::unordered_map<void*, BufferInfo> gBuffers;
std::unordered_map<void*, std::vector<uint8_t>> gIndexCopies;

static std::string gDir;   // the folder tsfixplus.dll is in
static FILE* gLog;

// ---------------------------------------------------------------- log

void log(const char* fmt, ...) {
    if (!gLog) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(gLog, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(gLog, fmt, a); va_end(a);
    fputc('\n', gLog); fflush(gLog);
}

// ---------------------------------------------------------------- shaders

static std::unordered_map<void*, uint32_t> gShaderFlags;

uint32_t shaderFlags(void* shader) {
    auto it = gShaderFlags.find(shader);
    return it == gShaderFlags.end() ? 0 : it->second;
}

static bool containsText(const uint8_t* bytes, size_t length, const char* text) {
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= length; i++)
        if (!memcmp(bytes + i, text, n)) return true;
    return false;
}

// The game's shaders keep their constant table, names included. A vertex shader that places an
// object in the scene has mMatrixWVP or mMatrixWV in c0-c11 (the main pass: WVP, WV and W; the
// depth pass: WVP; the outline pass: P and WV); gCBuffer1 in c16-c18 holds light vectors or the
// outline width.
static void noteVertexShader(void* shader, const DWORD* code) {
    size_t words = 1;
    while (code[words - 1] != 0x0000FFFF && words < (1u << 20)) words++;   // the end token
    const uint8_t* bytes = (const uint8_t*)code;
    uint32_t flags = 0;
    if (containsText(bytes, words * 4, "mMatrixWV")) flags |= SHADER_PLACES_OBJECT;
    if (containsText(bytes, words * 4, "gCBuffer1")) flags |= SHADER_HAS_LIGHTS;
    gShaderFlags[shader] = flags;
}

// ---------------------------------------------------------------- vtable patching

template <class T> static void patch(T* slot, T hook, T* original) {
    if (*slot == hook) return;   // already hooked: another object of the same class
    DWORD old;
    VirtualProtect(slot, sizeof(T), PAGE_EXECUTE_READWRITE, &old);
    *original = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(T), old, &old);
}

// ---------------------------------------------------------------- vertex and index buffers

// The game writes a buffer with Lock ... Unlock. For dynamic buffers (rewritten during frames)
// the written bytes are recorded; for index buffers they are also copied into gIndexCopies.

struct PendingWrite { void* data; UINT offset, size; DWORD flags; };
static std::unordered_map<void*, PendingWrite> gPendingWrites;

static void noteLock(void* buffer, UINT offset, UINT size, void* data, DWORD flags) {
    Locked lock;
    auto it = gBuffers.find(buffer);
    if (it == gBuffers.end() || (flags & D3DLOCK_READONLY)) return;
    if (!(it->second.usage & D3DUSAGE_DYNAMIC) && !it->second.index) return;
    if (!size) size = it->second.length - offset;
    gPendingWrites[buffer] = {data, offset, size, flags};
}

static void noteUnlock(void* buffer, bool index) {
    Locked lock;
    auto it = gPendingWrites.find(buffer);
    if (it == gPendingWrites.end()) return;
    PendingWrite w = it->second;
    gPendingWrites.erase(it);
    auto info = gBuffers.find(buffer);
    if (info == gBuffers.end()) return;
    if (index) {
        std::vector<uint8_t>& copy = gIndexCopies[buffer];
        if (copy.size() < info->second.length) copy.resize(info->second.length);
        if ((uint64_t)w.offset + w.size <= copy.size()) memcpy(copy.data() + w.offset, w.data, w.size);
    }
    if (info->second.usage & D3DUSAGE_DYNAMIC) recordBufferWrite(index, buffer, w.offset, w.size, w.flags, w.data);
}

// Vertex and index buffers have Lock and Unlock at the same vtable slots with the same
// arguments, so one pair of hooks serves both. Buffers can come from more than one class, so
// the original methods are kept per vtable.
typedef HRESULT(STDMETHODCALLTYPE* LockFn)(void*, UINT, UINT, void**, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* UnlockFn)(void*);
struct BufferClass { LockFn lock; UnlockFn unlock; bool index; };
static std::unordered_map<const void*, BufferClass> gBufferClasses;   // vtable -> originals

static BufferClass classOf(void* buffer) {
    Locked lock;
    return gBufferClasses.at(*(void**)buffer);
}
HRESULT realLock(void* buffer, UINT offset, UINT size, void** data, DWORD flags) {
    return classOf(buffer).lock(buffer, offset, size, data, flags);
}
HRESULT realUnlock(void* buffer) { return classOf(buffer).unlock(buffer); }

static HRESULT STDMETHODCALLTYPE hookLock(void* buffer, UINT offset, UINT size, void** data, DWORD flags) {
    HRESULT hr = realLock(buffer, offset, size, data, flags);
    if (SUCCEEDED(hr) && data && *data) noteLock(buffer, offset, size, *data, flags);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hookUnlock(void* buffer) {
    noteUnlock(buffer, classOf(buffer).index);
    return realUnlock(buffer);
}

static void hookBufferClass(void* buffer, bool index) {   // with gLock held
    void** vtable = *(void***)buffer;
    if (gBufferClasses.count(vtable)) return;
    static_assert(offsetof(IDirect3DVertexBuffer9Vtbl, Lock) == offsetof(IDirect3DIndexBuffer9Vtbl, Lock), "Lock slot");
    static_assert(offsetof(IDirect3DVertexBuffer9Vtbl, Unlock) == offsetof(IDirect3DIndexBuffer9Vtbl, Unlock), "Unlock slot");
    BufferClass c{};
    c.index = index;
    patch((LockFn*)((uint8_t*)vtable + offsetof(IDirect3DVertexBuffer9Vtbl, Lock)), (LockFn)hookLock, &c.lock);
    patch((UnlockFn*)((uint8_t*)vtable + offsetof(IDirect3DVertexBuffer9Vtbl, Unlock)), (UnlockFn)hookUnlock, &c.unlock);
    gBufferClasses[vtable] = c;
}

// ---------------------------------------------------------------- device hooks

typedef IDirect3DDevice9Ex Device;
#define HOOK static HRESULT STDMETHODCALLTYPE

HOOK hookPresent(Device* d, const RECT* from, const RECT* to, HWND window, const RGNDATA* region) {
    { Locked lock; standaloneFrame((IDirect3DDevice9*)d); endRecordedFrame(); }
    return presentFrame(d, [&] { return R.Present(d, from, to, window, region); });
}
HOOK hookPresentEx(Device* d, const RECT* from, const RECT* to, HWND window, const RGNDATA* region, DWORD flags) {
    { Locked lock; standaloneFrame((IDirect3DDevice9*)d); endRecordedFrame(); }
    return presentFrame(d, [&] { return R.PresentEx(d, from, to, window, region, flags); });
}
HOOK hookReset(Device* d, D3DPRESENT_PARAMETERS* p) {
    { Locked lock; resetRecorder(); resetInterpolation(); }   // a Reset needs our references gone
    bool fullscreen = standaloneDeviceParams(p, nullptr);
    HRESULT hr = R.Reset(d, p);
    standaloneDeviceCreated(p, fullscreen, hr);
    return hr;
}
HOOK hookResetEx(Device* d, D3DPRESENT_PARAMETERS* p, D3DDISPLAYMODEEX* mode) {
    { Locked lock; resetRecorder(); resetInterpolation(); }
    bool fullscreen = standaloneDeviceParams(p, nullptr);
    HRESULT hr = R.ResetEx(d, p, fullscreen ? nullptr : mode);   // a window has no display mode
    standaloneDeviceCreated(p, fullscreen, hr);
    return hr;
}
HOOK hookCreateVertexShader(Device* d, const DWORD* code, IDirect3DVertexShader9** out) {
    HRESULT hr = R.CreateVertexShader(d, code, out);
    if (SUCCEEDED(hr) && out && *out && code) { Locked lock; noteVertexShader(*out, code); }
    return hr;
}
HOOK hookCreateVertexBuffer(Device* d, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared) {
    HRESULT hr = R.CreateVertexBuffer(d, length, usage, fvf, pool, out, shared);
    if (SUCCEEDED(hr) && out && *out) {
        Locked lock;
        gBuffers[*out] = {length, usage};
        hookBufferClass(*out, false);
    }
    return hr;
}
HOOK hookCreateIndexBuffer(Device* d, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared) {
    HRESULT hr = R.CreateIndexBuffer(d, length, usage, format, pool, out, shared);
    if (SUCCEEDED(hr) && out && *out) {
        Locked lock;
        gBuffers[*out] = {length, usage, true, format};
        gIndexCopies.erase(*out);   // a new buffer at a freed one's address
        hookBufferClass(*out, true);
    }
    return hr;
}

// Hooks that record the call, then pass it on.
#define RECORDING_HOOK(name, params, record, args)                                                  \
    HOOK hook##name params {                                                                          \
        { Locked lock; record; }                                                                      \
        return R.name args;                                                                           \
    }

RECORDING_HOOK(SetRenderTarget, (Device* d, DWORD i, IDirect3DSurface9* s), recordRenderTarget(i, s), (d, i, s))
RECORDING_HOOK(SetDepthStencilSurface, (Device* d, IDirect3DSurface9* s), recordDepthStencil(s), (d, s))
RECORDING_HOOK(Clear, (Device* d, DWORD n, const D3DRECT* r, DWORD f, D3DCOLOR c, float z, DWORD s),
               recordClear(n, r, f, c, z, s), (d, n, r, f, c, z, s))
RECORDING_HOOK(SetViewport, (Device* d, const D3DVIEWPORT9* v), recordViewport(v), (d, v))
RECORDING_HOOK(SetTransform, (Device* d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m), recordTransform(t, m), (d, t, m))
RECORDING_HOOK(SetClipPlane, (Device* d, DWORD i, const float* p), recordClipPlane(i, p), (d, i, p))
RECORDING_HOOK(SetMaterial, (Device* d, const D3DMATERIAL9* m), recordMaterial(m), (d, m))
RECORDING_HOOK(SetLight, (Device* d, DWORD i, const D3DLIGHT9* l), recordLight(i, l), (d, i, l))
RECORDING_HOOK(LightEnable, (Device* d, DWORD i, BOOL on), recordLightEnable(i, on), (d, i, on))
RECORDING_HOOK(SetRenderState, (Device* d, D3DRENDERSTATETYPE s, DWORD v), recordRenderState(s, v), (d, s, v))
// The recorder keeps the game's own texture; a texture pack's replacement is swapped in only
// where a texture is really set (here and in the replay).
HOOK hookSetTexture(Device* d, DWORD stage, IDirect3DBaseTexture9* t) {
    { Locked lock; recordTexture(stage, t); }
    return R.SetTexture(d, stage, textureReplacement(t));
}
RECORDING_HOOK(SetTextureStageState, (Device* d, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v),
               recordTextureStageState(stage, t, v), (d, stage, t, v))
RECORDING_HOOK(SetSamplerState, (Device* d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v), recordSamplerState(s, t, v), (d, s, t, v))
RECORDING_HOOK(SetScissorRect, (Device* d, const RECT* r), recordScissor(r), (d, r))
RECORDING_HOOK(SetVertexShader, (Device* d, IDirect3DVertexShader9* s), recordVertexShader(s), (d, s))
RECORDING_HOOK(SetPixelShader, (Device* d, IDirect3DPixelShader9* s), recordPixelShader(s), (d, s))
RECORDING_HOOK(SetVertexShaderConstantF, (Device* d, UINT r, const float* v, UINT n), recordConstants(VS_FLOAT, r, v, n), (d, r, v, n))
RECORDING_HOOK(SetVertexShaderConstantI, (Device* d, UINT r, const int* v, UINT n), recordConstants(VS_INT, r, v, n), (d, r, v, n))
RECORDING_HOOK(SetVertexShaderConstantB, (Device* d, UINT r, const BOOL* v, UINT n), recordConstants(VS_BOOL, r, v, n), (d, r, v, n))
RECORDING_HOOK(SetPixelShaderConstantF, (Device* d, UINT r, const float* v, UINT n), recordConstants(PS_FLOAT, r, v, n), (d, r, v, n))
RECORDING_HOOK(SetPixelShaderConstantI, (Device* d, UINT r, const int* v, UINT n), recordConstants(PS_INT, r, v, n), (d, r, v, n))
RECORDING_HOOK(SetPixelShaderConstantB, (Device* d, UINT r, const BOOL* v, UINT n), recordConstants(PS_BOOL, r, v, n), (d, r, v, n))
RECORDING_HOOK(SetVertexDeclaration, (Device* d, IDirect3DVertexDeclaration9* v), recordVertexDeclaration(v), (d, v))
RECORDING_HOOK(SetFVF, (Device* d, DWORD f), recordFVF(f), (d, f))
RECORDING_HOOK(SetStreamSource, (Device* d, UINT s, IDirect3DVertexBuffer9* b, UINT o, UINT st), recordStreamSource(s, b, o, st), (d, s, b, o, st))
RECORDING_HOOK(SetStreamSourceFreq, (Device* d, UINT s, UINT v), recordStreamFrequency(s, v), (d, s, v))
RECORDING_HOOK(SetIndices, (Device* d, IDirect3DIndexBuffer9* b), recordIndices(b), (d, b))
RECORDING_HOOK(DrawPrimitive, (Device* d, D3DPRIMITIVETYPE t, UINT s, UINT c), recordDraw(t, s, c), (d, t, s, c))
RECORDING_HOOK(DrawIndexedPrimitive, (Device* d, D3DPRIMITIVETYPE t, INT b, UINT m, UINT n, UINT s, UINT c),
               recordDrawIndexed(t, b, m, n, s, c), (d, t, b, m, n, s, c))
RECORDING_HOOK(DrawPrimitiveUP, (Device* d, D3DPRIMITIVETYPE t, UINT c, const void* v, UINT s), recordDrawUP(t, c, v, s), (d, t, c, v, s))
RECORDING_HOOK(DrawIndexedPrimitiveUP,
               (Device* d, D3DPRIMITIVETYPE t, UINT m, UINT n, UINT c, const void* i, D3DFORMAT f, const void* v, UINT s),
               recordDrawIndexedUP(t, m, n, c, i, f, v, s), (d, t, m, n, c, i, f, v, s))
RECORDING_HOOK(BeginScene, (Device* d), recordScene(true), (d))
RECORDING_HOOK(EndScene, (Device* d), recordScene(false), (d))
// Copies made by the texture loader's thread (while it creates replacements) aren't the game's:
// they're passed on without being recorded.
#define COPY_HOOK(name, params, record, args)                                                        \
    HOOK hook##name params {                                                                          \
        if (!textureWorkerThread()) { Locked lock; record; }                                          \
        return R.name args;                                                                           \
    }
COPY_HOOK(StretchRect, (Device* d, IDirect3DSurface9* a, const RECT* ra, IDirect3DSurface9* b, const RECT* rb, D3DTEXTUREFILTERTYPE f),
          recordStretchRect(a, ra, b, rb, f), (d, a, ra, b, rb, f))
COPY_HOOK(ColorFill, (Device* d, IDirect3DSurface9* s, const RECT* r, D3DCOLOR c), recordColorFill(s, r, c), (d, s, r, c))
COPY_HOOK(UpdateSurface, (Device* d, IDirect3DSurface9* a, const RECT* ra, IDirect3DSurface9* b, const POINT* pb),
          recordUpdateSurface(a, ra, b, pb), (d, a, ra, b, pb))
COPY_HOOK(UpdateTexture, (Device* d, IDirect3DBaseTexture9* a, IDirect3DBaseTexture9* b), recordUpdateTexture(a, b), (d, a, b))

static void hookDevice(Device* device, bool ex) {
    IDirect3DDevice9ExVtbl* v = device->lpVtbl;
#define HOOK_METHOD(name) patch(&v->name, hook##name, &R.name);
    HOOK_METHOD(Present) HOOK_METHOD(Reset)
    if (ex) { HOOK_METHOD(PresentEx) HOOK_METHOD(ResetEx) }
    HOOK_METHOD(CreateVertexShader) HOOK_METHOD(CreateVertexBuffer) HOOK_METHOD(CreateIndexBuffer)
    HOOK_METHOD(SetRenderTarget) HOOK_METHOD(SetDepthStencilSurface) HOOK_METHOD(Clear) HOOK_METHOD(SetViewport)
    HOOK_METHOD(SetTransform) HOOK_METHOD(SetClipPlane) HOOK_METHOD(SetMaterial) HOOK_METHOD(SetLight) HOOK_METHOD(LightEnable)
    HOOK_METHOD(SetRenderState) HOOK_METHOD(SetTexture) HOOK_METHOD(SetTextureStageState) HOOK_METHOD(SetSamplerState)
    HOOK_METHOD(SetScissorRect) HOOK_METHOD(SetVertexShader) HOOK_METHOD(SetPixelShader)
    HOOK_METHOD(SetVertexShaderConstantF) HOOK_METHOD(SetVertexShaderConstantI) HOOK_METHOD(SetVertexShaderConstantB)
    HOOK_METHOD(SetPixelShaderConstantF) HOOK_METHOD(SetPixelShaderConstantI) HOOK_METHOD(SetPixelShaderConstantB)
    HOOK_METHOD(SetVertexDeclaration) HOOK_METHOD(SetFVF) HOOK_METHOD(SetStreamSource) HOOK_METHOD(SetStreamSourceFreq)
    HOOK_METHOD(SetIndices) HOOK_METHOD(DrawPrimitive) HOOK_METHOD(DrawIndexedPrimitive) HOOK_METHOD(DrawPrimitiveUP)
    HOOK_METHOD(DrawIndexedPrimitiveUP) HOOK_METHOD(BeginScene) HOOK_METHOD(EndScene) HOOK_METHOD(StretchRect)
    HOOK_METHOD(ColorFill) HOOK_METHOD(UpdateSurface) HOOK_METHOD(UpdateTexture)
#undef HOOK_METHOD
}

// ---------------------------------------------------------------- IDirect3D9 hooks

static decltype(IDirect3D9ExVtbl::CreateDevice) gCreateDevice;
static decltype(IDirect3D9ExVtbl::CreateDeviceEx) gCreateDeviceEx;

static HRESULT STDMETHODCALLTYPE hookCreateDevice(IDirect3D9Ex* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                                  D3DPRESENT_PARAMETERS* p, IDirect3DDevice9** out) {
    bool fullscreen = standaloneDeviceParams(p, window);
    HRESULT hr = gCreateDevice(d3d, adapter, type, window, flags, p, out);
    standaloneDeviceCreated(p, fullscreen, hr);
    if (SUCCEEDED(hr) && out && *out) {
        Locked lock;
        resetRecorder(); resetInterpolation();
        hookDevice((Device*)*out, false);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE hookCreateDeviceEx(IDirect3D9Ex* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                                    D3DPRESENT_PARAMETERS* p, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out) {
    bool fullscreen = standaloneDeviceParams(p, window);
    HRESULT hr = gCreateDeviceEx(d3d, adapter, type, window, flags, p, fullscreen ? nullptr : mode, out);
    standaloneDeviceCreated(p, fullscreen, hr);
    if (SUCCEEDED(hr) && out && *out) {
        Locked lock;
        resetRecorder(); resetInterpolation();
        hookDevice(*out, true);
    }
    return hr;
}

// ---------------------------------------------------------------- start-up

static HMODULE gRealD3D9;

static std::string moduleDir() {
    char path[MAX_PATH]; HMODULE self;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&moduleDir, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path);
    return s.substr(0, s.find_last_of("\\/"));
}

// With TSFix (version 0.9's setup): TSFix+ paces the game itself, so TSFix's own frame limiter
// must be well above 30. At 60,
// its ticks drift against TSFix+'s schedule and hold a frame back about every 20 game frames
// (a small hitch); at 30, the game can't keep time. Warn in the log.
static void checkTsfix() {
    std::string ini = gDir + "\\tsfix.ini";
    if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { log("tsfix.ini not found next to the game"); return; }
    char value[32];
    GetPrivateProfileStringA("TSFix.Window", "ForegroundFPS", "", value, sizeof value, ini.c_str());
    if (value[0] && atof(value) <= 60.0)
        log("WARNING: tsfix.ini has ForegroundFPS=%s; set ForegroundFPS and BackgroundFPS to 1000 (see README)", value);
}

static bool start() {
    static bool started = false;
    if (started) return gRealD3D9 != nullptr;
    started = true;
    InitializeCriticalSection(&gLock);
    gDir = moduleDir();
    gLog = fopen((gDir + "\\tsfixplus.log").c_str(), "w");
    log("TSFix+ started");
    // On its own: the system's Direct3D 9. With TSFix (loaded by Special K, version 0.9's setup):
    // dgVoodoo if it is installed next to the game, as TSFix sets it up. (dgVoodoo's modern
    // swapchain also brings in Windows' Auto HDR, which broke up the videos.)
    std::string dgVoodoo = gDir + "\\dgVoodoo.dll";
    if (GetModuleHandleA("tsfix.dll") && GetFileAttributesA(dgVoodoo.c_str()) != INVALID_FILE_ATTRIBUTES)
        gRealD3D9 = LoadLibraryA(dgVoodoo.c_str());
    if (!gRealD3D9) {
        char system[MAX_PATH];
        GetSystemDirectoryA(system, MAX_PATH);
        gRealD3D9 = LoadLibraryA((std::string(system) + "\\d3d9.dll").c_str());
    }
    char loaded[MAX_PATH] = "";
    if (gRealD3D9) GetModuleFileNameA(gRealD3D9, loaded, MAX_PATH);
    log("Direct3D 9: %s", gRealD3D9 ? loaded : "could not be loaded");
    std::string ini = gDir + "\\tsfixplus.ini";
    configureInterpolation(ini.c_str());
    standaloneStart();
    if (gStandalone) texturesStart(GetPrivateProfileIntA("TSFixPlus", "TexturePacks", 1, ini.c_str()) != 0);
    else checkTsfix();
    return gRealD3D9 != nullptr;
}

static FARPROC real(const char* name) { return gRealD3D9 ? GetProcAddress(gRealD3D9, name) : nullptr; }

// ---------------------------------------------------------------- exports (see tsfixplus.def)

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    if (!start()) return nullptr;
    auto create = (IDirect3D9*(WINAPI*)(UINT))real("Direct3DCreate9");
    IDirect3D9* d3d = create ? create(sdk) : nullptr;
    if (d3d) patch(&((IDirect3D9Ex*)d3d)->lpVtbl->CreateDevice, hookCreateDevice, &gCreateDevice);
    return d3d;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    if (!start()) return E_FAIL;
    auto create = (HRESULT(WINAPI*)(UINT, IDirect3D9Ex**))real("Direct3DCreate9Ex");
    if (!create) return E_NOTIMPL;
    HRESULT hr = create(sdk, out);
    if (SUCCEEDED(hr) && out && *out) {
        patch(&(*out)->lpVtbl->CreateDevice, hookCreateDevice, &gCreateDevice);
        patch(&(*out)->lpVtbl->CreateDeviceEx, hookCreateDeviceEx, &gCreateDeviceEx);
    }
    return hr;
}

// Debugging markers (PIX): passed to the real Direct3D 9.
extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    auto f = (int(WINAPI*)(D3DCOLOR, LPCWSTR))real("D3DPERF_BeginEvent");
    return f ? f(color, name) : 0;
}
extern "C" int WINAPI D3DPERF_EndEvent() {
    auto f = (int(WINAPI*)())real("D3DPERF_EndEvent");
    return f ? f() : 0;
}
extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    if (auto f = (void(WINAPI*)(D3DCOLOR, LPCWSTR))real("D3DPERF_SetMarker")) f(color, name);
}
extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    if (auto f = (void(WINAPI*)(D3DCOLOR, LPCWSTR))real("D3DPERF_SetRegion")) f(color, name);
}
extern "C" BOOL WINAPI D3DPERF_QueryRepeatFrame() {
    auto f = (BOOL(WINAPI*)())real("D3DPERF_QueryRepeatFrame");
    return f ? f() : FALSE;
}
extern "C" void WINAPI D3DPERF_SetOptions(DWORD options) {
    if (auto f = (void(WINAPI*)(DWORD))real("D3DPERF_SetOptions")) f(options);
}
extern "C" DWORD WINAPI D3DPERF_GetStatus() {
    auto f = (DWORD(WINAPI*)())real("D3DPERF_GetStatus");
    return f ? f() : 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
