// plugin_example.cpp - 示例识别插件, 编译成 plugin.dll
//
// 演示三个导出函数。当前 on_frame 把画面转灰度, 这样运行后能直观看到
// "预览显示的是插件处理后的图像"。把里面换成你自己的识别/处理即可。

#include "plugin_api.h"
#include <stdint.h>
#include <stddef.h>

// 用 C 链接导出, 保证导出名就是 on_init / on_frame / on_exit (无名字修饰)
#define PLUGIN_EXPORT extern "C" __declspec(dllexport)

// 主循环开始前调用一次。返回 0 成功, 非 0 则主程序中止。
PLUGIN_EXPORT int on_init(void) {
    // 在这里做一次性初始化(分配资源、加载模型等)
    return 0;
}

// 每帧调用。可读, 也可就地修改 frame->data (BGRA), 修改会显示在预览里。
PLUGIN_EXPORT void on_frame(ScreenFrame* f) {
    for (int y = 0; y < f->height; ++y) {
        uint8_t* row = f->data + (size_t)y * f->stride;
        for (int x = 0; x < f->width; ++x) {
            uint8_t* p = row + x * 4;          // p[0]=B p[1]=G p[2]=R p[3]=A
            uint8_t g = (uint8_t)((p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8); // 灰度
            p[0] = p[1] = p[2] = g;
        }
    }
}

// 程序结束时调用一次, 释放 on_init 里分配的资源。
PLUGIN_EXPORT void on_exit(void) {
}
