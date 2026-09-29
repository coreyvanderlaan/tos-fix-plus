# How TOSFIXPLUS works

This document explains the design for anyone who wants to understand or change the code. It
assumes some familiarity with Direct3D 9 (devices, shaders, vertex buffers, Present).

## The problem

Tales of Symphonia (PC, 2016) advances its world exactly one step each time it presents a frame,
and TSFix limits it to 30 frames a second. Raising the limit makes the whole game run faster:
battles, walking and cutscenes all go at double speed at 60. The game's logic can't simply run
faster.

So TOSFIXPLUS leaves the logic at 30 and adds frames **between** the game's own: for every
refresh of the display, it redraws the game's latest frame with each object moved partway back
towards where it was in the frame before. This is interpolation, done with the game's own draw
calls, not by processing images.

## The pipeline

```
TOS.exe ──▶ Special K (d3d9.dll) ──▶ tosfixplus.dll ──▶ dgVoodoo.dll or Windows' d3d9.dll
                                        │
                                        ├─ main.cpp         hooks the device's methods
                                        ├─ recorder.cpp     records each frame, can redraw it
                                        └─ interpolate.cpp  pairs, blends, paces
```

Special K loads `tosfixplus.dll` as its Direct3D 9 "proxy". TOSFIXPLUS loads the real Direct3D 9,
lets the game create its device, and replaces entries in the device's method table (vtable) with
its own functions. Each of those tells the recorder what the game did and then calls the real
method, so the game draws exactly as before. Present is the exception: it goes to
`presentFrame()` in `interpolate.cpp`.

## Recording a frame (recorder.cpp)

Between two Presents, every call that changes device state or draws is stored as a `Command`
in a `Frame`, with its data (constants, rectangles, and so on) in the frame's byte store. Two
details make a recording complete:

- **The starting state.** The game creates its device as a *pure* device, which can't be asked
  for its current state. So TOSFIXPLUS keeps its own copy of everything the game has set
  (`DeviceState`), and each frame starts with a snapshot of it.
- **Buffer contents.** The game rewrites some vertex buffers during a frame (characters, which it
  poses on the CPU, and a shared buffer for sprites and 2D). Every write to a dynamic buffer is
  recorded (`OP_VERTEX_WRITE`), so a redraw writes the same bytes at the same point.

A frame holds a reference to every object it uses, so nothing it needs is freed before a redraw.
All of them are released before a device Reset, which Direct3D 9 requires.

`replay()` applies the starting state and issues every command again, calling `ReplayHooks`
around each draw and buffer write; that is how interpolation changes what gets drawn. With no
hooks, a redraw is pixel-identical to the game's own frame. (This was verified by filling every
render target with a solid colour before redrawing and comparing every pixel.)

## Pairing draws between frames (interpolate.cpp, buildPlan)

To blend an object, TOSFIXPLUS has to find the same object in the previous frame. There are no
object names at this level, only draw calls, so a draw's identity is built from what it uses: the
render target, shaders, buffers and their offsets, the draw's arguments and its first texture
(`DrawInfo::key`). Draws with the same identity are paired in order. For the world's 3D objects
this pairs everything.

The game's vertex shaders kept their constant tables, variable names included, which tells
TOSFIXPLUS what each shader's constants are (`noteVertexShader` in `main.cpp`):

| Constants | Name | Meaning |
|---|---|---|
| c0-c3 | `mMatrixWVP` | object → screen |
| c4-c7 | `mMatrixWV` | object → camera |
| c8-c11 | `mMatrixW` | object → world |
| c16-c18 | `gCBuffer1` | light vectors (or outline width) |

Each object is drawn by up to three shaders: the colour pass, a depth pass (which TSFix's
outlines are built from) and an outline pass. All three are recognised by `mMatrixWV` in their
constant table, so an object's colour, depth and outline always move together.

## What is blended, and how

An in-between frame has a blend factor `alpha`: 0 is the previous frame, 1 the latest.

**Objects placed by matrices** (scenery, terrain, buildings, the camera): constants c0-c11 (and
c16-c18) are blended between the paired draws, set just before the draw and put back after it.

**Characters**: the game poses them on the CPU and rewrites their whole vertex buffer every
frame. When the previous frame wrote the same buffer range, positions and normals are blended
(the shaders renormalise normals). A mesh that moved further than its own size is taken to have
teleported and isn't blended.

**Sprites** (effects, grass, damage numbers, shadows, the target marker) need more care:

- They are rebuilt every frame in one shared, ring-like vertex buffer, so their buffer offsets
  never match between frames. They are paired by kind instead (same shader, texture and size,
  `DrawInfo::key2`).
- They are drawn **one triangle per draw**. A quad is two draws; a round shadow is a ring of
  about 16 thin triangles around a shared centre.
- All sprites share the camera's matrix, so blending that (c0-c3) keeps them with the scene when
  the camera moves, whatever the pairing. That is done for every sprite.
- Their own movement is written into their vertices. Blending that per triangle pulled shapes
  apart (triangles paired with the wrong partner), so only two kinds are moved, both recognised
  by the part of their texture they show, which stays the same with or without texture packs:
  - the **battle target marker** (`MARKER`): each half is paired with the nearest half of the
    previous frame and moved as one piece;
  - **round ground shadows** (`SHADOW`): triangles that share a vertex are grouped into whole
    shadows, each shadow is paired with the nearest one, and all its triangles move together.
- An indexed sprite draw's `MinIndex`/`NumVertices` arguments don't describe the vertices it
  uses (they say 0-2 while the index list points thousands further on), so the vertices are
  found from the draw's index list. Index buffers can't be read back from the GPU, so TOSFIXPLUS
  keeps a copy of each as the game writes it (`gIndexCopies`).
- The shared buffer is written with `D3DLOCK_NOOVERWRITE`, a promise not to change data the GPU
  may still be reading. Moved sprite vertices therefore go into TOSFIXPLUS's own buffer, renewed
  for each in-between frame, and the draw reads from there.

**Never blended**: anything drawn with a flat (orthographic) projection, which is the HUD, menus
and on-screen text; sprites whose matrix changed a lot (animations such as the end-of-battle
text, which otherwise flashed across the screen); and everything during a camera cut, detected
when most objects' matrices change completely at once.

## Pacing (interpolate.cpp, presentFrame)

TOSFIXPLUS decides when the game gets to run, which makes it the frame limiter:

1. When the game presents frame N, a schedule advances by exactly 1/30 s. After a stall (a load,
   a hitch) the schedule restarts.
2. The desktop compositor, which puts images on screen in windowed and borderless modes, reports
   when the last refresh happened and the time between refreshes. An image presented between two
   refreshes appears at the second, and a later Present before then would replace it. So
   TOSFIXPLUS draws each in-between frame just after the previous one's refresh has passed, for
   the next refresh, with `alpha` taken from that refresh's time: one frame per refresh, on one
   continuous timeline across game frames.
3. It stops early enough for the game to produce frame N+1 on time, using a running estimate of
   how long the game takes per frame (frames longer than 1/30 s, such as loads, don't count
   towards it), then waits until the schedule says the game may continue.

With F9 off, the game's own frame is presented as it is and the same pacing applies, so the game
speed never changes. Direct3D 9's Present doesn't wait for the display here, which is why the
compositor's timing is used.

## Changing things

- **A sprite that still steps at 30.** Find the part of its texture it shows (its texture
  coordinates' bounding box) and add it to `MARKER` if it's a simple shape, or `SHADOW` if it's a
  ring or fan of triangles sharing a vertex. The texture coordinates can be read from a draw's
  vertices with a Direct3D 9 capture tool such as RenderDoc or apitrace, or by logging
  `Sprite::uvLo` and `uvHi` from `spriteOf()`.
- **Something blends that shouldn't.** Check how its draws are paired (`buildPlan`) and whether
  a guard applies: `flat()`, the size tests, `matrixChange()`.
- **Testing a change**: compare with F9, and watch `tosfixplus.log`. Its line every minute shows
  game frames per second (30.00 when pacing is right) and frames shown per second (close to the
  refresh rate when blending is running).
