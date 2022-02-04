cmake_minimum_required(VERSION 3.8)
project(cacheline_movement_perf)

add_executable(${PROJECT_NAME} main.cpp tests.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
target_link_libraries(${PROJECT_NAME} PRIVATE -pthread)
