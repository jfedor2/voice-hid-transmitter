cmake_minimum_required(VERSION 3.20)
project(voice-hid-transmitter CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# static CRT so the exe runs without the VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9b
  GIT_SHALLOW TRUE
)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui json)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)

# Everything except the UI, so the tests can link it too.
add_library(vht_core STATIC
  src/gamepad.cpp
  src/notation.cpp
  src/engine.cpp
  src/features.cpp
  src/recognizer.cpp
  src/audio.cpp
  src/config.cpp
  src/embedding.cpp
  src/sound_vectors.cpp
)
target_compile_definitions(vht_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(vht_core PUBLIC /W3 /utf-8)
target_link_libraries(vht_core PUBLIC
  nlohmann_json::nlohmann_json
  ws2_32 ole32 winmm
)

# The model files are built into the executables (see src/resources.rc).
set_source_files_properties(src/resources.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/models/speech_embedding.bin;${CMAKE_CURRENT_SOURCE_DIR}/models/sound_background.bin")

add_executable(voice-hid-transmitter WIN32 src/main.cpp src/resources.rc)
target_link_libraries(voice-hid-transmitter PRIVATE vht_core imgui_lib d3d11 dxgi d3dcompiler shell32)

# Command line tool for exercising the recognizer, notation parser and engine.
add_executable(vht-test src/test_main.cpp src/resources.rc)
target_link_libraries(vht-test PRIVATE vht_core)
target_compile_definitions(vht-test PRIVATE _CRT_SECURE_NO_WARNINGS)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT voice-hid-transmitter)
