cmake_minimum_required(VERSION 3.0)
project(demo)
set(CMAKE_CXX_STANDARD 17)

add_executable(demo main.cpp)

target_link_libraries(demo pthread
  /usr/lib/x86_64-linux-gnu/libavcodec.so
  /usr/lib/x86_64-linux-gnu/libavformat.so
  /usr/lib/x86_64-linux-gnu/libavutil.so
)
