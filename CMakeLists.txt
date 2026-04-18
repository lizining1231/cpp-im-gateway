cmake_minimum_required(VERSION 3.10)
project(cpp-im-gateway LANGUAGES C CXX)

# 改为C++11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 创建可执行文件 - 名字叫 TCPserver
add_executable(TCPserver  
    src/main.cpp
    src/server/TCPserver.cpp
)

# 包含目录
target_include_directories(TCPserver PRIVATE  
    src
    src/server
)

# Linux系统库
target_link_libraries(TCPserver pthread)  

message(STATUS "Using C++${CMAKE_CXX_STANDARD} standard")