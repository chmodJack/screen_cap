# screen_cap — 屏幕区域捕获预览

捕获屏幕指定矩形区域，实时显示在一个置顶小窗口中。

## 当前版本：纯 CPU（GDI BitBlt）
- `main.cpp` — 全部实现
- `CMakeLists.txt` — 构建脚本

## 在 Visual Studio 2022 里运行

**方式一：打开 CMake 文件夹（推荐）**
1. VS2022 → 文件 → 打开 → CMake → 选择本文件夹的 `CMakeLists.txt`
2. 顶部选择 `screen_cap.exe` 作为启动项，点运行（F5）

**方式二：新建项目手动加入**
1. 新建「空项目」(Empty Project)
2. 把 `main.cpp` 加入项目
3. 项目属性 → 链接器 → 系统 → 子系统 改为 **窗口 (/SUBSYSTEM:WINDOWS)**
4. 字符集设为 **使用 Unicode 字符集**
5. F5 运行

## 调整捕获区域
编辑 `main.cpp` 顶部的参数：
```cpp
static int g_capX = 100;   // 区域左上角 X
static int g_capY = 100;   // 区域左上角 Y
static int g_capW = 640;   // 宽
static int g_capH = 360;   // 高
static int g_targetFps = 60;
```
预览窗口标题栏会显示实时 FPS。

## 性能说明
- GDI `BitBlt` 在普通桌面、几百×几百像素区域、60 FPS 下 CPU 占用很低。
- 如果区域很大（如全屏 4K）或帧率不够、占用过高，再升级到 GPU 方案：
  **Desktop Duplication API (`IDXGIOutputDuplication`) + Direct3D 11** —
  由 GPU 直接拿到桌面帧，零内存拷贝显示，开销远低于 GDI。
  需要时告诉我，我把 `main.cpp` 改成 D3D 版本。
