cmake_minimum_required(VERSION 3.10)
project(cpp_python_networking)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Add include path for nlohmann/json
include_directories(${CMAKE_SOURCE_DIR}/nlohmann)

# Add executable target
add_executable(server server.cpp)
