// TSFix+: declarations shared by the source files.
//
//   main.cpp         loads the real Direct3D 9, hooks the device and passes every call on
//   recorder.cpp     records each game frame's drawing commands and can replay them
//   interpolate.cpp  draws the in-between frames and paces the game
//   standalone.cpp   the fixes the Steam version needs (what TSFix did), when TSFix isn't loaded
//   textures.cpp     TSFix-format texture packs, when TSFix isn't loaded
//   lzma/            the 7-Zip decoder (LZMA SDK, public domain), for the packs
//
// See docs/HOW-IT-WORKS.md for the design.
#pragma once

#define CINTERFACE          // C-style COM: device methods are reachable as vtable members
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------- main.cpp

void log(const char* fmt, ...);   // appends to tsfixplus.log next to the DLL

// One lock guards everything shared: the game creates its device as multithreaded and loads
// resources on other threads.
extern CRITICAL_SECTION gLock;
struct Locked {
    Locked() { EnterCriticalSection(&gLock); }
    ~Locked() { LeaveCriticalSection(&gLock); }
};

// The real (unhooked) device methods TSFix+ calls.
#define REAL_DEVICE_METHODS(X)                                                                      \
    X(Present) X(PresentEx) X(Reset) X(ResetEx) X(CreateVertexShader) X(CreateVertexBuffer)          \
    X(CreateIndexBuffer) X(SetRenderTarget) X(SetDepthStencilSurface) X(Clear) X(SetViewport)       \
    X(SetTransform) X(SetClipPlane) X(SetMaterial) X(SetLight) X(LightEnable) X(SetRenderState)      \
    X(SetTexture) X(SetTextureStageState) X(SetSamplerState) X(SetScissorRect) X(SetVertexShader)    \
    X(SetPixelShader) X(SetVertexShaderConstantF) X(SetVertexShaderConstantI)                      \
    X(SetVertexShaderConstantB) X(SetPixelShaderConstantF) X(SetPixelShaderConstantI)             \
    X(SetPixelShaderConstantB) X(SetVertexDeclaration) X(SetFVF) X(SetStreamSource)               \
    X(SetStreamSourceFreq) X(SetIndices) X(DrawPrimitive) X(DrawIndexedPrimitive)                 \
    X(DrawPrimitiveUP) X(DrawIndexedPrimitiveUP) X(BeginScene) X(EndScene) X(StretchRect)          \
    X(ColorFill) X(UpdateSurface) X(UpdateTexture)

struct RealDevice {
#define X(name) decltype(IDirect3DDevice9ExVtbl::name) name;
    REAL_DEVICE_METHODS(X)
#undef X
};
extern RealDevice R;

// Real Lock/Unlock of a vertex or index buffer.
HRESULT realLock(void* buffer, UINT offset, UINT size, void** data, DWORD flags);
HRESULT realUnlock(void* buffer);

// Every vertex and index buffer the game creates.
struct BufferInfo {
    UINT length;
    DWORD usage;
    bool index = false;
    D3DFORMAT format = D3DFMT_UNKNOWN;   // index buffers: 16- or 32-bit indices
};
extern std::unordered_map<void*, BufferInfo> gBuffers;

// A copy of every index buffer's contents, kept as the game writes it. A draw's index list says
// which vertices it really uses, and index buffers can't be read back from the GPU.
extern std::unordered_map<void*, std::vector<uint8_t>> gIndexCopies;

// What a vertex shader's constant table says about it (found by name when it is created).
enum {
    SHADER_PLACES_OBJECT = 1,   // has mMatrixWVP or mMatrixWV in c0-c11: draws something in 3D
    SHADER_HAS_LIGHTS = 2,      // has gCBuffer1 in c16-c18 (light vectors or outline width)
};
uint32_t shaderFlags(void* vertexShader);

// ---------------------------------------------------------------- recorder.cpp

enum ConstantKind { VS_FLOAT, VS_INT, VS_BOOL, PS_FLOAT, PS_INT, PS_BOOL };

// Called by the device hooks, with gLock held, before each call is passed on.
void recordRenderTarget(DWORD index, IDirect3DSurface9* surface);
void recordDepthStencil(IDirect3DSurface9* surface);
void recordClear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil);
void recordViewport(const D3DVIEWPORT9* viewport);
void recordTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix);
void recordClipPlane(DWORD index, const float* plane);
void recordMaterial(const D3DMATERIAL9* material);
void recordLight(DWORD index, const D3DLIGHT9* light);
void recordLightEnable(DWORD index, BOOL enable);
void recordRenderState(D3DRENDERSTATETYPE state, DWORD value);
void recordTexture(DWORD stage, IDirect3DBaseTexture9* texture);
void recordTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value);
void recordSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value);
void recordScissor(const RECT* rect);
void recordVertexShader(IDirect3DVertexShader9* shader);
void recordPixelShader(IDirect3DPixelShader9* shader);
void recordConstants(ConstantKind kind, UINT start, const void* data, UINT count);
void recordVertexDeclaration(IDirect3DVertexDeclaration9* declaration);
void recordFVF(DWORD fvf);
void recordStreamSource(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride);
void recordStreamFrequency(UINT stream, UINT value);
void recordIndices(IDirect3DIndexBuffer9* buffer);
void recordDraw(D3DPRIMITIVETYPE type, UINT start, UINT count);
void recordDrawIndexed(D3DPRIMITIVETYPE type, INT base, UINT minIndex, UINT vertices, UINT start, UINT count);
void recordDrawUP(D3DPRIMITIVETYPE type, UINT count, const void* vertices, UINT stride);
void recordDrawIndexedUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT vertices, UINT count, const void* indices,
                         D3DFORMAT format, const void* vertexData, UINT stride);
void recordScene(bool begin);
void recordStretchRect(IDirect3DSurface9* from, const RECT* fromRect, IDirect3DSurface9* to, const RECT* toRect,
                       D3DTEXTUREFILTERTYPE filter);
void recordColorFill(IDirect3DSurface9* surface, const RECT* rect, D3DCOLOR color);
void recordUpdateSurface(IDirect3DSurface9* from, const RECT* fromRect, IDirect3DSurface9* to, const POINT* toPoint);
void recordUpdateTexture(IDirect3DBaseTexture9* from, IDirect3DBaseTexture9* to);
void recordBufferWrite(bool index, void* buffer, UINT offset, UINT size, DWORD flags, const void* data);

void endRecordedFrame();   // at each Present: the frame being recorded becomes the last frame
void resetRecorder();      // before a device Reset or a new device

// ---------------------------------------------------------------- interpolate.cpp

void configureInterpolation(const char* iniPath);
void resetInterpolation();   // before a device Reset or a new device

// Shows the finished game frame: in-between frames up to the refresh rate, then returns when
// the game must start its next frame. `present` performs one real Present.
HRESULT presentFrame(IDirect3DDevice9Ex* device, HRESULT (*present)(void*), void* context);
template <class F> HRESULT presentFrame(IDirect3DDevice9Ex* device, F present) {
    return presentFrame(device, [](void* c) { return (*(F*)c)(); }, &present);
}
int blendedLastFrame();   // objects blended in the last game frame (0 while a video plays)

// ---------------------------------------------------------------- standalone.cpp

extern bool gStandalone;   // TSFix isn't loaded: TSFix+ provides the game fixes itself
void standaloneStart();    // at start-up: patches the game
// Around device creation and Reset: fullscreen becomes a borderless window, and the window is
// kept active. standaloneDeviceParams returns whether the game asked for fullscreen.
bool standaloneDeviceParams(D3DPRESENT_PARAMETERS* p, HWND focusWindow);
void standaloneDeviceCreated(D3DPRESENT_PARAMETERS* p, bool fullscreen, HRESULT hr);
void standaloneFrame(IDirect3DDevice9* device);   // at each game Present, with gLock held

// ---------------------------------------------------------------- textures.cpp

void texturesStart(bool enabled);
IDirect3DBaseTexture9* textureReplacement(IDirect3DBaseTexture9* texture);   // what to draw with instead
bool textureWorkerThread();   // the texture loader's own Direct3D calls, which aren't the game's
