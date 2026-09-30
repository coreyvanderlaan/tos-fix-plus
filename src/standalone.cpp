// TSFix+: the fixes the Steam version of the game needs, so that TSFix isn't needed.
//
// TSFix (Kaldaien, GPL-3, github.com/Kaldaien/TSF) found these problems; TSFix+ fixes the ones
// the official Steam version (the August 2016 update) still has. Everything in the game is found
// by its code, never by a fixed address: TSFix's fixed addresses are for the 2016 launch version
// and crash the current one.
//   - the game's own 30 fps limiter is switched off: TSFix+ keeps the game at 30 itself;
//   - the game's 60 Hz timer is created the way the game asks for it (videos stutter and break
//     up otherwise);
//   - the game's clock stays at 2 ticks per frame;
//   - the Zelos title achievement, misspelled in the game, is corrected;
//   - the game keeps running in the background, and fullscreen is a borderless window over the
//     screen (in exclusive fullscreen the game minimises on Alt+Tab and stops responding);
//   - Alt+Tab works: the window is never "always on top", the keyboard and mouse are shared with
//     Windows, and the cursor isn't kept inside the window;
//   - each game frame waits for the GPU while a video plays (black blocks otherwise).
// None of this runs when TSFix is loaded: it does these itself.
#include "common.h"
#include <cstring>

bool gStandalone;

static HWND gWindow;                                   // the game's window
static std::unordered_map<HWND, WNDPROC> gGameProcs;   // its own window procedure

// ---------------------------------------------------------------- patching

// Finds `pattern` in the game's code (mask 0 = any byte). Returns nullptr unless it's there
// exactly once.
static uint8_t* findOnce(const uint8_t* pattern, const uint8_t* mask, size_t n) {
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    uint8_t* imageEnd = base + nt->OptionalHeader.SizeOfImage;
    uint8_t* found = nullptr;
    int count = 0;
    for (uint8_t* p = base; p + n <= imageEnd;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi)) break;
        uint8_t* end = (uint8_t*)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint8_t* q = p; q + n <= end && q + n <= imageEnd; q++) {
                size_t i = 0;
                while (i < n && (!mask[i] || q[i] == pattern[i])) i++;
                if (i == n) { found = q; count++; }
            }
        p = end;
    }
    return count == 1 ? found : nullptr;
}

static void writeBytes(void* at, const void* bytes, size_t n) {
    DWORD old;
    VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

static void writeJump(uint8_t* at, const void* to) {
    uint8_t jmp[5] = {0xE9};
    int32_t rel = (int32_t)((const uint8_t*)to - (at + 5));
    memcpy(jmp + 1, &rel, 4);
    writeBytes(at, jmp, 5);
}

// Makes a function jump to `hook`. It must start with the standard hot-patch prologue
// (mov edi,edi / push ebp / mov ebp,esp): those five bytes move to a small trampoline, returned,
// which runs the original function. nullptr if the function doesn't start that way.
static void* hotpatch(void* function, void* hook) {
    uint8_t* f = (uint8_t*)function;
    if (!f || memcmp(f, "\x8B\xFF\x55\x8B\xEC", 5) != 0) return nullptr;
    uint8_t* trampoline = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return nullptr;
    memcpy(trampoline, f, 5);
    writeJump(trampoline + 5, f + 5);
    writeJump(f, hook);
    return trampoline;
}

// Hooks a function whichever way it starts: the hot-patch prologue (above); a jump through a
// pointer to its implementation in another DLL (FF 25 <address>: user32's ClipCursor and
// SetCursorPos, for one); or a jump another program's hook put there (E9: the Steam overlay's).
// A jump's target becomes the original. nullptr otherwise.
static void* hookFunction(void* function, void* hook) {
    uint8_t* f = (uint8_t*)function;
    if (!f) return nullptr;
    if (void* original = hotpatch(f, hook)) return original;
    void* original = nullptr;
    if (f[0] == 0xFF && f[1] == 0x25) original = **(void***)(f + 2);
    else if (f[0] == 0xE9) original = f + 5 + *(int32_t*)(f + 1);   // already hooked (the Steam overlay): chain on
    else return nullptr;
    writeJump(f, hook);
    return original;
}

// ---------------------------------------------------------------- the game's frame limiter

// The game's limiter waits until 1/30 s has passed since the last frame. TSFix+ keeps the game at
// 30 itself, and two limiters drift against each other and hold frames back. Its first
// instruction becomes a return (it takes no arguments and returns nothing). The pattern is
// TSFix's.
static void disableGameLimiter() {
    static const uint8_t pattern[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x14, 0x80, 0x3D, 0, 0, 0, 0,
                                      0x00, 0x53, 0x56, 0x57, 0x0F, 0x84, 0, 0, 0, 0, 0x83, 0x3D, 0, 0, 0,
                                      0, 0x02, 0x0F, 0x82, 0, 0, 0, 0};
    static const uint8_t mask[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0,
                                   1, 1, 1, 0, 0, 0, 0};
    uint8_t* f = findOnce(pattern, mask, sizeof pattern);
    if (!f) { log("Game frame limiter not found: left as it is"); return; }
    const uint8_t ret = 0xC3;
    writeBytes(f, &ret, 1);
    log("Game frame limiter switched off");
}

// ---------------------------------------------------------------- the game's 60 Hz timer

// The game creates its 60 Hz timer with the "run once" flag (WT_EXECUTEONLYONCE) and a 16 ms
// repeat period, which Windows' documentation rules out: with that flag the period must be 0.
// The videos run on this timer; left as it is, they stutter and break up into black blocks, more
// and more as they play. TSFix sets the period to 0 in this case, and so does this.
typedef BOOL(WINAPI* CreateTimerQueueTimerFn)(PHANDLE, HANDLE, WAITORTIMERCALLBACK, PVOID, DWORD, DWORD, ULONG);
static CreateTimerQueueTimerFn gCreateTimerQueueTimer;

static BOOL WINAPI hookCreateTimerQueueTimer(PHANDLE timer, HANDLE queue, WAITORTIMERCALLBACK callback, PVOID parameter,
                                             DWORD due, DWORD period, ULONG flags) {
    if ((flags & WT_EXECUTEONLYONCE) && period) {
        log("Game timer (%lu ms, repeating every %lu ms, run once) created without the repeat", due, period);
        period = 0;
    }
    return gCreateTimerQueueTimer(timer, queue, callback, parameter, due, period, flags);
}

static void fixGameTimer() {
    void* f = GetProcAddress(GetModuleHandleA("kernelbase.dll"), "CreateTimerQueueTimer");
    if (!f) f = GetProcAddress(GetModuleHandleA("kernel32.dll"), "CreateTimerQueueTimer");
    gCreateTimerQueueTimer = (CreateTimerQueueTimerFn)hotpatch(f, (void*)hookCreateTimerQueueTimer);
    if (!gCreateTimerQueueTimer) log("CreateTimerQueueTimer couldn't be hooked: videos may stutter");
}

// ---------------------------------------------------------------- the game's clock

// The game counts time in 60 Hz ticks, with a rate in ticks per frame: 2 at 30 fps. TSFix keeps
// it at 2 (its fixed-address hooks in the launch version); this does the same. The function that
// queues a rate change stores the request and queues a setter:
//   2B 05 ........  sub eax, [...]           C7 40 04 <setter>  mov [eax+4], setter
//   89 35 <req>     mov [requested], esi     setter: 55 8B EC 8B 45 08 A3 <rate> 5D C3
// The request store is removed, the setter always stores 2, and both are set to 2 now.
static void pinTickRate() {
    static const uint8_t pattern[] = {0x2B, 0x05, 0, 0, 0, 0, 0x89, 0x35, 0, 0, 0, 0, 0x83, 0xF8, 0x0C, 0x73, 0x13, 0x8B, 0x0D};
    static const uint8_t mask[] = {1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1};
    uint8_t* q = findOnce(pattern, mask, sizeof pattern);
    if (!q || memcmp(q + 0x3F, "\xC7\x40\x04", 3) != 0) { log("Game clock code not found: left as it is"); return; }
    uint32_t* requested = *(uint32_t**)(q + 8);
    uint8_t* setter = *(uint8_t**)(q + 0x42);
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(setter, &mi, sizeof mi) || mi.State != MEM_COMMIT ||
        memcmp(setter, "\x55\x8B\xEC\x8B\x45\x08\xA3", 7) != 0 || memcmp(setter + 11, "\x5D\xC3", 2) != 0) {
        log("Game clock setter not as expected: left as it is");
        return;
    }
    uint32_t* rate = *(uint32_t**)(setter + 7);
    uint8_t store2[11] = {0xB8, 2, 0, 0, 0, 0xA3, 0, 0, 0, 0, 0xC3};   // mov eax, 2 / mov [rate], eax / ret
    memcpy(store2 + 6, &rate, 4);
    writeBytes(setter, store2, sizeof store2);
    static const uint8_t nops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    writeBytes(q + 6, nops, sizeof nops);
    *rate = 2;
    *requested = 2;
    log("Game clock kept at 2 ticks per frame");
}

// ---------------------------------------------------------------- the Zelos achievement

// The game asks Steam for "TROPHY_ID_ZELOSZ_TITLE_COMPLET", missing its last letter, so the Zelos
// title achievement never unlocks (found by TSFix). Only zero bytes follow the text, so the
// corrected one fits.
static void fixZelosAchievement() {
    static const char wrong[] = "TROPHY_ID_ZELOSZ_TITLE_COMPLET";   // with its terminating zero
    uint8_t mask[sizeof wrong];
    memset(mask, 1, sizeof mask);
    uint8_t* s = findOnce((const uint8_t*)wrong, mask, sizeof wrong);
    if (!s || s[sizeof wrong] != 0) { log("Zelos achievement ID not found (or already correct)"); return; }
    writeBytes(s, "TROPHY_ID_ZELOSZ_TITLE_COMPLETE", sizeof wrong + 1);
    log("Zelos achievement ID corrected");
}

// ---------------------------------------------------------------- background and window

// The game never learns that it lost focus: it would stop (and, in exclusive fullscreen, minimise
// and stop responding). Its window's activation messages aren't passed to it, and Windows' "which
// window is active" functions report the game's window. (These are patched in user32 itself: the
// game's own import table is encrypted, and hooking it crashed.)
typedef HWND(WINAPI* WindowFn)();
static WindowFn gRealForegroundWindow;   // Windows' own answer, when it could be kept

static HWND WINAPI hookGetForegroundWindow() { return gWindow; }
static HWND WINAPI hookGetFocus() { return gWindow; }
static HWND WINAPI hookGetActiveWindow() { return gWindow; }

static bool reallyInFront() { return !gRealForegroundWindow || gRealForegroundWindow() == gWindow; }

static void reportGameWindowAsActive() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    const struct { const char* name; void* hook; } hooks[] = {{"GetForegroundWindow", (void*)hookGetForegroundWindow},
                                                             {"GetFocus", (void*)hookGetFocus},
                                                             {"GetActiveWindow", (void*)hookGetActiveWindow}};
    for (const auto& h : hooks) {
        uint8_t* f = (uint8_t*)GetProcAddress(user32, h.name);
        if (!f) { log("user32 %s not found", h.name); continue; }
        void* original = hookFunction(f, h.hook);
        if (!original) writeJump(f, h.hook);   // without a way back to the original
        else if (h.hook == (void*)hookGetForegroundWindow) gRealForegroundWindow = (WindowFn)original;
    }
}

// ---------------------------------------------------------------- keyboard, mouse and Alt+Tab

// The game takes the keyboard and mouse through DirectInput in exclusive mode, which keeps
// Alt+Tab from reaching Windows, and it confines the cursor to its window. As TSFix does, its
// devices are set up non-exclusive (input still only while the game is in front), the cursor is
// never confined, and the game can't move the cursor while another window is in front.
enum { DISCL_EXCLUSIVE_ = 1, DISCL_NONEXCLUSIVE_ = 2, DISCL_FOREGROUND_ = 4, DISCL_BACKGROUND_ = 8 };
enum { SLOT_CREATE_DEVICE = 3, SLOT_SET_COOPERATIVE_LEVEL = 13 };   // IDirectInput8, IDirectInputDevice8 vtables

typedef HRESULT(WINAPI* DirectInput8CreateFn)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
typedef HRESULT(STDMETHODCALLTYPE* CreateInputDeviceFn)(void*, REFGUID, void**, IUnknown*);
typedef HRESULT(STDMETHODCALLTYPE* SetCooperativeLevelFn)(void*, HWND, DWORD);
static DirectInput8CreateFn gDirectInput8Create;
static std::unordered_map<void*, CreateInputDeviceFn> gCreateInputDevice;       // per vtable
static std::unordered_map<void*, SetCooperativeLevelFn> gSetCooperativeLevel;   // per vtable

// Replaces slot `slot` of an object's vtable, keeping the original per vtable.
template <class T> static void patchSlot(void* object, int slot, T hook, std::unordered_map<void*, T>& originals) {
    void** vtable = *(void***)object;
    if (originals.count(vtable)) return;
    originals[vtable] = (T)vtable[slot];
    DWORD old;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtable[slot] = (void*)hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
}

static HRESULT STDMETHODCALLTYPE hookSetCooperativeLevel(void* device, HWND w, DWORD flags) {
    SetCooperativeLevelFn original;
    { Locked lock; original = gSetCooperativeLevel.at(*(void**)device); }
    DWORD shared = (flags & ~(DISCL_EXCLUSIVE_ | DISCL_BACKGROUND_)) | DISCL_NONEXCLUSIVE_ | DISCL_FOREGROUND_;
    if (shared != flags) log("Keyboard/mouse: shared with Windows instead of exclusive (Alt+Tab works)");
    return original(device, w, shared);
}

static HRESULT STDMETHODCALLTYPE hookCreateInputDevice(void* input, REFGUID guid, void** device, IUnknown* outer) {
    CreateInputDeviceFn original;
    { Locked lock; original = gCreateInputDevice.at(*(void**)input); }
    HRESULT hr = original(input, guid, device, outer);
    if (SUCCEEDED(hr) && device && *device) {
        Locked lock;
        patchSlot(*device, SLOT_SET_COOPERATIVE_LEVEL, (SetCooperativeLevelFn)hookSetCooperativeLevel, gSetCooperativeLevel);
    }
    return hr;
}

static HRESULT WINAPI hookDirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, void** out, IUnknown* outer) {
    HRESULT hr = gDirectInput8Create(instance, version, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        Locked lock;
        patchSlot(*out, SLOT_CREATE_DEVICE, (CreateInputDeviceFn)hookCreateInputDevice, gCreateInputDevice);
    }
    return hr;
}

typedef BOOL(WINAPI* ClipCursorFn)(const RECT*);
typedef BOOL(WINAPI* SetCursorPosFn)(int, int);
static ClipCursorFn gClipCursor;
static SetCursorPosFn gSetCursorPos;
static BOOL WINAPI hookClipCursor(const RECT*) { return gClipCursor(nullptr); }
static BOOL WINAPI hookSetCursorPos(int x, int y) { return reallyInFront() ? gSetCursorPos(x, y) : TRUE; }

static void shareInput() {
    HMODULE dinput8 = LoadLibraryA("dinput8.dll");   // the game's own import: already loaded
    gDirectInput8Create = (DirectInput8CreateFn)hookFunction(GetProcAddress(dinput8, "DirectInput8Create"), (void*)hookDirectInput8Create);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    gClipCursor = (ClipCursorFn)hookFunction(GetProcAddress(user32, "ClipCursor"), (void*)hookClipCursor);
    gSetCursorPos = (SetCursorPosFn)hookFunction(GetProcAddress(user32, "SetCursorPos"), (void*)hookSetCursorPos);
    if (!gDirectInput8Create) log("DirectInput8Create couldn't be hooked: Alt+Tab may not work");
    if (!gClipCursor || !gSetCursorPos) log("ClipCursor/SetCursorPos couldn't be hooked: the cursor may stay in the window");
}

static LRESULT CALLBACK windowProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATEAPP:
    case WM_ACTIVATE:
        return 0;                                // not passed to the game
    case WM_NCACTIVATE:
        return DefWindowProcA(w, msg, wp, lp);   // the frame only
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_WINDOWPOSCHANGING: {
        // Never "always on top": another window brought to the front with Alt+Tab would be behind
        // the game, which would seem not to let go. The window becomes it by being asked for it,
        // or by being placed just behind a window that is (an overlay).
        WINDOWPOS* pos = (WINDOWPOS*)lp;
        if (pos && !(pos->flags & SWP_NOZORDER)) {
            HWND after = pos->hwndInsertAfter;
            bool special = after == HWND_TOP || after == HWND_BOTTOM || after == HWND_TOPMOST || after == HWND_NOTOPMOST;
            if (after == HWND_TOPMOST || (!special && (GetWindowLongA(after, GWL_EXSTYLE) & WS_EX_TOPMOST))) pos->hwndInsertAfter = HWND_NOTOPMOST;
        }
        break;
    }
    }
    auto it = gGameProcs.find(w);
    return it == gGameProcs.end() ? DefWindowProcA(w, msg, wp, lp) : CallWindowProcA(it->second, w, msg, wp, lp);
}

// Fullscreen: a borderless window over the whole monitor.
static void coverMonitor(HWND w) {
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& m = mi.rcMonitor;
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongA(w, GWL_EXSTYLE, WS_EX_APPWINDOW);
    SetWindowPos(w, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

// Windowed: a window without a frame, centred on the monitor (as TSFix shows it).
static void borderlessCentred(HWND w, UINT width, UINT height) {
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& m = mi.rcMonitor;
    int mw = m.right - m.left, mh = m.bottom - m.top;
    int ww = (int)width < mw ? (int)width : mw, wh = (int)height < mh ? (int)height : mh;
    SetWindowLongA(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(w, HWND_NOTOPMOST, m.left + (mw - ww) / 2, m.top + (mh - wh) / 2, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

bool standaloneDeviceParams(D3DPRESENT_PARAMETERS* p, HWND focus) {
    if (!gStandalone || !p) return false;
    HWND w = p->hDeviceWindow ? p->hDeviceWindow : focus;
    if (w) {
        gWindow = w;
        if (!gGameProcs.count(w)) gGameProcs[w] = (WNDPROC)SetWindowLongPtrA(w, GWLP_WNDPROC, (LONG_PTR)windowProc);
        reportGameWindowAsActive();
    }
    if (p->Windowed) return false;
    p->Windowed = TRUE;   // a borderless window instead of exclusive fullscreen
    p->FullScreen_RefreshRateInHz = 0;
    return true;
}

void standaloneDeviceCreated(D3DPRESENT_PARAMETERS* p, bool fullscreen, HRESULT hr) {
    if (!gStandalone || !p) return;
    if (!fullscreen) {
        if (SUCCEEDED(hr) && gWindow && p->BackBufferWidth > 640) borderlessCentred(gWindow, p->BackBufferWidth, p->BackBufferHeight);
        return;
    }
    p->Windowed = FALSE;   // the game reads its settings back
    if (SUCCEEDED(hr) && gWindow) {
        coverMonitor(gWindow);
        log("Fullscreen %ux%u, shown as a borderless window", p->BackBufferWidth, p->BackBufferHeight);
    }
}

// ---------------------------------------------------------------- videos

// While nothing is blended (a video is playing), each game frame waits until the GPU has finished
// it. The game's video player writes the next video frame into its texture while the GPU may
// still be drawing the previous one from it: black blocks and stutter otherwise. Blended scenes
// don't need it, and the wait would cost them frames.
void standaloneFrame(IDirect3DDevice9* device) {
    if (!gStandalone) return;
    // The window, if it has become "always on top" anyway (see windowProc), is put back.
    if (gWindow && (GetWindowLongA(gWindow, GWL_EXSTYLE) & WS_EX_TOPMOST))
        SetWindowPos(gWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    if (blendedLastFrame() > 0) return;
    static IDirect3DQuery9* query;
    static IDirect3DDevice9* queryDevice;
    if (queryDevice != device) {
        if (query) query->lpVtbl->Release(query);
        query = nullptr;
        queryDevice = device;
        if (FAILED(device->lpVtbl->CreateQuery(device, D3DQUERYTYPE_EVENT, &query))) query = nullptr;
    }
    if (!query) return;
    query->lpVtbl->Issue(query, D3DISSUE_END);
    for (int i = 0; query->lpVtbl->GetData(query, nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && i < 200000; i++) YieldProcessor();
}

void standaloneStart() {
    gStandalone = GetModuleHandleA("tsfix.dll") == nullptr;
    if (!gStandalone) { log("TSFix is loaded: it provides the game fixes"); return; }
    disableGameLimiter();
    pinTickRate();
    fixGameTimer();
    fixZelosAchievement();
    shareInput();
}
