cmake_minimum_required(VERSION 3.20)
project(screen_cap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WIN32 = 生成无控制台的窗口程序 (入口为 wWinMain)
add_executable(screen_cap WIN32 main.cpp)

# 使用 Unicode (wWinMain / 宽字符 API)
target_compile_definitions(screen_cap PRIVATE UNICODE _UNICODE)

# 关键: 让 MSVC 按 UTF-8 解析源文件 (源码含中文注释/字符串)。
# 否则会按系统代码页(936)误读, 报 C4819 / C3872 等错误。
if (MSVC)
    target_compile_options(screen_cap PRIVATE /utf-8)
endif()

# D3D11 + DXGI (Desktop Duplication) + GDI (窗口/预览)
target_link_libraries(screen_cap PRIVATE d3d11 dxgi Gdi32 User32)

# ---- 示例识别插件 plugin.dll ----
# 与 exe 输出到同一目录, 这样 exe 启动时能在自身目录找到它。
add_library(plugin SHARED plugin_example.cpp)
target_compile_definitions(plugin PRIVATE UNICODE _UNICODE)
if (MSVC)
    target_compile_options(plugin PRIVATE /utf-8)
endif()
set_target_properties(plugin PROPERTIES
    OUTPUT_NAME "plugin"
    PREFIX ""                                  # 不要 lib 前缀, 输出就叫 plugin.dll
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:screen_cap>")

# ---- OpenCV 图像处理插件 plugin_opencv.dll ----
# 使用 OpenCV 对每帧进行 Canny 边缘检测等实时处理。
# 安装路径: C:\Users\hongyunz\share\opencv
set(OpenCV_DIR "C:/Users/hongyunz/share/opencv/build" CACHE PATH "OpenCV cmake config directory")
find_package(OpenCV QUIET PATHS "${OpenCV_DIR}" NO_DEFAULT_PATH)
if (OpenCV_FOUND)
    add_library(plugin_opencv SHARED plugin_opencv.cpp)
    target_compile_definitions(plugin_opencv PRIVATE UNICODE _UNICODE)
    if (MSVC)
        target_compile_options(plugin_opencv PRIVATE /utf-8)
    endif()
    target_include_directories(plugin_opencv PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(plugin_opencv PRIVATE ${OpenCV_LIBS})
    set_target_properties(plugin_opencv PROPERTIES
        OUTPUT_NAME "plugin_opencv"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:screen_cap>")
    message(STATUS "plugin_opencv: OpenCV found at ${OpenCV_DIR}, outputs plugin_opencv.dll")
else()
    message(STATUS "plugin_opencv: OpenCV NOT found at ${OpenCV_DIR}, skipping")
endif()
