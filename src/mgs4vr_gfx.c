/* MGS4-PCVR - Copyright (c) 2026 Shiffo0. MIT; see LICENSE and THIRD_PARTY_NOTICES.md. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mgs4vr_gfx.h"

typedef HRESULT (WINAPI *CREATE_FACTORY_FN)(REFIID, void **);
typedef HRESULT (WINAPI *CREATE_FACTORY2_FN)(UINT, REFIID, void **);
typedef HRESULT (WINAPI *D3D11_CREATE_FN)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *,
    ID3D11DeviceContext **);
typedef HRESULT (WINAPI *D3D11_CREATE_SC_FN)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *D3D12_CREATE_FN)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef FARPROC (WINAPI *GPA_FN)(HMODULE, LPCSTR);
typedef HRESULT (STDMETHODCALLTYPE *CREATE_SC_FN)(IDXGIFactory *, IUnknown *,
    DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT (STDMETHODCALLTYPE *CREATE_SC_HWND_FN)(IDXGIFactory2 *, IUnknown *, HWND,
    const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *,
    IDXGISwapChain1 **);
typedef HRESULT (STDMETHODCALLTYPE *PRESENT_FN)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *PRESENT1_FN)(IDXGISwapChain1 *, UINT, UINT,
    const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT (STDMETHODCALLTYPE *RESIZE_FN)(IDXGISwapChain *, UINT, UINT, UINT,
    DXGI_FORMAT, UINT);

typedef struct { void **slot; void *old_value; void *our_value; } PATCH;

#define MAX_PATCHES 32

static CRITICAL_SECTION g_cs;
static LONG g_cs_state;                 
static volatile LONG g_started, g_stopping;
static void (*volatile g_log)(const char *fmt, ...);
static int g_stats_period_ms = 5000;

static PATCH g_patches[MAX_PATCHES];
static int g_patch_count;
static PATCH *g_iat_gpa;                

static GPA_FN g_real_gpa;
static CREATE_FACTORY_FN g_real_factory, g_real_factory1;
static CREATE_FACTORY2_FN g_real_factory2;
static D3D11_CREATE_FN g_real_d3d11;
static D3D11_CREATE_SC_FN g_real_d3d11_sc;
static D3D12_CREATE_FN g_real_d3d12;
static CREATE_SC_FN g_real_create_sc;
static CREATE_SC_HWND_FN g_real_create_sc_hwnd;
static PRESENT_FN g_real_present;
static PRESENT1_FN g_real_present1;
static RESIZE_FN g_real_resize;

static MGS4VR_GFX_STATS g_stats;
static volatile LONG g_strip_singlethreaded;
static void (*volatile g_present_cb)(void *sc, unsigned device_flags);
static IDXGISwapChain *g_swapchain;     
static __declspec(thread) int t_inside;

static void log_msg(const char *fmt, ...) {
    char text[700];
    va_list args;
    void (*log)(const char *fmt_, ...) = g_log;
    if (!log) return;
    va_start(args, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, args);
    va_end(args);
    log("gfx: %s", text);
}

static void lock_init(void) {
    if (InterlockedCompareExchange(&g_cs_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs);
        InterlockedExchange(&g_cs_state, 2);
    }
    while (g_cs_state != 2) Sleep(0);
}

static int write_slot(void **slot, void *value) {
    DWORD old, ignored;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer((void * volatile *)slot, value);
    VirtualProtect(slot, sizeof(*slot), old, &ignored);
    return 1;
}

static void *patch_slot(void **slot, void *ours, const char *what) {
    int i;
    void *old;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_patch_count; ++i) {
        if (g_patches[i].slot == slot) {
            old = g_patches[i].old_value;
            LeaveCriticalSection(&g_cs);
            return old;
        }
    }
    old = *slot;
    if (old == ours) { LeaveCriticalSection(&g_cs); return NULL; }
    if (g_patch_count >= MAX_PATCHES || !write_slot(slot, ours)) {
        LeaveCriticalSection(&g_cs);
        InterlockedIncrement(&g_stats.errors);
        log_msg("ERROR: could not patch %s at %p", what, (void *)slot);
        return NULL;
    }
    g_patches[g_patch_count].slot = slot;
    g_patches[g_patch_count].old_value = old;
    g_patches[g_patch_count].our_value = ours;
    g_patch_count++;
    LeaveCriticalSection(&g_cs);
    log_msg("patched %s: slot %p %p -> %p", what, (void *)slot, old, ours);
    return old;
}

static void restore_all(void) {
    EnterCriticalSection(&g_cs);
    while (g_patch_count > 0) {
        PATCH *p = &g_patches[--g_patch_count];
        
        if (*p->slot == p->our_value) write_slot(p->slot, p->old_value);
        p->slot = NULL;
    }
    g_iat_gpa = NULL;
    LeaveCriticalSection(&g_cs);
}

static void **find_import_slot(const char *dll, const char *name) {
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_DATA_DIRECTORY dir;
    IMAGE_IMPORT_DESCRIPTOR *desc;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return NULL;
    dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return NULL;
    for (desc = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); desc->Name; ++desc) {
        IMAGE_THUNK_DATA64 *names, *addrs;
        if (_stricmp((const char *)(base + desc->Name), dll) != 0) continue;
        names = (IMAGE_THUNK_DATA64 *)(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        addrs = (IMAGE_THUNK_DATA64 *)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addrs) {
            IMAGE_IMPORT_BY_NAME *imp;
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            imp = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)imp->Name, name) == 0) return (void **)&addrs->u1.Function;
        }
    }
    return NULL;
}

static const char *format_name(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
    default: return "other";
    }
}

static const char *effect_name(DXGI_SWAP_EFFECT e) {
    switch (e) {
    case DXGI_SWAP_EFFECT_DISCARD: return "DISCARD";
    case DXGI_SWAP_EFFECT_SEQUENTIAL: return "SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_DISCARD: return "FLIP_DISCARD";
    default: return "other";
    }
}

static void log_window(HWND hwnd) {
    RECT c = {0}, w = {0};
    char title[128] = "";
    WCHAR wtitle[128] = L"";
    if (!hwnd) { log_msg("  window: none (composition / core window)"); return; }
    
    GetClientRect(hwnd, &c);
    GetWindowRect(hwnd, &w);
    InternalGetWindowText(hwnd, wtitle, 128);
    WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, title, sizeof(title), NULL, NULL);
    log_msg("  window %p \"%s\" client %ldx%ld outer %ldx%ld", (void *)hwnd, title,
            c.right - c.left, c.bottom - c.top, w.right - w.left, w.bottom - w.top);
}

static void log_adapter(IDXGIAdapter *adapter, const char *prefix) {
    DXGI_ADAPTER_DESC d;
    char name[128];
    if (!adapter) { log_msg("  %s adapter: default (NULL)", prefix); return; }
    if (FAILED(IDXGIAdapter_GetDesc(adapter, &d))) return;
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), NULL, NULL);
    log_msg("  %s adapter: \"%s\" vendor %04x device %04x vram %llu MB", prefix, name,
            d.VendorId, d.DeviceId, (unsigned long long)(d.DedicatedVideoMemory >> 20));
}

static int cmp_ll(const void *a, const void *b) {
    LONGLONG x = *(const LONGLONG *)a, y = *(const LONGLONG *)b;
    return x < y ? -1 : x > y;
}

static void on_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    (void)sync;
    if(flags & DXGI_PRESENT_TEST)return;
    {void (*cb)(void *,unsigned)=g_present_cb;if(cb && sc==g_swapchain)cb(sc,g_stats.device_flags_used);}
    InterlockedIncrement(&g_stats.presents);
}

static HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (!t_inside && !g_stopping) { t_inside = 1; on_present(sc, sync, flags); t_inside = 0; }
    return g_real_present(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1 *sc, UINT sync, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS *params) {
    if (!t_inside && !g_stopping) { t_inside = 1; on_present((IDXGISwapChain *)sc, sync, flags); t_inside = 0; }
    return g_real_present1(sc, sync, flags, params);
}

static HRESULT STDMETHODCALLTYPE hook_resize(IDXGISwapChain *sc, UINT count, UINT w, UINT h,
                                             DXGI_FORMAT fmt, UINT flags) {
    HRESULT hr = g_real_resize(sc, count, w, h, fmt, flags);
    InterlockedIncrement(&g_stats.resizes);
    log_msg("ResizeBuffers(%p): %ux%u %s x%u flags 0x%x -> 0x%08lx", (void *)sc, w, h,
            format_name(fmt), count, flags, (unsigned long)hr);
    return hr;
}

static void identify_backend(IUnknown *device_arg, IDXGISwapChain *sc) {
    ID3D11Device *d11 = NULL;
    ID3D12CommandQueue *q12 = NULL;
    ID3D12Device *d12 = NULL;
    if (sc && SUCCEEDED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&d11)) && d11) {
        D3D_FEATURE_LEVEL fl = ID3D11Device_GetFeatureLevel(d11);
        log_msg("  backend: D3D11 (swapchain device %p, feature level 0x%x)", (void *)d11, fl);
        g_stats.backend = 11;
        ID3D11Device_Release(d11);
        return;
    }
    if (device_arg && SUCCEEDED(IUnknown_QueryInterface(device_arg, &IID_ID3D12CommandQueue, (void **)&q12)) && q12) {
        log_msg("  backend: D3D12 (swapchain created on command queue %p)", (void *)q12);
        g_stats.backend = 12;
        ID3D12CommandQueue_Release(q12);
        return;
    }
    if (sc && SUCCEEDED(IDXGISwapChain_GetDevice(sc, &IID_ID3D12Device, (void **)&d12)) && d12) {
        log_msg("  backend: D3D12 (swapchain device %p)", (void *)d12);
        g_stats.backend = 12;
        ID3D12Device_Release(d12);
        return;
    }
    log_msg("  backend: could not identify from swapchain %p / device arg %p", (void *)sc, (void *)device_arg);
}

static void adopt_swapchain(IDXGISwapChain *sc, IUnknown *device_arg) {
    IDXGISwapChain1 *sc1 = NULL;
    void *old;
    InterlockedIncrement(&g_stats.swapchains);
    if (!g_swapchain) { IDXGISwapChain_AddRef(sc); g_swapchain = sc; }
    identify_backend(device_arg, sc);
    old = patch_slot((void **)&sc->lpVtbl->Present, (void *)hook_present, "IDXGISwapChain::Present");
    if (old) g_real_present = (PRESENT_FN)old;
    old = patch_slot((void **)&sc->lpVtbl->ResizeBuffers, (void *)hook_resize, "IDXGISwapChain::ResizeBuffers");
    if (old) g_real_resize = (RESIZE_FN)old;
    if (SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain1, (void **)&sc1)) && sc1) {
        old = patch_slot((void **)&sc1->lpVtbl->Present1, (void *)hook_present1, "IDXGISwapChain1::Present1");
        if (old) g_real_present1 = (PRESENT1_FN)old;
        IDXGISwapChain1_Release(sc1);
    }
}

static HRESULT STDMETHODCALLTYPE hook_create_sc(IDXGIFactory *f, IUnknown *device,
                                                DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **sc) {
    HRESULT hr = g_real_create_sc(f, device, desc, sc);
    log_msg("CreateSwapChain(factory %p, device %p) -> 0x%08lx", (void *)f, (void *)device, (unsigned long)hr);
    if (desc) {
        log_msg("  desc %ux%u %s x%u %s refresh %u/%u samples %u windowed %d flags 0x%x usage 0x%x",
                desc->BufferDesc.Width, desc->BufferDesc.Height, format_name(desc->BufferDesc.Format),
                desc->BufferCount, effect_name(desc->SwapEffect), desc->BufferDesc.RefreshRate.Numerator,
                desc->BufferDesc.RefreshRate.Denominator, desc->SampleDesc.Count, desc->Windowed,
                desc->Flags, desc->BufferUsage);
        log_window(desc->OutputWindow);
    }
    if (SUCCEEDED(hr) && sc && *sc) adopt_swapchain(*sc, device);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_create_sc_hwnd(IDXGIFactory2 *f, IUnknown *device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *out,
    IDXGISwapChain1 **sc) {
    HRESULT hr = g_real_create_sc_hwnd(f, device, hwnd, desc, fs, out, sc);
    log_msg("CreateSwapChainForHwnd(factory %p, device %p) -> 0x%08lx", (void *)f, (void *)device, (unsigned long)hr);
    if (desc) {
        log_msg("  desc1 %ux%u %s x%u %s samples %u stereo %d flags 0x%x usage 0x%x scaling %d alpha %d",
                desc->Width, desc->Height, format_name(desc->Format), desc->BufferCount,
                effect_name(desc->SwapEffect), desc->SampleDesc.Count, desc->Stereo, desc->Flags,
                desc->BufferUsage, (int)desc->Scaling, (int)desc->AlphaMode);
        if (fs) log_msg("  fullscreen desc: windowed %d refresh %u/%u", fs->Windowed,
                        fs->RefreshRate.Numerator, fs->RefreshRate.Denominator);
        log_window(hwnd);
    }
    if (SUCCEEDED(hr) && sc && *sc) adopt_swapchain((IDXGISwapChain *)*sc, device);
    return hr;
}

static void adopt_factory(IUnknown *obj, const char *via) {
    IDXGIFactory *f = NULL;
    IDXGIFactory2 *f2 = NULL;
    void *old;
    InterlockedIncrement(&g_stats.factories);
    if (SUCCEEDED(IUnknown_QueryInterface(obj, &IID_IDXGIFactory, (void **)&f)) && f) {
        old = patch_slot((void **)&f->lpVtbl->CreateSwapChain, (void *)hook_create_sc, "IDXGIFactory::CreateSwapChain");
        if (old) g_real_create_sc = (CREATE_SC_FN)old;
        IDXGIFactory_Release(f);
    }
    if (SUCCEEDED(IUnknown_QueryInterface(obj, &IID_IDXGIFactory2, (void **)&f2)) && f2) {
        old = patch_slot((void **)&f2->lpVtbl->CreateSwapChainForHwnd, (void *)hook_create_sc_hwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
        if (old) g_real_create_sc_hwnd = (CREATE_SC_HWND_FN)old;
        IDXGIFactory2_Release(f2);
    } else {
        log_msg("  factory via %s has no IDXGIFactory2 (pre-DXGI 1.2)", via);
    }
}

static HRESULT WINAPI hook_create_factory(REFIID iid, void **out) {
    HRESULT hr = g_real_factory(iid, out);
    log_msg("CreateDXGIFactory -> 0x%08lx obj %p", (unsigned long)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) adopt_factory((IUnknown *)*out, "CreateDXGIFactory");
    return hr;
}

static HRESULT WINAPI hook_create_factory1(REFIID iid, void **out) {
    HRESULT hr = g_real_factory1(iid, out);
    log_msg("CreateDXGIFactory1 -> 0x%08lx obj %p", (unsigned long)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) adopt_factory((IUnknown *)*out, "CreateDXGIFactory1");
    return hr;
}

static HRESULT WINAPI hook_create_factory2(UINT flags, REFIID iid, void **out) {
    HRESULT hr = g_real_factory2(flags, iid, out);
    log_msg("CreateDXGIFactory2(flags 0x%x) -> 0x%08lx obj %p", flags, (unsigned long)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) adopt_factory((IUnknown *)*out, "CreateDXGIFactory2");
    return hr;
}

static void log_levels(const D3D_FEATURE_LEVEL *levels, UINT n) {
    char buf[160] = "";
    UINT i;
    size_t len = 0;
    if (!levels) { log_msg("  requested levels: default"); return; }
    for (i = 0; i < n && len < sizeof(buf) - 8; ++i) {
        _snprintf_s(buf + len, sizeof(buf) - len, _TRUNCATE, "%s0x%x", i ? "," : "", levels[i]);
        len = strlen(buf);
    }
    log_msg("  requested levels: %s", buf);
}

static HRESULT WINAPI hook_d3d11_create(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw,
    UINT flags, const D3D_FEATURE_LEVEL *levels, UINT n, UINT sdk, ID3D11Device **device,
    D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx) {
    D3D_FEATURE_LEVEL fl = 0;
    UINT asked = flags;
    HRESULT hr;
    if (g_strip_singlethreaded) flags &= ~(UINT)D3D11_CREATE_DEVICE_SINGLETHREADED;
    hr = g_real_d3d11(adapter, type, sw, flags, levels, n, sdk, device, got ? got : &fl, ctx);
    log_msg("D3D11CreateDevice(type %d flags 0x%x%s sdk %u) -> 0x%08lx device %p ctx %p level 0x%x",
            (int)type, asked, asked != flags ? " -> SINGLETHREADED stripped for XR" : "", sdk, (unsigned long)hr,
            device ? (void *)*device : NULL, ctx ? (void *)*ctx : NULL, got ? *got : fl);
    if (SUCCEEDED(hr) && device && *device && !g_stats.d3d11_devices) { g_stats.device_flags_asked = asked; g_stats.device_flags_used = flags; }
    log_adapter(adapter, "d3d11");
    log_levels(levels, n);
    if (SUCCEEDED(hr) && device && *device) { InterlockedIncrement(&g_stats.d3d11_devices); if (!g_stats.backend) g_stats.backend = 11; }
    return hr;
}

static HRESULT WINAPI hook_d3d11_create_sc(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw,
    UINT flags, const D3D_FEATURE_LEVEL *levels, UINT n, UINT sdk, const DXGI_SWAP_CHAIN_DESC *desc,
    IDXGISwapChain **sc, ID3D11Device **device, D3D_FEATURE_LEVEL *got, ID3D11DeviceContext **ctx) {
    UINT asked = flags;
    HRESULT hr;
    if (g_strip_singlethreaded) flags &= ~(UINT)D3D11_CREATE_DEVICE_SINGLETHREADED;
    hr = g_real_d3d11_sc(adapter, type, sw, flags, levels, n, sdk, desc, sc, device, got, ctx);
    log_msg("D3D11CreateDeviceAndSwapChain(type %d flags 0x%x%s) -> 0x%08lx device %p sc %p",
            (int)type, asked, asked != flags ? " -> SINGLETHREADED stripped for XR" : "", (unsigned long)hr,
            device ? (void *)*device : NULL, sc ? (void *)*sc : NULL);
    if (SUCCEEDED(hr) && device && *device && !g_stats.d3d11_devices) { g_stats.device_flags_asked = asked; g_stats.device_flags_used = flags; }
    log_adapter(adapter, "d3d11");
    if (SUCCEEDED(hr) && device && *device) { InterlockedIncrement(&g_stats.d3d11_devices); if (!g_stats.backend) g_stats.backend = 11; }
    if (SUCCEEDED(hr) && sc && *sc) {
        if (desc) log_window(desc->OutputWindow);
        adopt_swapchain(*sc, device ? (IUnknown *)*device : NULL);
    }
    return hr;
}

static HRESULT WINAPI hook_d3d12_create(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **out) {
    HRESULT hr = g_real_d3d12(adapter, level, iid, out);
    log_msg("D3D12CreateDevice(level 0x%x) -> 0x%08lx device %p", level, (unsigned long)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) { InterlockedIncrement(&g_stats.d3d12_devices); g_stats.backend = 12; }
    return hr;
}

typedef struct { const char *name; void *hook; void **real; } ENTRY;

static ENTRY g_entries[] = {
    { "D3D11CreateDevice",             (void *)hook_d3d11_create,    (void **)&g_real_d3d11 },
    { "D3D11CreateDeviceAndSwapChain", (void *)hook_d3d11_create_sc, (void **)&g_real_d3d11_sc },
    { "D3D12CreateDevice",             (void *)hook_d3d12_create,    (void **)&g_real_d3d12 },
    { "CreateDXGIFactory",             (void *)hook_create_factory,  (void **)&g_real_factory },
    { "CreateDXGIFactory1",            (void *)hook_create_factory1, (void **)&g_real_factory1 },
    { "CreateDXGIFactory2",            (void *)hook_create_factory2, (void **)&g_real_factory2 },
};

static FARPROC WINAPI hook_gpa(HMODULE module, LPCSTR name) {
    FARPROC real = g_real_gpa(module, name);
    size_t i;
    InterlockedIncrement(&g_stats.gpa_calls);
    if (g_stopping || !real || !name || HIWORD((ULONG_PTR)name) == 0) return real;
    for (i = 0; i < sizeof(g_entries) / sizeof(g_entries[0]); ++i) {
        void *prev;
        if (strcmp(name, g_entries[i].name) != 0) continue;
        
        prev = InterlockedCompareExchangePointer(g_entries[i].real, (void *)real, NULL);
        if (prev != NULL && prev != (void *)real) {
            log_msg("GetProcAddress(%s) resolved to a DIFFERENT address %p (kept %p) - passing through",
                    name, (void *)real, prev);
            return real;
        }
        InterlockedIncrement(&g_stats.gpa_wrapped);
        log_msg("GetProcAddress(%p, \"%s\") = %p -> wrapper %p", (void *)module, name, (void *)real, g_entries[i].hook);
        return (FARPROC)g_entries[i].hook;
    }
    if (strstr(name, "vkCreateInstance") || strstr(name, "vkGetInstanceProcAddr"))
        log_msg("GetProcAddress(\"%s\") requested - Vulkan loader activity", name);
    return real;
}

int mgs4vr_gfx_start(void (*log)(const char *fmt, ...), int stats_period_ms) {
    void **slot;
    void *old;
    lock_init();
    if (InterlockedCompareExchange(&g_started, 1, 0)) return 1;
    g_log = log;
    g_stopping = 0;
    if (stats_period_ms > 0) g_stats_period_ms = stats_period_ms;
    slot = find_import_slot("KERNEL32.dll", "GetProcAddress");
    if (!slot) {
        log_msg("ERROR: KERNEL32!GetProcAddress not found in the host import table");
        InterlockedIncrement(&g_stats.errors);
        g_started = 0;
        return 0;
    }
    old = patch_slot(slot, (void *)hook_gpa, "IAT KERNEL32!GetProcAddress");
    if (!old) { g_started = 0; return 0; }
    g_real_gpa = (GPA_FN)old;
    g_stats.real_get_proc_address = old;
    EnterCriticalSection(&g_cs);
    g_iat_gpa = &g_patches[g_patch_count - 1];
    LeaveCriticalSection(&g_cs);
    log_msg("started: watching GetProcAddress for %u graphics entry points, cadence report every %d ms",
            (unsigned)(sizeof(g_entries) / sizeof(g_entries[0])), g_stats_period_ms);
    return 1;
}

int mgs4vr_gfx_verify(void) {
    int repairs = 0;
    if (!g_started || g_stopping) return 0;
    EnterCriticalSection(&g_cs);
    if (g_iat_gpa && g_iat_gpa->slot && *g_iat_gpa->slot != g_iat_gpa->our_value) {
        void *now = *g_iat_gpa->slot;
        
        if (now && now != g_iat_gpa->old_value) { g_iat_gpa->old_value = now; g_real_gpa = (GPA_FN)now; }
        if (write_slot(g_iat_gpa->slot, g_iat_gpa->our_value)) {
            repairs++;
            InterlockedIncrement(&g_stats.iat_repairs);
        }
    }
    LeaveCriticalSection(&g_cs);
    if (repairs) log_msg("IAT slot had been overwritten (now chaining to %p); re-patched", (void *)g_real_gpa);
    return repairs;
}

void mgs4vr_gfx_stop(void) {
    if (!g_started) return;
    g_stopping = 1;
    restore_all();
    
    if (g_swapchain) { IDXGISwapChain_Release(g_swapchain); g_swapchain = NULL; }
    log_msg("stopped: %ld presents, %ld swapchains, %ld d3d11 devices, %ld d3d12 devices, %ld iat repairs, %ld errors",
            g_stats.presents, g_stats.swapchains, g_stats.d3d11_devices, g_stats.d3d12_devices,
            g_stats.iat_repairs, g_stats.errors);
    g_started = 0;
}

void mgs4vr_gfx_get_stats(MGS4VR_GFX_STATS *out) { *out = g_stats; }
unsigned long mgs4vr_gfx_frame(void) { return (unsigned long)g_stats.presents; }
void mgs4vr_gfx_set_options(int strip_singlethreaded) { InterlockedExchange(&g_strip_singlethreaded, strip_singlethreaded ? 1 : 0); }
void mgs4vr_gfx_set_present_callback(void (*cb)(void *dxgi_swapchain, unsigned device_flags)) { g_present_cb = cb; }
