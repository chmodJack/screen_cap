# screen_cap — 屏幕区域捕获预览

捕获屏幕指定矩形区域，实时显示在一个置顶小窗口中。

## 当前版本：D3D11 Desktop Duplication + GDI 预览
- `main.cpp` — 全部实现（~630 行）
- `CMakeLists.txt` — 构建脚本

## 要求
- Windows 8.0 或更高
- 支持 DirectX 11.0 的 GPU
- Visual Studio 2022（或其他 MSVC 工具链）

## 在 Visual Studio 2022 里运行

**方式一：打开 CMake 文件夹（推荐）**
1. VS2022 → 文件 → 打开 → CMake → 选择本文件夹的 `CMakeLists.txt`
2. 顶部选择 `screen_cap.exe` 作为启动项，点运行（F5）

**方式二：新建项目手动加入**
1. 新建「空项目」(Empty Project)
2. 把 `main.cpp` 加入项目
3. 项目属性 → 链接器 → 系统 → 子系统 改为 **窗口 (/SUBSYSTEM:WINDOWS)**
4. 字符集设为 **使用 Unicode 字符集**
5. 链接器 → 输入 → 附加依赖项 添加 `d3d11.lib;dxgi.lib`
6. F5 运行

## 调整捕获区域

编辑 `main.cpp` 顶部的参数：
```cpp
static int g_targetFps = 120;        // 预览目标帧率上限
static int g_initRx = 300, g_initRy = 200;   // 选区左上角
static int g_initRw = 640, g_initRh = 360;   // 选区宽高
```

预览窗口标题栏会显示实时 FPS。

## 光标

Desktop Duplication API 原生不包含光标。本程序用 GDI `GetCursorInfo` + `DrawIconEx`
在帧数据上手工合成光标，因此预览中能看到鼠标箭头。

## 选区跨显示器

选区拖动时自动跟随当前所在显示器，无需手动配置。

## 外部插件

支持动态加载 `plugin.dll`，在每帧上运行识别/处理。接口见 `plugin_api.h`。
