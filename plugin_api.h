// plugin_api.h - 屏幕捕获程序的外部插件接口 (C ABI)
//
// 主程序(screen_cap.exe)和插件 DLL 都包含本头文件, 以保证接口一致。
// 插件需用 C 链接导出以下三个函数(名字必须完全一致):
//
//   int  on_init(void);                 // 主循环开始前调用一次。返回 0 成功, 非 0 失败(主程序会中止)。
//   void on_frame(ScreenFrame* frame);  // 每帧调用。可读, 也可就地修改像素(改动会显示在预览里)。
//   void on_exit(void);                 // 程序结束时调用一次, 用于清理。
//
// 三个函数都是可选的: 缺哪个主程序就跳过哪个。

#ifndef SCREENCAP_PLUGIN_API_H
#define SCREENCAP_PLUGIN_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 一帧图像。像素格式: BGRA, 8 位/通道, top-down(第一行在最前), stride = 每行字节数。
//   访问像素 (x, y):  uint8_t* p = data + (size_t)y * stride + x * 4;
//                     p[0]=B  p[1]=G  p[2]=R  p[3]=A
typedef struct ScreenFrame {
    uint8_t* data;          // 像素首地址, 可读可写
    int32_t  width;
    int32_t  height;
    int32_t  stride;        // 每行字节数 (= width * 4)
    int64_t  timestampMs;   // 该帧的毫秒时间戳
} ScreenFrame;

// 函数指针类型 (主程序用 GetProcAddress 取地址后按这些类型调用)
typedef int  (*screencap_on_init_fn)(void);
typedef void (*screencap_on_frame_fn)(ScreenFrame* frame);
typedef void (*screencap_on_exit_fn)(void);

#ifdef __cplusplus
}
#endif

#endif // SCREENCAP_PLUGIN_API_H
