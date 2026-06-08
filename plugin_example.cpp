// plugin_example.cpp - 示例识别插件, 编译成 plugin.dll
//
// 落实多线程异步处理架构。在 on_frame 中，主线程只进行极速的像素数据深度拷贝和线程唤醒，
// 真正的识别/处理逻辑在后台独立的 worker 线程中运行，绝不阻塞主程序捕获与渲染（不卡顿）。

#include "plugin_api.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <cstring>

// 用 C 链接导出, 保证导出名就是 on_init / on_frame / on_exit (无名字修饰)
#define PLUGIN_EXPORT extern "C" __declspec(dllexport)

// --- 共享数据区与线程同步组件 ---
struct SharedFrame {
    std::vector<uint8_t> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int32_t stride = 0;
    int64_t timestampMs = 0;
    bool has_new_frame = false;
};

static std::thread              g_workerThread;
static std::atomic<bool>        g_running{ false };
static SharedFrame              g_sharedBuffer;
static std::mutex               g_mutex;
static std::condition_variable  g_cv;

// --- 异步后台识别/处理函数 ---
// 在独立的子线程中执行，可以进行耗时的 OCR、特征检测、AI 推理等。
static void processFrameAsync(const std::vector<uint8_t>& pixels, int32_t w, int32_t h, int32_t stride, int64_t timestamp) {
    if (pixels.empty() || w <= 0 || h <= 0) return;

    // -------------------------------------------------------------
    // 【此处放置你实际的识别逻辑】
    // 例如：调用深度学习模型推理、进行复杂的目标匹配等。
    //
    // 作为演示，这里在后台对本地拷贝的像素数据运行一次灰度计算以消耗 CPU：
    // -------------------------------------------------------------
    uint64_t totalLuminance = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = pixels.data() + (size_t)y * stride;
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + x * 4; // BGRA
            // 经典的 YUV 亮度公式
            uint8_t gray = (uint8_t)((p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8);
            totalLuminance += gray;
        }
    }
    
    // 模拟轻微的处理延迟（2毫秒），代表识别耗时
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

// --- 后台工作线程主循环 ---
static void workerLoop() {
    SharedFrame localFrame;
    while (g_running) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            // 等待新帧到来，或者收到停止运行的通知
            g_cv.wait(lock, [] { return g_sharedBuffer.has_new_frame || !g_running; });

            if (!g_running) {
                break;
            }

            // 移动语义：将共享缓冲区的数据移动到本地线程缓冲区，避免再次分配和拷贝内存
            localFrame = std::move(g_sharedBuffer);
            g_sharedBuffer.has_new_frame = false;
        }

        // 在锁区外部进行识别处理，绝不阻塞主线程生产新帧
        processFrameAsync(localFrame.pixels, localFrame.width, localFrame.height, localFrame.stride, localFrame.timestampMs);
    }
}

// --- 导出函数：主循环开始前调用一次 ---
PLUGIN_EXPORT int on_init(void) {
    g_running = true;
    // 启动异步识别后台线程
    g_workerThread = std::thread(workerLoop);
    return 0;
}

// --- 导出函数：每帧调用。主线程调用，要求极速返回 ---
PLUGIN_EXPORT void on_frame(ScreenFrame* f) {
    if (!f || !f->data || f->width <= 0 || f->height <= 0) return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        // 1. 动态自适应调整插件侧的缓存大小，避免每帧都重新分配内存
        size_t requiredSize = (size_t)f->height * f->stride;
        if (g_sharedBuffer.pixels.capacity() < requiredSize) {
            g_sharedBuffer.pixels.reserve(requiredSize);
        }
        g_sharedBuffer.pixels.resize(requiredSize);

        // 2. 深度拷贝（Deep Copy）原始帧的像素数据
        // 主程序在 on_frame 返回后会立即 Unmap / 释放这一帧，因此必须在返回前拷贝走
        std::memcpy(g_sharedBuffer.pixels.data(), f->data, requiredSize);

        // 3. 复制帧元数据
        g_sharedBuffer.width = f->width;
        g_sharedBuffer.height = f->height;
        g_sharedBuffer.stride = f->stride;
        g_sharedBuffer.timestampMs = f->timestampMs;
        g_sharedBuffer.has_new_frame = true;
    }

    // 4. 唤醒工作线程进行异步计算
    g_cv.notify_one();
}

// --- 导出函数：程序结束时调用一次 ---
PLUGIN_EXPORT void on_exit(void) {
    // 1. 标志位设为 false 并唤醒线程，使其优雅退出
    g_running = false;
    g_cv.notify_one();

    // 2. 等待子线程安全结束
    if (g_workerThread.joinable()) {
        g_workerThread.join();
    }

    // 3. 彻底释放缓存内存
    std::vector<uint8_t>().swap(g_sharedBuffer.pixels);
}
