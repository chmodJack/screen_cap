// screen_cap - 动态选区屏幕捕获 (D3D11 Desktop Duplication 版本)
//
// 两个窗口:
//   [选区窗口] 一个"空心"细线框: 中间挖洞(SetWindowRgn) -> 桌面透出、鼠标穿透、
//             抓屏时拍到的是洞后面的桌面而不是边框本身。边框环用纯红色填充。
//             只能拖边/拖角缩放(左上角可整体拖动)。
//   [预览窗口] 实时显示线框内部区域, 自动跟随选区大小变化。
//
// 捕获用 D3D11 Desktop Duplication API, 直接从 GPU 获取桌面帧。
// 光标由 API 原生提供位置 + 形状数据, 软件合成到帧上。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <timeapi.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <vector>
#include <shellapi.h>
#include "plugin_api.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winmm.lib")

// ======================= 可配置参数 =======================
static int g_targetFps = 120;

static const int B    = 2;
static const int GRAB = 10;
static const int MINW = 40;
static const int MINH = 40;

static int g_initRx = 300, g_initRy = 200, g_initRw = 640, g_initRh = 360;

static wchar_t g_pluginName[MAX_PATH] = L"plugin.dll";
// =========================================================

// ---- 外部插件 (DLL) ----
static HMODULE                g_plugin   = nullptr;
static screencap_on_init_fn   g_on_init  = nullptr;
static screencap_on_frame_fn  g_on_frame = nullptr;
static screencap_on_exit_fn   g_on_exit  = nullptr;

static bool loadPlugin() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), g_pluginName);
    g_plugin = LoadLibraryW(path);
    if (!g_plugin) g_plugin = LoadLibraryW(g_pluginName);
    if (!g_plugin) return false;
    g_on_init  = (screencap_on_init_fn)  GetProcAddress(g_plugin, "on_init");
    g_on_frame = (screencap_on_frame_fn) GetProcAddress(g_plugin, "on_frame");
    g_on_exit  = (screencap_on_exit_fn)  GetProcAddress(g_plugin, "on_exit");
    return true;
}

static void unloadPlugin() {
    if (g_on_exit) g_on_exit();
    g_on_init = nullptr; g_on_frame = nullptr; g_on_exit = nullptr;
    if (g_plugin) { FreeLibrary(g_plugin); g_plugin = nullptr; }
}

struct Frame {
    const uint8_t* data;
    int     width;
    int     height;
    int     stride;
    int64_t timestampMs;
};

static int64_t nowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return t.QuadPart * 1000 / f.QuadPart;
}

// ============================================================
//  D3D11 Desktop Duplication 捕获引擎
// ============================================================
class D3D11Capture {
    ID3D11Device*             device     = nullptr;
    ID3D11DeviceContext*      ctx        = nullptr;
    IDXGIOutputDuplication*   dupl       = nullptr;
    ID3D11Texture2D*          stagingTex = nullptr;
    int                       stageW = 0, stageH = 0;

    HMONITOR curMonitor = nullptr;
    int      monX = 0, monY = 0, monW = 0, monH = 0;

    bool frameAcquired = false;
    bool mapped = false;

    const uint8_t* frameBits  = nullptr;
    int            frameW     = 0;
    int            frameH     = 0;
    int            frameStride = 0;

    void destroyDupl() {
        done();
        if (dupl) { dupl->Release(); dupl = nullptr; }
        curMonitor = nullptr;
    }

    void destroyStaging() {
        if (stagingTex) { stagingTex->Release(); stagingTex = nullptr; }
        stageW = stageH = 0;
    }

    bool createDuplForMonitor(HMONITOR hmon, int mx, int my, int mw, int mh) {
        destroyDupl();
        curMonitor = hmon;
        monX = mx; monY = my; monW = mw; monH = mh;

        IDXGIDevice*  dxgiDev  = nullptr;
        IDXGIAdapter* adapter  = nullptr;
        IDXGIOutput*  output   = nullptr;
        IDXGIOutput1* output1  = nullptr;
        bool ok = false;

        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) goto fail;
        if (FAILED(dxgiDev->GetParent(IID_PPV_ARGS(&adapter))))    goto fail;

        for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            if (desc.Monitor == hmon) {
                if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output1))) &&
                    SUCCEEDED(output1->DuplicateOutput(device, &dupl))) {
                    ok = true;
                }
                if (output1) output1->Release();
                output->Release();
                break;
            }
            output->Release();
            output = nullptr;
        }

    fail:
        if (adapter) adapter->Release();
        if (dxgiDev) dxgiDev->Release();
        if (!ok) { curMonitor = nullptr; return false; }
        return true;
    }

    bool ensureStaging(int w, int h) {
        if (stagingTex && stageW == w && stageH == h) return true;
        destroyStaging();
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width          = (UINT)w;
        desc.Height         = (UINT)h;
        desc.MipLevels      = 1;
        desc.ArraySize      = 1;
        desc.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage          = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags      = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &stagingTex)))
            return false;
        stageW = w; stageH = h;
        return true;
    }

public:
    D3D11Capture() = default;
    ~D3D11Capture() { release(); }

    bool init() {
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D_FEATURE_LEVEL selected;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                        nullptr, flags, levels,
                                        ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                        &device, &selected, &ctx);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP,
                                    nullptr, flags, levels,
                                    ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                    &device, &selected, &ctx);
        }
        return SUCCEEDED(hr);
    }

    bool grab(int roiX, int roiY, int roiW, int roiH) {
        done();

        if (roiW < 1) roiW = 1;
        if (roiH < 1) roiH = 1;

        // 检查选区是否移到了另一个显示器
        POINT center = { roiX + roiW / 2, roiY + roiH / 2 };
        HMONITOR hmon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        if (hmon != curMonitor) {
            MONITORINFOEXW mi = { sizeof(MONITORINFOEXW) };
            GetMonitorInfoW(hmon, &mi);
            if (!createDuplForMonitor(hmon,
                                      mi.rcMonitor.left, mi.rcMonitor.top,
                                      mi.rcMonitor.right  - mi.rcMonitor.left,
                                      mi.rcMonitor.bottom - mi.rcMonitor.top)) {
                return false;
            }
        }

        if (!dupl) return false;
        if (!ensureStaging(roiW, roiH)) return false;

        IDXGIResource* desktopRes = nullptr;
        DXGI_OUTDUPL_FRAME_INFO fi = {};
        HRESULT hr = dupl->AcquireNextFrame(16, &fi, &desktopRes);
        if (FAILED(hr)) {
            // DXGI_ERROR_ACCESS_LOST: 分辨率/显示模式变更, 需要重建 dupl
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
                || hr == DXGI_ERROR_ACCESS_LOST)
                destroyDupl();
            return false;
        }
        frameAcquired = true;

        // 获取桌面纹理
        ID3D11Texture2D* desktopTex = nullptr;
        if (FAILED(desktopRes->QueryInterface(IID_PPV_ARGS(&desktopTex)))) {
            desktopRes->Release();
            done();
            return false;
        }

        int offsetX = roiX - monX;
        int offsetY = roiY - monY;

        D3D11_BOX box = {
            (UINT)max(0, offsetX),
            (UINT)max(0, offsetY),
            0,
            (UINT)min(offsetX + roiW, monW),
            (UINT)min(offsetY + roiH, monH),
            1
        };
        int actualW = (int)(box.right  - box.left);
        int actualH = (int)(box.bottom - box.top);
        if (actualW < 1) actualW = 1;
        if (actualH < 1) actualH = 1;

        ctx->CopySubresourceRegion(stagingTex, 0, 0, 0, 0,
                                    desktopTex, 0, &box);
        desktopTex->Release();
        desktopRes->Release();

        // 映射到 CPU
        D3D11_MAPPED_SUBRESOURCE ms = {};
        if (FAILED(ctx->Map(stagingTex, 0, D3D11_MAP_READ, 0, &ms))) {
            done();
            return false;
        }
        mapped = true;

        frameBits   = (const uint8_t*)ms.pData;
        frameStride = (int)ms.RowPitch;
        frameW      = actualW;
        frameH      = actualH;

        return true;
    }

    void done() {
        if (mapped && ctx && stagingTex) {
            ctx->Unmap(stagingTex, 0);
            mapped = false;
        }
        if (frameAcquired && dupl) {
            dupl->ReleaseFrame();
            frameAcquired = false;
        }
        frameBits = nullptr;
    }

    void release() {
        done();
        destroyStaging();
        if (dupl) { dupl->Release(); dupl = nullptr; }
        if (ctx)    { ctx->Release();    ctx = nullptr; }
        if (device) { device->Release(); device = nullptr; }
        curMonitor = nullptr;
    }

    const uint8_t* bits()   const { return frameBits; }
    int width()             const { return frameW; }
    int height()            const { return frameH; }
    int stride()            const { return frameStride; }
};

static D3D11Capture            g_cap;
static HWND                    g_selector = nullptr;
static HWND                    g_preview  = nullptr;

static std::vector<uint8_t>    g_displayBuf;
static int                     g_dispW = 0, g_dispH = 0;

// ============================================================
//  每抓到一帧调用: 转交给外部插件 DLL 的 on_frame
// ============================================================
static void onFrame(const Frame& f) {
    if (!g_on_frame) return;
    ScreenFrame sf;
    sf.data        = const_cast<uint8_t*>(f.data);
    sf.width       = f.width;
    sf.height      = f.height;
    sf.stride      = f.stride;
    sf.timestampMs = f.timestampMs;
    g_on_frame(&sf);
}

static void blitToWindow(HWND hwnd, HDC hdc) {
    if (g_displayBuf.empty()) return;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_dispW;
    bi.bmiHeader.biHeight      = -g_dispH;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, 0, 0, cw, ch,
                  0, 0, g_dispW, g_dispH,
                  g_displayBuf.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

// ---- GDI 光标绘制 (预览显示用) ----
// 用 GetCursorInfo + DrawIconEx, 绕过 Desktop Duplication 光标 API 的兼容性问题。
static HDC      g_cursorDC  = nullptr;
static HBITMAP  g_cursorDIB = nullptr;
static uint8_t* g_cursorBits = nullptr;
static int      g_cursorBufW = 0, g_cursorBufH = 0;

static void ensureCursorDIB(int w, int h) {
    if (g_cursorDC && g_cursorBufW == w && g_cursorBufH == h) return;
    if (g_cursorDIB) DeleteObject(g_cursorDIB);
    if (g_cursorDC)  DeleteDC(g_cursorDC);
    g_cursorDC = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_cursorDIB = CreateDIBSection(g_cursorDC, &bi, DIB_RGB_COLORS,
                                    (void**)&g_cursorBits, nullptr, 0);
    SelectObject(g_cursorDC, g_cursorDIB);
    g_cursorBufW = w; g_cursorBufH = h;
}

static void drawCursorOnDisplay(uint8_t* buf, int w, int h, int stride,
                                 int roiScreenX, int roiScreenY) {
    CURSORINFO ci = { sizeof(CURSORINFO) };
    GetCursorInfo(&ci);
    if (!(ci.flags & CURSOR_SHOWING)) return;
    ensureCursorDIB(w, h);
    if (!g_cursorBits) return;

    // buf → temp DIB
    const uint8_t* src = buf;
    for (int y = 0; y < h; ++y) {
        memcpy(g_cursorBits + y * w * 4, src, (size_t)w * 4);
        src += stride;
    }

    DrawIconEx(g_cursorDC,
               ci.ptScreenPos.x - roiScreenX,
               ci.ptScreenPos.y - roiScreenY,
               ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);

    // temp DIB → buf
    uint8_t* dst = buf;
    for (int y = 0; y < h; ++y) {
        memcpy(dst, g_cursorBits + y * w * 4, (size_t)w * 4);
        dst += stride;
    }
}

static void captureAndShow() {
    if (!g_selector || !g_preview) return;
    RECT wr; GetWindowRect(g_selector, &wr);
    int x = wr.left + B;
    int y = wr.top  + B;
    int w = (wr.right  - B) - x;
    int h = (wr.bottom - B) - y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (!g_cap.grab(x, y, w, h)) return;

    Frame f{ g_cap.bits(), g_cap.width(), g_cap.height(), g_cap.stride(), nowMs() };
    onFrame(f);

    int dstStride = g_cap.width() * 4;
    g_displayBuf.resize(g_cap.height() * dstStride);
    const uint8_t* src = g_cap.bits();
    uint8_t* dst = g_displayBuf.data();
    for (int row = 0; row < g_cap.height(); ++row) {
        memcpy(dst, src, dstStride);
        src += g_cap.stride();
        dst += dstStride;
    }
    g_dispW = g_cap.width();
    g_dispH = g_cap.height();

    // 用 GDI 绘制光标到显示缓冲 (ROI 屏幕原点 = x, y)
    drawCursorOnDisplay(g_displayBuf.data(), g_dispW, g_dispH, dstStride, x, y);

    g_cap.done();

    if (g_dispW > 0 && g_dispH > 0) {
        RECT pr = { 0, 0, g_dispW, g_dispH };
        AdjustWindowRect(&pr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(g_preview, nullptr, 0, 0,
                     pr.right - pr.left, pr.bottom - pr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    InvalidateRect(g_preview, nullptr, FALSE);
    UpdateWindow(g_preview);
}

static void updateRegion(HWND hwnd) {
    RECT c; GetClientRect(hwnd, &c);
    int W = c.right, H = c.bottom;
    HRGN outer = CreateRectRgn(0, 0, W, H);
    HRGN hole  = CreateRectRgn(B, B, W - B, H - B);
    CombineRgn(outer, outer, hole, RGN_DIFF);
    SetWindowRgn(hwnd, outer, TRUE);
    DeleteObject(hole);
}

// ---------------- 选区线框窗口过程 ----------------
LRESULT CALLBACK SelectorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        RECT c; GetClientRect(hwnd, &c);
        int W = c.right, H = c.bottom;
        bool L  = pt.x < GRAB;
        bool R  = pt.x > W - GRAB;
        bool T  = pt.y < GRAB;
        bool Bo = pt.y > H - GRAB;
        if (T && L) return HTCAPTION;
        if (T && R) return HTTOPRIGHT;
        if (Bo && L) return HTBOTTOMLEFT;
        if (Bo && R) return HTBOTTOMRIGHT;
        if (pt.x < B)     return HTLEFT;
        if (pt.x > W - B) return HTRIGHT;
        if (pt.y < B)     return HTTOP;
        if (pt.y > H - B) return HTBOTTOM;
        return HTNOWHERE;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = MINW + 2 * B;
        mmi->ptMinTrackSize.y = MINH + 2 * B;
        return 0;
    }
    case WM_SIZE:
        updateRegion(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOVE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT c; GetClientRect(hwnd, &c);
        HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(hdc, &c, red);
        DeleteObject(red);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        SetTimer(hwnd, 1, 15, nullptr);
        return 0;
    case WM_TIMER:
        captureAndShow();
        return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(hwnd, 1);
        captureAndShow();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---------------- 预览窗口过程 ----------------
LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        blitToWindow(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    // 第一个命令行参数作为插件 DLL 文件名
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(pCmdLine, &argc);
    if (argv && argc > 0) {
        wcscpy_s(g_pluginName, MAX_PATH, argv[0]);
        LocalFree(argv);
    }

    if (!g_cap.init()) {
        MessageBoxW(nullptr, L"D3D11 设备创建失败, 程序退出。", L"错误", MB_ICONERROR);
        return 1;
    }

    WNDCLASSW sc = {};
    sc.lpfnWndProc   = SelectorProc;
    sc.hInstance     = hInstance;
    sc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    sc.lpszClassName = L"ScreenCapSelector";
    RegisterClassW(&sc);

    WNDCLASSW pc = {};
    pc.lpfnWndProc   = PreviewProc;
    pc.hInstance     = hInstance;
    pc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    pc.lpszClassName = L"ScreenCapPreview";
    RegisterClassW(&pc);

    int sx = g_initRx - B;
    int sy = g_initRy - B;
    int sw = g_initRw + 2 * B;
    int sh = g_initRh + 2 * B;
    g_selector = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"ScreenCapSelector", L"selector",
        WS_POPUP | WS_VISIBLE,
        sx, sy, sw, sh,
        nullptr, nullptr, hInstance, nullptr);
    if (!g_selector) return 1;
    updateRegion(g_selector);

    RECT pr = { 0, 0, g_initRw, g_initRh };
    AdjustWindowRect(&pr, WS_OVERLAPPEDWINDOW, FALSE);
    g_preview = CreateWindowExW(
        WS_EX_TOPMOST,
        L"ScreenCapPreview", L"屏幕捕获预览",
        WS_OVERLAPPEDWINDOW,
        20, 20, pr.right - pr.left, pr.bottom - pr.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!g_preview) return 1;
    ShowWindow(g_preview, nCmdShow);

    if (!loadPlugin()) {
        wchar_t msg[256];
        swprintf(msg, 256, L"未找到插件 %s (识别功能禁用)。\n程序仍会正常显示捕获画面。", g_pluginName);
        MessageBoxW(g_preview, msg, L"提示", MB_ICONINFORMATION);
    } else if (g_on_init) {
        if (g_on_init() != 0) {
            MessageBoxW(g_preview, L"插件 on_init 返回失败, 程序退出。", L"错误", MB_ICONERROR);
            unloadPlugin();
            return 1;
        }
    }

    LARGE_INTEGER freq, last, fpsTimer;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    fpsTimer = last;
    const double frameInterval = (g_targetFps > 0) ? 1.0 / g_targetFps : 0.0;
    int frameCount = 0;

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = double(now.QuadPart - last.QuadPart) / freq.QuadPart;

        if (frameInterval == 0.0 || elapsed >= frameInterval) {
            last = now;
            captureAndShow();

            frameCount++;
            double sinceFps = double(now.QuadPart - fpsTimer.QuadPart) / freq.QuadPart;
            if (sinceFps >= 1.0) {
                double fps = frameCount / sinceFps;
                wchar_t title[160];
                swprintf(title, 160, L"屏幕捕获预览  [%dx%d]  %.1f FPS  (ESC 退出)",
                         g_dispW, g_dispH, fps);
                SetWindowTextW(g_preview, title);
                frameCount = 0;
                fpsTimer = now;
            }
        } else if (frameInterval - elapsed > 0.002) {
            Sleep(1);
        }
    }

    unloadPlugin();
    g_cap.release();
    if (g_cursorDIB) DeleteObject(g_cursorDIB);
    if (g_cursorDC)  DeleteDC(g_cursorDC);
    timeEndPeriod(1);
    return 0;
}
