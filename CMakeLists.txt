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

# GDI 等 API 在 Gdi32/User32 中, 通常 MSVC 会自动链接, 这里显式写出更稳妥
target_link_libraries(screen_cap PRIVATE Gdi32 User32)
