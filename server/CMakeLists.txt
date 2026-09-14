cmake_minimum_required(VERSION 3.16)
project(threejs_viz_backend CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(viz_backend main.cpp)

find_package(Threads REQUIRED)
target_link_libraries(viz_backend PRIVATE Threads::Threads)

# 零外部依赖:仅使用 POSIX socket + pthread + 标准库。
target_compile_options(viz_backend PRIVATE -O2 -Wall)