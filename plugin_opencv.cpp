// plugin_opencv.cpp - OpenCV 极致线条画插件
//
// 策略: 只保留显著的强边界，忽略细碎纹理。
//
// 需要 OpenCV 库，安装路径：C:\Users\hongyunz\share\opencv

#include "plugin_api.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <cstdio>

#define PLUGIN_EXPORT extern "C" __declspec(dllexport)

static const int    kBlurSize    = 5;
static const double kCannyLow    = 50.0;
static const double kCannyHigh   = 150.0;

static cv::Ptr<cv::CLAHE> g_clahe;

PLUGIN_EXPORT int on_init(void) {
    g_clahe = cv::createCLAHE(1.0, cv::Size(8, 8));
    fprintf(stderr, "[OpenCV Plugin] blur=%d canny=[%.0f,%.0f]\n",
            kBlurSize, kCannyLow, kCannyHigh);
    return 0;
}

PLUGIN_EXPORT void on_frame(ScreenFrame* f) {
    if (!f || !f->data || f->width <= 0 || f->height <= 0) return;

    cv::Mat frame(f->height, f->width, CV_8UC4, f->data, f->stride);

    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(kBlurSize, kBlurSize), 0);

    cv::Mat enhanced;
    g_clahe->apply(blurred, enhanced);

    cv::Mat edges;
    cv::Canny(enhanced, edges, kCannyLow, kCannyHigh);

    // 二值边缘图转 BGRA 写回显示缓冲
    cv::Mat bgra;
    cv::cvtColor(edges, bgra, cv::COLOR_GRAY2BGRA);
    bgra.copyTo(frame);
}

PLUGIN_EXPORT void on_exit(void) {
    fprintf(stderr, "[OpenCV Plugin] Shutdown.\n");
}
