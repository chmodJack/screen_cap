// screen_cap - 动态选区屏幕捕获 (纯 CPU / GDI 版本)
//
// 两个窗口:
//   [选区窗口] 一个"空心"细线框: 中间挖洞(SetWindowRgn) -> 桌面透出、鼠标穿透、
//             抓屏时拍到的是洞后面的桌面而不是边框本身。边框环用 NOTSRCCOPY 把
//             身后的桌面像素取反绘制 -> 反色线框。只能拖边/拖角缩放(无整体移动)。
//   [预览窗口] 实时显示线框内部区域, 自动跟随选区大小变化。
//
// 捕获用 GDI BitBlt(srcDC=屏幕)。如纯 CPU 不够再上 Desktop Duplication + D3D。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>              // GET_X_LPARAM / GET_Y_LPARAM
#include <timeapi.h>               // timeBeginPeriod / timeEndPeriod
#include <cstdio>                  // swprintf
#include <cstdint>                 // uint8_t / int64_t
#include <cwchar>                  // wcsrchr / wcscpy_s
#include "plugin_api.h"            // 外部识别插件 (DLL) 接口
#pragma comment(lib, "winmm.lib")

// ======================= 可配置参数 =======================
static int g_targetFps = 120;      // 预览目标帧率上限

static const int B    = 2;         // 边框线宽(像素), 越小越细
static const int GRAB = 10;        // 四角抓取判定长度(沿边方向)
static const int MINW = 40;        // 选区最小宽
static const int MINH = 40;        // 选区最小高

// 选区默认位置 / 大小 (内部捕获区域, 屏幕坐标)
static int g_initRx = 300, g_initRy = 200, g_initRw = 640, g_initRh = 360;

// 外部识别插件 DLL 文件名 (从 exe 所在目录加载)
static const wchar_t* g_pluginName = L"plugin.dll";
// =========================================================

// ---- 外部插件 (DLL) ----
static HMODULE                g_plugin   = nullptr;
static screencap_on_init_fn   g_on_init  = nullptr;
static screencap_on_frame_fn  g_on_frame = nullptr;
static screencap_on_exit_fn   g_on_exit  = nullptr;

// 从 exe 所在目录加载插件, 解析三个导出函数 (都可选)。
// 返回 false 表示 DLL 文件未能加载 (识别功能将被禁用, 程序仍可运行)。
static bool loadPlugin() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);     // exe 完整路径
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), g_pluginName);

    g_plugin = LoadLibraryW(path);
    if (!g_plugin) g_plugin = LoadLibraryW(g_pluginName);  // 退而求其次: 按搜索路径找
    if (!g_plugin) return false;

    g_on_init  = (screencap_on_init_fn)  GetProcAddress(g_plugin, "on_init");
    g_on_frame = (screencap_on_frame_fn) GetProcAddress(g_plugin, "on_frame");
    g_on_exit  = (screencap_on_exit_fn)  GetProcAddress(g_plugin, "on_exit");
    return true;
}

static void unloadPlugin() {
    if (g_on_exit) g_on_exit();          // 先让插件清理
    g_on_init = nullptr; g_on_frame = nullptr; g_on_exit = nullptr;
    if (g_plugin) { FreeLibrary(g_plugin); g_plugin = nullptr; }
}

// ---- 通用实时帧 (零依赖): 任何识别算法都从这里开始 ----
// 像素布局: BGRA, 8位/通道, top-down(第一行在前), 连续, stride = width*4
struct Frame {
    const uint8_t* data;        // 像素首地址 (BGRA)
    int     width;
    int     height;
    int     stride;             // 每行字节数
    int64_t timestampMs;
};

// ---- 捕获用的 GDI 资源 (尺寸变化时才重建) ----
// 用 CreateDIBSection 而非 CreateCompatibleBitmap: 抓完帧后像素直接可读(bits),
// 无需再 GetDIBits 拷贝 —— 既能显示, 又能零拷贝交给识别算法。
struct Capture {
    HDC     screenDC = nullptr;
    HDC     memDC    = nullptr;
    HBITMAP bmp      = nullptr;
    void*   bits     = nullptr;   // 直接指向 BGRA 像素
    int     w = 0, h = 0;

    bool init(int width, int height) {
        release();
        w = width; h = height;
        screenDC = GetDC(nullptr);
        if (!screenDC) return false;
        memDC = CreateCompatibleDC(screenDC);
        if (!memDC) return false;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;       // 负 = top-down
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;       // BGRA
        bi.bmiHeader.biCompression = BI_RGB;

        bmp = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) return false;
        SelectObject(memDC, bmp);
        return true;
    }
    void grab(int srcX, int srcY) {
        BitBlt(memDC, 0, 0, w, h, screenDC, srcX, srcY, SRCCOPY);
        // 抓完: (uint8_t*)bits 即本帧 BGRA 数据, stride = w*4
    }
    void release() {
        if (bmp)      { DeleteObject(bmp);            bmp = nullptr; bits = nullptr; }
        if (memDC)    { DeleteDC(memDC);              memDC = nullptr; }
        if (screenDC) { ReleaseDC(nullptr, screenDC); screenDC = nullptr; }
    }
    ~Capture() { release(); }
};

static Capture g_cap;
static HWND    g_selector = nullptr;   // 选区线框窗口
static HWND    g_preview  = nullptr;   // 预览窗口
static HDC     g_previewDC = nullptr;  // 复用的预览窗口 DC

// 毫秒时间戳 (用于给每帧打时间)
static int64_t nowMs() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return t.QuadPart * 1000 / f.QuadPart;
}

// ============================================================
//  每抓到一帧调用: 转交给外部插件 DLL 的 on_frame。
//  插件可就地修改像素(BGRA), 修改后的结果会显示在预览窗口里。
//  注意: 在抓屏线程上同步执行, 插件处理太重会拖累预览帧率。
// ============================================================
static void onFrame(const Frame& f) {
    if (!g_on_frame) return;
    ScreenFrame sf;
    sf.data        = const_cast<uint8_t*>(f.data);   // 允许插件就地修改
    sf.width       = f.width;
    sf.height      = f.height;
    sf.stride      = f.stride;
    sf.timestampMs = f.timestampMs;
    g_on_frame(&sf);
}

// 把内存位图画到预览窗口: 等大则 1:1 直拷, 否则快速拉伸适配
static void blitToWindow(HWND hwnd, HDC hdc) {
    RECT rc; GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw == g_cap.w && ch == g_cap.h) {
        BitBlt(hdc, 0, 0, cw, ch, g_cap.memDC, 0, 0, SRCCOPY);
    } else {
        SetStretchBltMode(hdc, COLORONCOLOR);
        StretchBlt(hdc, 0, 0, cw, ch, g_cap.memDC, 0, 0, g_cap.w, g_cap.h, SRCCOPY);
    }
}

// 读取选区线框当前的"内部矩形"(屏幕坐标), 抓屏并刷新预览
static void captureAndShow() {
    if (!g_selector || !g_preview || !g_previewDC) return;
    RECT wr; GetWindowRect(g_selector, &wr);
    int x = wr.left + B;
    int y = wr.top  + B;
    int w = (wr.right  - B) - x;
    int h = (wr.bottom - B) - y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w != g_cap.w || h != g_cap.h) {       // 选区尺寸变了 -> 重建捕获位图
        if (!g_cap.init(w, h)) return;
        // 预览窗口客户区同步调整为与选区等大 (1:1 显示)
        RECT pr = { 0, 0, w, h };
        AdjustWindowRect(&pr, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(g_preview, nullptr, 0, 0,
                     pr.right - pr.left, pr.bottom - pr.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_cap.grab(x, y);
    GdiFlush();   // 确保 BitBlt 已写入 DIBSection 内存, 插件才能读到正确像素

    // 抓到原始帧 -> 交给插件 (零拷贝, 直接引用 DIBSection 像素)
    Frame f{ (const uint8_t*)g_cap.bits, g_cap.w, g_cap.h, g_cap.w * 4, nowMs() };
    onFrame(f);

    blitToWindow(g_preview, g_previewDC);   // 显示(可能被插件修改过的)像素
}

// 根据窗口当前尺寸重建"空心细线框"的窗口区域 (外框 - 中间洞 = 边框环)
static void updateRegion(HWND hwnd) {
    RECT c; GetClientRect(hwnd, &c);
    int W = c.right, H = c.bottom;
    HRGN outer = CreateRectRgn(0, 0, W, H);
    HRGN hole  = CreateRectRgn(B, B, W - B, H - B);
    CombineRgn(outer, outer, hole, RGN_DIFF);     // 只剩边框环
    SetWindowRgn(hwnd, outer, TRUE);              // 窗口接管 outer 句柄
    DeleteObject(hole);
}

// ---------------- 选区线框窗口过程 ----------------
LRESULT CALLBACK SelectorProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST: {                            // 只做缩放, 不做整体移动
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hwnd, &pt);
        RECT c; GetClientRect(hwnd, &c);
        int W = c.right, H = c.bottom;

        bool L  = pt.x < GRAB;
        bool R  = pt.x > W - GRAB;
        bool T  = pt.y < GRAB;
        bool Bo = pt.y > H - GRAB;
        if (T && L) return HTCAPTION;    // 左上角 -> 整体平移(其余角仍为缩放)
        if (T && R) return HTTOPRIGHT;
        if (Bo && L) return HTBOTTOMLEFT;
        if (Bo && R) return HTBOTTOMRIGHT;
        if (pt.x < B)     return HTLEFT;
        if (pt.x > W - B) return HTRIGHT;
        if (pt.y < B)     return HTTOP;
        if (pt.y > H - B) return HTBOTTOM;
        return HTNOWHERE;
    }

    case WM_GETMINMAXINFO: {                        // 限制最小尺寸
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = MINW + 2 * B;
        mmi->ptMinTrackSize.y = MINH + 2 * B;
        return 0;
    }

    case WM_SIZE:
        updateRegion(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOVE:                                   // 移动后反色要按新位置重算
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT c; GetClientRect(hwnd, &c);

        // 纯红色边框。窗口区域已裁剪成边框环, 填满客户区即只显示那一圈。
        HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
        FillRect(hdc, &c, red);
        DeleteObject(red);

        EndPaint(hwnd, &ps);
        return 0;
    }

    // 系统进入"缩放"模态循环时主循环暂停, 用定时器维持预览与反色框实时刷新
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);

    // 注册两个窗口类
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

    // 选区线框窗口: 外框 = 内部区域 + 四周边框
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

    // 预览窗口 (放在左上角, 默认与初始选区等大, 之后随选区拉伸适配)
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
    g_previewDC = GetDC(g_preview);

    // ---- 环境就绪, 加载外部插件并初始化 ----
    if (!loadPlugin()) {
        MessageBoxW(g_preview,
                    L"未找到插件 plugin.dll (识别功能禁用)。\n程序仍会正常显示捕获画面。",
                    L"提示", MB_ICONINFORMATION);
    } else if (g_on_init) {
        if (g_on_init() != 0) {              // 插件初始化失败 -> 中止
            MessageBoxW(g_preview, L"插件 on_init 返回失败, 程序退出。", L"错误", MB_ICONERROR);
            unloadPlugin();
            return 1;
        }
    }

    // ---- 主循环: 抓屏 + 刷新预览 + FPS 统计 ----
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
                         g_cap.w, g_cap.h, fps);
                SetWindowTextW(g_preview, title);
                frameCount = 0;
                fpsTimer = now;
            }
        } else if (frameInterval - elapsed > 0.002) {
            Sleep(1);
        }
    }

    unloadPlugin();   // 调用插件 on_exit 并卸载 DLL
    if (g_previewDC) ReleaseDC(g_preview, g_previewDC);
    timeEndPeriod(1);
    g_cap.release();
    return 0;
}
