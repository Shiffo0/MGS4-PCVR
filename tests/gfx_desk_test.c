/* gfx_desk_test.c - W01 acceptance: the renderer observer works without the
 * game, without an XR runtime and without any game memory.
 *
 * The test host plays MGS4's role: it resolves the D3D11/DXGI entry points
 * through KERNEL32!GetProcAddress exactly the way bgfx does, so its own IAT
 * slot is what mgs4vr_gfx patches. It then creates a real device and a
 * swapchain on a hidden window, presents 30 frames, and checks every
 * counter, then stops and checks that the IAT slot is restored.
 * Exit code 0 = pass. Any other value names the failed check on stdout. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>
#include "../src/mgs4vr_gfx.h"

typedef HRESULT (WINAPI *D3D11_CREATE_FN)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *CREATE_FACTORY_FN)(REFIID, void **);

static int g_log_lines;
static void test_log(const char *fmt, ...) {
    char text[900];
    va_list a;
    va_start(a, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, a);
    va_end(a);
    ++g_log_lines;
    printf("  log| %s\n", text);
}

static volatile long g_cb_calls; static void *g_cb_sc; static unsigned g_cb_flags;
static void present_cb(void *sc, unsigned flags) { ++g_cb_calls; g_cb_sc = sc; g_cb_flags = flags; }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

typedef struct { IDXGIFactory1 *factory; ID3D11Device *device; DXGI_SWAP_CHAIN_DESC *desc; IDXGISwapChain *sc; HRESULT hr; } SC_JOB;
static DWORD WINAPI sc_thread(LPVOID p) {
    SC_JOB *j = (SC_JOB *)p;
    j->hr = IDXGIFactory1_CreateSwapChain(j->factory, (IUnknown *)j->device, j->desc, &j->sc);
    return 0;
}

#define CHECK(cond, name) do { if (!(cond)) { printf("FAIL: %s\n", name); return 1; } printf("ok: %s\n", name); } while (0)

int main(void) {
    WNDCLASSA wc = {0};
    HWND hwnd;
    HMODULE d3d11, dxgi;
    D3D11_CREATE_FN create;
    CREATE_FACTORY_FN create_factory;
    FARPROC real_create, real_factory_fn;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGIFactory1 *factory = NULL;
    IDXGISwapChain *sc = NULL;
    DXGI_SWAP_CHAIN_DESC desc = {0};
    D3D_FEATURE_LEVEL got = 0;
    MGS4VR_GFX_STATS s;
    HRESULT hr;
    int i;

    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "mgs4vr_desk";
    RegisterClassA(&wc);
    hwnd = CreateWindowA("mgs4vr_desk", "mgs4vr desk test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
    CHECK(hwnd != NULL, "hidden window created");

    d3d11 = LoadLibraryA("d3d11.dll");
    dxgi = LoadLibraryA("dxgi.dll");
    CHECK(d3d11 && dxgi, "d3d11.dll and dxgi.dll loaded");
    real_create = GetProcAddress(d3d11, "D3D11CreateDevice");
    real_factory_fn = GetProcAddress(dxgi, "CreateDXGIFactory1");
    CHECK(real_create && real_factory_fn, "real exports resolved before start (baseline)");

    mgs4vr_gfx_set_options(1);                                  /* W05: XR requested -> strip SINGLETHREADED */
    mgs4vr_gfx_set_present_callback(present_cb);
    CHECK(mgs4vr_gfx_start(test_log, 100), "mgs4vr_gfx_start");
    mgs4vr_gfx_get_stats(&s);
    CHECK(s.real_get_proc_address != NULL, "IAT GetProcAddress slot captured");

    create = (D3D11_CREATE_FN)GetProcAddress(d3d11, "D3D11CreateDevice");
    create_factory = (CREATE_FACTORY_FN)GetProcAddress(dxgi, "CreateDXGIFactory1");
    CHECK((FARPROC)create != real_create, "D3D11CreateDevice request answered with wrapper");
    CHECK((FARPROC)create_factory != real_factory_fn, "CreateDXGIFactory1 request answered with wrapper");
    CHECK(GetProcAddress(GetModuleHandleA("kernel32.dll"), "Sleep") != NULL, "unrelated GetProcAddress passes through");
    mgs4vr_gfx_get_stats(&s);
    CHECK(s.gpa_wrapped == 2, "exactly two wrappers handed out");

    /* ask exactly what bgfx asks: SINGLETHREADED | BGRA_SUPPORT (0x21, measured in run 02) */
    hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                D3D11_SDK_VERSION, &device, &got, &ctx);
    CHECK(SUCCEEDED(hr) && device, "D3D11 device created through wrapper");
    CHECK((ID3D11Device_GetCreationFlags(device) & D3D11_CREATE_DEVICE_SINGLETHREADED) == 0 &&
          (ID3D11Device_GetCreationFlags(device) & D3D11_CREATE_DEVICE_BGRA_SUPPORT) != 0,
          "the device that came out is NOT single-threaded, the other flags are kept");
    mgs4vr_gfx_get_stats(&s);
    CHECK(s.device_flags_asked == 0x21 && s.device_flags_used == 0x20, "asked 0x21, used 0x20 recorded");
    hr = create_factory(&IID_IDXGIFactory1, (void **)&factory);
    CHECK(SUCCEEDED(hr) && factory, "DXGI factory created through wrapper");
    mgs4vr_gfx_get_stats(&s);
    CHECK(s.d3d11_devices == 1 && s.factories == 1 && s.backend == 11, "device/factory counted, backend D3D11");

    desc.BufferDesc.Width = 320; desc.BufferDesc.Height = 240;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    /* bgfx shape (live hang 2026-09-19): the swapchain is created on a render
       thread while the thread that owns the window is BLOCKED waiting for it
       and pumps no messages. Anything in the hook that sends a window message
       (GetWindowText, SendMessage...) deadlocks the game. */
    {
        SC_JOB job;
        HANDLE th;
        DWORD wait;
        job.factory = factory; job.device = device; job.desc = &desc; job.sc = NULL; job.hr = E_FAIL;
        th = CreateThread(NULL, 0, sc_thread, &job, 0, NULL);
        wait = WaitForSingleObject(th, 10000);
        if (wait != WAIT_OBJECT_0) {
            printf("FAIL: swapchain hook deadlocks when the window-owner thread is blocked\n");
            ExitProcess(2);
        }
        CloseHandle(th);
        hr = job.hr; sc = job.sc;
    }
    CHECK(SUCCEEDED(hr) && sc, "swapchain created on a render thread while the window owner is blocked");
    mgs4vr_gfx_get_stats(&s);
    CHECK(s.swapchains == 1, "swapchain counted");

    for (i = 0; i < 30; ++i) {
        hr = IDXGISwapChain_Present(sc, 0, 0);
        if (FAILED(hr)) { printf("FAIL: Present %d hr 0x%08lx\n", i, (unsigned long)hr); return 1; }
        Sleep(5);
    }
    IDXGISwapChain_Present(sc, 0, DXGI_PRESENT_TEST);
    mgs4vr_gfx_get_stats(&s);
    CHECK(s.presents == 30, "30 Presents counted, DXGI_PRESENT_TEST excluded");
    CHECK(g_cb_calls == 30 && g_cb_sc == (void *)sc && g_cb_flags == 0x20, "present callback: once per real Present, game swapchain, creation flags handed over");
    hr = IDXGISwapChain_ResizeBuffers(sc, 0, 400, 300, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr), "ResizeBuffers through patched vtable succeeds");
    mgs4vr_gfx_get_stats(&s);
    CHECK(s.resizes == 1, "ResizeBuffers counted");
    CHECK(mgs4vr_gfx_verify() == 0, "verify finds the IAT slot intact");
    CHECK(s.errors == 0, "no patch errors");
    CHECK(g_log_lines >= 10, "observation was logged");

    mgs4vr_gfx_stop();
    CHECK(GetProcAddress(d3d11, "D3D11CreateDevice") == real_create, "IAT slot restored: real export again");
    for (i = 0; i < 5; ++i) { hr = IDXGISwapChain_Present(sc, 0, 0); if (FAILED(hr)) { printf("FAIL: post-stop Present\n"); return 1; } }
    mgs4vr_gfx_get_stats(&s);
    CHECK(s.presents == 30, "vtable restored: post-stop Presents not counted");

    IDXGISwapChain_Release(sc);
    IDXGIFactory1_Release(factory);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(device);
    DestroyWindow(hwnd);
    printf("PASS: gfx_desk_test\n");
    return 0;
}
