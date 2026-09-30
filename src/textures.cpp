// TSFix+: texture packs in TSFix's format (such as the 4x upscale pack), used when TSFix isn't.
//
// A pack replaces a game texture by the CRC-32 of the file the game loads it from: a replacement
// is `<crc as 8 hex digits>.dds`, loose in TSFix_Res\inject\textures\ (and its streaming\ and
// blocking\ folders) or inside a .7z in TSFix_Res\inject\. The first one found wins: loose files,
// then the archives in name order (01_CleanFont before 99_Upscale4x). Same rules as TSFix
// (Kaldaien, GPL-3, github.com/Kaldaien/TSF), whose texture loading this is a rewrite of.
//
// How it works:
//   - The game loads every texture with D3DXCreateTextureFromFileInMemoryEx; that function is
//     hooked. When a replacement exists, the game still gets its own texture at once, and a worker
//     thread loads the replacement (4x textures take a while to decompress);
//   - SetTexture then draws with the replacement once it's ready. Replacements of textures still
//     in use are kept; unused ones stay cached (1 GB at most), least recently used out first.
//   - The recorder keeps the game's own texture, so that draws are paired between frames the
//     same way before and after a replacement arrives; main.cpp and recorder.cpp swap in the
//     replacement wherever a texture is really set.
//   - Texture Release is hooked to learn when the game is done with a texture.
#include "common.h"
#include <algorithm>
#include <deque>
#include <string>

extern "C" {
#include "lzma/7z.h"
#include "lzma/7zAlloc.h"
#include "lzma/7zCrc.h"
#include "lzma/7zFile.h"
}

// ---------------------------------------------------------------- D3DX (the game's d3dx9_43.dll)

struct D3DXIMAGE_INFO_ {
    UINT Width, Height, Depth, MipLevels;
    D3DFORMAT Format;
    D3DRESOURCETYPE ResourceType;
    DWORD ImageFileFormat;
};
typedef HRESULT(WINAPI* PFN_CreateFromMemory)(IDirect3DDevice9*, LPCVOID, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                DWORD, DWORD, D3DCOLOR, D3DXIMAGE_INFO_*, PALETTEENTRY*, IDirect3DTexture9**);
typedef HRESULT(WINAPI* PFN_GetImageInfo)(LPCVOID, UINT, D3DXIMAGE_INFO_*);
static PFN_CreateFromMemory o_CreateFromMemory;   // trampoline to the original
static PFN_GetImageInfo pGetImageInfo;
#define D3DX_DEFAULT_ ((UINT)-1)

// ---------------------------------------------------------------- the packs

struct Entry { int archive; UInt32 file; UInt32 size; std::wstring path; };   // archive -1: loose file
static std::unordered_map<uint32_t, Entry> gEntries;                        // crc -> where its replacement is
static std::vector<std::wstring> gArchives;

// Game textures TSFix never replaces (TSFix's list): unused, gamma ramps, plain black and white,
// and logos the EULA says mustn't be replaced.
static const uint32_t kNeverReplace[] = {0x3016437b, 0xfcbde7ab, 0x53709d09, 0xacc41af0, 0xf4329f92, 0x2840f65e,
                                         0xd66ce109, 0x61082a54, 0xd5d4653a, 0x1e5c8a5e, 0x5606ed7b};

static std::wstring gRes;   // <game folder>\TSFix_Res\inject

static bool addEntry(uint32_t crc, const Entry& e) {
    for (uint32_t n : kNeverReplace) if (n == crc) return false;
    return gEntries.emplace(crc, e).second;
}

// "…/0a1b2c3d.dds" -> 0x0a1b2c3d
static bool crcFromName(const wchar_t* name, uint32_t* crc) {
    const wchar_t* base = name;
    for (const wchar_t* p = name; *p; p++) if (*p == L'/' || *p == L'\\') base = p + 1;
    wchar_t* end;
    unsigned long v = wcstoul(base, &end, 16);
    if (end - base != 8 || _wcsicmp(end, L".dds") != 0) return false;
    *crc = (uint32_t)v;
    return true;
}

static void scanLoose(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dds").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        uint32_t crc;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && crcFromName(fd.cFileName, &crc))
            addEntry(crc, {-1, 0, fd.nFileSizeLow, dir + L"\\" + fd.cFileName});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static const ISzAlloc gAlloc = {SzAlloc, SzFree}, gAllocTemp = {SzAllocTemp, SzFreeTemp};

// An open archive: its index, and the file it's read from.
struct Archive {
    CFileInStream file;
    CLookToRead2 look;
    CSzArEx db;
    Byte readBuf[1 << 18];
    bool open = false;
    bool openFile(const std::wstring& path) {
        FileInStream_CreateVTable(&file);
        File_Construct(&file.file);
        LookToRead2_CreateVTable(&look, False);
        look.buf = readBuf;
        look.bufSize = sizeof readBuf;
        look.realStream = &file.vt;
        LookToRead2_INIT(&look);
        if (InFile_OpenW(&file.file, path.c_str())) return false;
        SzArEx_Init(&db);
        if (SzArEx_Open(&db, &look.vt, &gAlloc, &gAllocTemp) != SZ_OK) { File_Close(&file.file); return false; }
        return open = true;
    }
    void close() {
        if (!open) return;
        SzArEx_Free(&db, &gAlloc);
        File_Close(&file.file);
        open = false;
    }
};

static void scanArchive(const std::wstring& path) {
    static Archive a;   // big (its read buffer): not on the stack
    if (!a.openFile(path)) { log("Can't open texture pack %ls", path.c_str()); return; }
    int index = (int)gArchives.size(), added = 0;
    std::vector<UInt16> name;
    for (UInt32 i = 0; i < a.db.NumFiles; i++) {
        if (SzArEx_IsDir(&a.db, i)) continue;
        name.resize(SzArEx_GetFileNameUtf16(&a.db, i, nullptr));
        SzArEx_GetFileNameUtf16(&a.db, i, name.data());
        uint32_t crc;
        if (crcFromName((const wchar_t*)name.data(), &crc) && addEntry(crc, {index, i, (UInt32)SzArEx_GetFileSize(&a.db, i), L""}))
            added++;
    }
    a.close();
    if (added) gArchives.push_back(path);
    log("Texture pack %ls: %d replacements", path.c_str(), added);
}

// ---------------------------------------------------------------- replacements and the cache

struct Repl {
    IDirect3DTexture9* tex = nullptr;   // null until loaded
    UInt32 bytes = 0;                   // its file size, as the cache's measure
    int users = 0;                      // game textures alive that it replaces
    uint64_t lastUse = 0;
    bool queued = false, failed = false;
};
static CRITICAL_SECTION gTexLock;       // guards everything below (never held while calling Direct3D)
static std::unordered_map<uint32_t, Repl> gRepl;              // crc -> replacement
static std::unordered_map<void*, uint32_t> gReplaced;         // game texture -> crc
static std::deque<std::pair<uint32_t, IDirect3DDevice9*>> gQueue;
static HANDLE gWake;
static const uint64_t gCacheLimit = 1024ull << 20;            // unused replacements kept
static uint64_t gLoadedBytes;
static thread_local bool tWorker;

struct TexLock {
    TexLock() { EnterCriticalSection(&gTexLock); }
    ~TexLock() { LeaveCriticalSection(&gTexLock); }
};

bool textureWorkerThread() { return tWorker; }

IDirect3DBaseTexture9* textureReplacement(IDirect3DBaseTexture9* t) {
    if (!t || !o_CreateFromMemory) return t;
    TexLock l;
    auto it = gReplaced.find(t);
    if (it == gReplaced.end()) return t;
    Repl& r = gRepl[it->second];
    return r.tex ? (IDirect3DBaseTexture9*)r.tex : t;
}

// Frees unused replacements, least recently used first, until the cache fits. Returns them to
// be released outside the lock.
static void trimCache(std::vector<IDirect3DTexture9*>& out) {
    while (gLoadedBytes > gCacheLimit) {
        auto oldest = gRepl.end();
        for (auto it = gRepl.begin(); it != gRepl.end(); ++it)
            if (it->second.tex && !it->second.users && (oldest == gRepl.end() || it->second.lastUse < oldest->second.lastUse))
                oldest = it;
        if (oldest == gRepl.end()) return;   // everything left is in use
        out.push_back(oldest->second.tex);
        gLoadedBytes -= oldest->second.bytes;
        gRepl.erase(oldest);
    }
}

// ---- texture Release: learn when the game is done with a texture

typedef ULONG(STDMETHODCALLTYPE* ReleaseFn)(IDirect3DTexture9*);
static std::unordered_map<const void*, ReleaseFn> gReleaseOf;   // texture vtable -> its Release

static ULONG STDMETHODCALLTYPE h_TexRelease(IDirect3DTexture9* t) {
    ReleaseFn real;
    { TexLock l; real = gReleaseOf.at(t->lpVtbl); }
    ULONG n = real(t);
    if (n == 0) {
        TexLock l;
        auto it = gReplaced.find(t);
        if (it != gReplaced.end()) {
            Repl& r = gRepl[it->second];
            r.users--;
            r.lastUse = GetTickCount64();
            gReplaced.erase(it);
        }
    }
    return n;
}

static void hookRelease(IDirect3DTexture9* t) {
    TexLock l;
    const void* vt = t->lpVtbl;
    if (gReleaseOf.count(vt)) return;
    ReleaseFn* slot = (ReleaseFn*)&t->lpVtbl->Release;
    DWORD old;
    VirtualProtect(slot, sizeof *slot, PAGE_EXECUTE_READWRITE, &old);
    gReleaseOf[vt] = *slot;
    *slot = h_TexRelease;
    VirtualProtect(slot, sizeof *slot, old, &old);
}

// ---- the game's texture loads

static HRESULT WINAPI h_CreateFromMemory(IDirect3DDevice9* dev, LPCVOID data, UINT size, UINT w, UINT h, UINT mips, DWORD usage,
                                         D3DFORMAT fmt, D3DPOOL pool, DWORD filter, DWORD mipFilter, D3DCOLOR key,
                                         D3DXIMAGE_INFO_* info, PALETTEENTRY* pal, IDirect3DTexture9** out) {
    HRESULT hr = o_CreateFromMemory(dev, data, size, w, h, mips, usage, fmt, pool, filter, mipFilter, key, info, pal, out);
    if (tWorker || FAILED(hr) || !out || !*out || (usage & (D3DUSAGE_DYNAMIC | D3DUSAGE_RENDERTARGET))) return hr;
    uint32_t crc = CrcCalc(data, size);
    auto e = gEntries.find(crc);   // read-only after start-up
    if (e == gEntries.end()) return hr;
    hookRelease(*out);
    TexLock l;
    auto old = gReplaced.find(*out);   // a texture at a freed one's address, if its release was missed
    if (old != gReplaced.end()) gRepl[old->second].users--;
    gReplaced[*out] = crc;
    Repl& r = gRepl[crc];
    r.users++;
    r.lastUse = GetTickCount64();
    if (!r.tex && !r.queued && !r.failed) {
        r.queued = true;
        r.bytes = e->second.size;
        gQueue.emplace_back(crc, dev);
        SetEvent(gWake);
    }
    return hr;
}

// ---- the worker: decompresses and creates the replacements

static DWORD WINAPI worker(void*) {
    tWorker = true;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<Archive> archives(gArchives.size());
    // The solid block last decompressed, kept for the next texture from the same block.
    int blockArchive = -1;
    UInt32 block = 0xFFFFFFFF;
    Byte* blockData = nullptr;
    size_t blockSize = 0;
    auto dropBlock = [&]() {
        if (blockData) IAlloc_Free(&gAlloc, blockData);
        blockData = nullptr; blockSize = 0; block = 0xFFFFFFFF; blockArchive = -1;
    };
    std::vector<uint8_t> loose;
    for (;;) {
        // An idle second frees the decompressed block (up to ~170 MB of a 32-bit address space).
        if (WaitForSingleObject(gWake, blockData ? 1000 : INFINITE) == WAIT_TIMEOUT) { dropBlock(); continue; }
        for (;;) {
            uint32_t crc; IDirect3DDevice9* dev; Entry e;
            {
                TexLock l;
                if (gQueue.empty()) break;
                // Textures from the block already decompressed go first.
                size_t pick = 0;
                for (size_t i = 0; i < gQueue.size() && blockData; i++) {
                    const Entry& q = gEntries.at(gQueue[i].first);
                    if (q.archive == blockArchive && archives[q.archive].open &&
                        archives[q.archive].db.FileToFolder[q.file] == block) { pick = i; break; }
                }
                crc = gQueue[pick].first; dev = gQueue[pick].second;
                gQueue.erase(gQueue.begin() + pick);
                e = gEntries.at(crc);
                if (!gRepl.count(crc) || !gRepl[crc].users) { gRepl.erase(crc); continue; }   // no longer wanted
            }
            const uint8_t* bytes = nullptr;
            size_t n = 0;
            if (e.archive < 0) {
                HANDLE f = CreateFileW(e.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                DWORD got = 0;
                if (f != INVALID_HANDLE_VALUE) {
                    loose.resize(GetFileSize(f, nullptr));
                    if (ReadFile(f, loose.data(), (DWORD)loose.size(), &got, nullptr) && got == loose.size()) { bytes = loose.data(); n = got; }
                    CloseHandle(f);
                }
            } else {
                Archive& a = archives[e.archive];
                if (!a.open && !a.openFile(gArchives[e.archive])) log("Can't open texture pack %ls", gArchives[e.archive].c_str());
                if (a.open) {
                    if (blockArchive != e.archive) dropBlock();
                    blockArchive = e.archive;
                    size_t offset = 0, got = 0;
                    SRes res = SzArEx_Extract(&a.db, &a.look.vt, e.file, &block, &blockData, &blockSize, &offset, &got, &gAlloc, &gAllocTemp);
                    if (res == SZ_OK) { bytes = blockData + offset; n = got; }
                    else { log("Texture %08x: decompression failed (%d)", crc, res); dropBlock(); }
                }
            }
            IDirect3DTexture9* tex = nullptr;
            D3DXIMAGE_INFO_ info = {};
            if (bytes && SUCCEEDED(pGetImageInfo(bytes, (UINT)n, &info)))
                o_CreateFromMemory(dev, bytes, (UINT)n, info.Width, info.Height, info.MipLevels, 0, info.Format, D3DPOOL_DEFAULT,
                                   D3DX_DEFAULT_, D3DX_DEFAULT_, 0, &info, nullptr, &tex);
            std::vector<IDirect3DTexture9*> release;
            {
                TexLock l;
                Repl& r = gRepl[crc];
                r.queued = false;
                if (tex) { r.tex = tex; gLoadedBytes += r.bytes; } else r.failed = true;
                trimCache(release);
            }
            for (IDirect3DTexture9* t : release) t->lpVtbl->Release(t);
            if (!tex) log("Texture %08x: its replacement couldn't be loaded", crc);
        }
    }
}

// ---------------------------------------------------------------- start-up

// Jumps from D3DX's function to the hook. Its first five bytes (mov edi,edi / push ebp /
// mov ebp,esp: the standard hot-patch prologue) move to a trampoline that calls the original.
static bool hookD3DX(HMODULE x) {
    uint8_t* f = (uint8_t*)GetProcAddress(x, "D3DXCreateTextureFromFileInMemoryEx");
    pGetImageInfo = (PFN_GetImageInfo)GetProcAddress(x, "D3DXGetImageInfoFromFileInMemory");
    static const uint8_t prologue[5] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC};
    if (!f || !pGetImageInfo || memcmp(f, prologue, 5) != 0) return false;
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(tramp, f, 5);
    tramp[5] = 0xE9;
    int32_t back = (int32_t)((f + 5) - (tramp + 10));
    memcpy(tramp + 6, &back, 4);
    o_CreateFromMemory = (PFN_CreateFromMemory)tramp;
    uint8_t jmp[5] = {0xE9};
    int32_t to = (int32_t)((uint8_t*)h_CreateFromMemory - (f + 5));
    memcpy(jmp + 1, &to, 4);
    DWORD old;
    VirtualProtect(f, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy(f, jmp, 5);
    VirtualProtect(f, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), f, 5);
    return true;
}

void texturesStart(bool enabled) {
    if (!enabled) { log("Texture packs off (TexturePacks=0)"); return; }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    gRes = exe;
    gRes = gRes.substr(0, gRes.find_last_of(L'\\')) + L"\\TSFix_Res\\inject";
    if (GetFileAttributesW(gRes.c_str()) == INVALID_FILE_ATTRIBUTES) { log("No texture packs (no TSFix_Res\\inject folder)"); return; }
    CrcGenerateTable();
    scanLoose(gRes + L"\\textures");
    scanLoose(gRes + L"\\textures\\streaming");
    scanLoose(gRes + L"\\textures\\blocking");
    size_t loose = gEntries.size();
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((gRes + L"\\*.7z").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do names.push_back(fd.cFileName); while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (const std::wstring& n : names) scanArchive(gRes + L"\\" + n);
    log("Texture packs: %zu replacements (%zu loose files, %zu archives)", gEntries.size(), loose, gArchives.size());
    if (gEntries.empty()) return;
    HMODULE x = LoadLibraryA("d3dx9_43.dll");
    InitializeCriticalSection(&gTexLock);
    if (!x || !hookD3DX(x)) { log("D3DX's texture loader couldn't be hooked: texture packs off"); return; }
    gWake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, worker, nullptr, 0, nullptr));
    log("Texture packs on");
}
