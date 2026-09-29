// TSFix+: recorded frames, shared by recorder.cpp and interpolate.cpp.
#pragma once
#include "common.h"

enum {
    TEXTURE_SLOTS = 20,   // 16 pixel samplers + 4 vertex texture samplers
    RENDER_STATES = 256,
    STAGE_STATES = 33,
    SAMPLER_STATES = 14,
    VS_FLOAT_REGISTERS = 256,
    PS_FLOAT_REGISTERS = 224,
    INT_BOOL_REGISTERS = 16,
    CLIP_PLANES = 6,
    STREAMS = 16,
    TRANSFORMS = 512,
};

struct Stream { IDirect3DVertexBuffer9* buffer; UINT offset, stride, frequency; };

// Everything the game can set on the device, and which entries it has set. The game's device
// can't be asked for its state (it is created as a "pure" device), so TSFix+ keeps its own
// copy. Only entries the game has set are re-applied; the rest are still at their defaults.
struct DeviceState {
    IDirect3DSurface9* renderTarget[4]; IDirect3DSurface9* depthStencil;
    D3DVIEWPORT9 viewport; bool viewportSet;
    RECT scissor; bool scissorSet;
    DWORD renderState[RENDER_STATES]; bool renderStateSet[RENDER_STATES];
    DWORD stageState[8][STAGE_STATES]; bool stageStateSet[8][STAGE_STATES];
    DWORD samplerState[TEXTURE_SLOTS][SAMPLER_STATES]; bool samplerStateSet[TEXTURE_SLOTS][SAMPLER_STATES];
    IDirect3DBaseTexture9* texture[TEXTURE_SLOTS];
    IDirect3DVertexShader9* vertexShader; IDirect3DPixelShader9* pixelShader;
    float vsFloat[VS_FLOAT_REGISTERS][4]; bool vsFloatSet[VS_FLOAT_REGISTERS];
    int vsInt[INT_BOOL_REGISTERS][4]; bool vsIntSet[INT_BOOL_REGISTERS];
    BOOL vsBool[INT_BOOL_REGISTERS]; bool vsBoolSet[INT_BOOL_REGISTERS];
    float psFloat[PS_FLOAT_REGISTERS][4]; bool psFloatSet[PS_FLOAT_REGISTERS];
    int psInt[INT_BOOL_REGISTERS][4]; bool psIntSet[INT_BOOL_REGISTERS];
    BOOL psBool[INT_BOOL_REGISTERS]; bool psBoolSet[INT_BOOL_REGISTERS];
    IDirect3DVertexDeclaration9* declaration; DWORD fvf; bool fvfSet;
    Stream stream[STREAMS]; bool streamSet[STREAMS]; bool frequencySet[STREAMS];
    IDirect3DIndexBuffer9* indices;
    float clipPlane[CLIP_PLANES][4]; bool clipPlaneSet[CLIP_PLANES];
    D3DMATRIX transform[TRANSFORMS]; bool transformSet[TRANSFORMS];   // fixed-function (unused by the game)
};

// One recorded device call.
enum Op : uint8_t {
    OP_RENDER_TARGET, OP_DEPTH_STENCIL, OP_CLEAR, OP_VIEWPORT, OP_SCISSOR, OP_TRANSFORM, OP_CLIP_PLANE,
    OP_MATERIAL, OP_LIGHT, OP_LIGHT_ENABLE, OP_RENDER_STATE, OP_TEXTURE, OP_STAGE_STATE, OP_SAMPLER_STATE,
    OP_VERTEX_SHADER, OP_PIXEL_SHADER, OP_CONSTANTS, OP_DECLARATION, OP_FVF, OP_STREAM, OP_STREAM_FREQUENCY,
    OP_INDICES, OP_DRAW, OP_DRAW_INDEXED, OP_DRAW_UP, OP_DRAW_INDEXED_UP, OP_BEGIN_SCENE, OP_END_SCENE,
    OP_STRETCH_RECT, OP_COLOR_FILL, OP_UPDATE_SURFACE, OP_UPDATE_TEXTURE, OP_VERTEX_WRITE, OP_INDEX_WRITE,
};

// The call's arguments. Their meaning depends on the op (see the record* functions); objects are
// in p and q, and variable-length data (constants, rectangles, buffer contents) is in the
// frame's byte store at [data, data + length).
struct Command {
    Op op; uint8_t kind;   // kind: ConstantKind, for OP_CONSTANTS
    uint32_t a, b, c, d, e, f;
    void* p; void* q;
    uint32_t data, length;
};

struct Frame {
    DeviceState* start = nullptr;    // the device state when the frame began
    std::vector<Command> commands;
    std::vector<uint8_t> bytes;
    std::vector<void*> references;   // objects the frame uses, kept alive until it is dropped
    uint64_t number = 0;
    bool complete = false;           // recorded from its first command
};

// Lets a replay change what it draws (interpolate.cpp blends matched draws and buffer writes).
struct ReplayHooks {
    virtual void beforeDraw(IDirect3DDevice9Ex* device, size_t command) {}
    virtual void afterDraw(IDirect3DDevice9Ex* device, size_t command) {}
    // The bytes to write for a buffer-write command: the recorded ones, or a blend.
    virtual const uint8_t* bufferData(size_t command, const Command& c, const uint8_t* recorded) { return recorded; }
};

extern Frame gLastFrame;     // the last finished game frame (N)
extern Frame gFrameBefore;   // the one before it (N-1)
extern DeviceState gState;   // the device as the game last left it

void applyState(IDirect3DDevice9Ex* device, const DeviceState& state);
void replay(IDirect3DDevice9Ex* device, const Frame& frame, ReplayHooks* hooks = nullptr);
UINT verticesFor(D3DPRIMITIVETYPE type, UINT primitives);
