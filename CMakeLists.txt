cmake_minimum_required(VERSION 3.20)
project(vScrubber2 VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── Qt6 ──────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Widgets
    OpenGL
    OpenGLWidgets
)

# ── FFmpeg ────────────────────────────────────────────────────────────────────
# Структура D:/Libs/ffmpeg-dev/:
#   include/     ← libavcodec/, libavformat/, libavutil/
#   lib/         ← avcodec.lib, avformat.lib, avutil.lib
#   bin/         ← avcodec-*.dll, avformat-*.dll, ...

set(FFMPEG_DIR "D:/Libs/ffmpeg-dev" CACHE PATH "Путь к FFmpeg dev сборке")

find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    HINTS "${FFMPEG_DIR}/include"
    REQUIRED
)

# Ищем каждую библиотеку отдельно
foreach(_lib avcodec avformat avutil)
    string(TOUPPER ${_lib} _LIB_UPPER)
    find_library(FFMPEG_${_LIB_UPPER}_LIBRARY
        NAMES ${_lib}
        HINTS "${FFMPEG_DIR}/lib"
        REQUIRED
    )
    message(STATUS "FFmpeg ${_lib}: ${FFMPEG_${_LIB_UPPER}_LIBRARY}")
endforeach()

message(STATUS "FFmpeg include: ${FFMPEG_INCLUDE_DIR}")

# ── CUDA ──────────────────────────────────────────────────────────────────────
# Нужен для GL-CUDA interop (cudaGraphicsGLRegisterImage и т.д.)
# Путь: C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2

set(CUDA_TOOLKIT_DIR "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2"
    CACHE PATH "Путь к CUDA Toolkit")

find_path(CUDA_INCLUDE_DIR
    NAMES cuda.h
    HINTS "${CUDA_TOOLKIT_DIR}/include"
    REQUIRED
)

find_library(CUDA_LIBRARY
    NAMES cuda
    HINTS "${CUDA_TOOLKIT_DIR}/lib/x64"
    REQUIRED
)

find_library(CUDART_LIBRARY
    NAMES cudart
    HINTS "${CUDA_TOOLKIT_DIR}/lib/x64"
    REQUIRED
)

message(STATUS "CUDA include: ${CUDA_INCLUDE_DIR}")
message(STATUS "CUDA lib:     ${CUDA_LIBRARY}")
message(STATUS "CUDART lib:   ${CUDART_LIBRARY}")

# ── Исходники ────────────────────────────────────────────────────────────────
set(SOURCES
    main.cpp
    mainwindow.cpp
    videowidget.cpp
    ffmpegdecoder.cpp
    frameringbuffer.cpp
    packetbuffer.cpp
    playbackcontroller.cpp
    inputcontroller.cpp
    zoompanstate.cpp
    osd.cpp
)

set(HEADERS
    mainwindow.h
    videowidget.h
    ffmpegdecoder.h
    frameringbuffer.h
    packetbuffer.h
    playbackcontroller.h
    inputcontroller.h
    zoompanstate.h
    osd.h
)

# ── Исполняемый файл ─────────────────────────────────────────────────────────
qt_add_executable(vScrubber2
    ${SOURCES}
    ${HEADERS}
)

target_include_directories(vScrubber2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIR}
    ${CUDA_INCLUDE_DIR}
)

target_link_libraries(vScrubber2 PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${CUDA_LIBRARY}
    ${CUDART_LIBRARY}
    opengl32
)

# ── Копирование DLL рядом с exe ───────────────────────────────────────────────
# FFmpeg DLL
file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
add_custom_command(TARGET vScrubber2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${FFMPEG_DLLS}
        "$<TARGET_FILE_DIR:vScrubber2>"
    COMMENT "Копирование FFmpeg DLL"
)

# ── Qt деплой ─────────────────────────────────────────────────────────────────
if(WIN32)
    add_custom_command(TARGET vScrubber2 POST_BUILD
        COMMAND "${Qt6_DIR}/../../../bin/windeployqt.exe"
            --no-translations
            "$<TARGET_FILE:vScrubber2>"
        COMMENT "windeployqt"
    )
endif()