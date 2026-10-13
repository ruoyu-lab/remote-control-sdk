cmake_minimum_required(VERSION 3.25)
project(RemoteControlExternalConsumer LANGUAGES CXX)
find_package(RemoteControlSDK REQUIRED)
add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_20)
target_link_libraries(consumer PRIVATE RemoteControl::All) # The core and every official module.
