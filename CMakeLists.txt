cmake_minimum_required(VERSION 3.10)
project(cpp-im-gateway LANGUAGES C CXX)

# 改为C++11（您的代码使用了{}初始化，这是C++11特性）
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 创建可执行文件
add_executable(${PROJECT_NAME}
    src/main.cpp
    src/server/echo_server.cpp
)

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    src
    src/server
)

# Linux系统库
target_link_libraries(${PROJECT_NAME} pthread)

message(STATUS "Using C++${CMAKE_CXX_STANDARD} standard")