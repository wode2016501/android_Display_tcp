cmake_minimum_required(VERSION 3.10.2)
project("my_scrcpy_codec")

add_library(my_scrcpy_codec SHARED MyNativeBridge.cpp)

# 寻找 Android 专属系统库
find_library(media-ndk-lib mediandk)
find_library(android-lib android)
find_library(log-lib log)

# 强制链接
target_link_libraries(my_scrcpy_codec
                      ${media-ndk-lib}
                      ${android-lib}
                      ${log-lib})



