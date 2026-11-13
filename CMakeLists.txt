cmake_minimum_required(VERSION 3.20)
project(WVC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the camera DLL is loaded into Zoom/Telegram/browsers, so it must not depend on an installed VC++ runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_options(/utf-8 /W3 /MP)
add_compile_definitions(WIN32 _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)

set(SC ${CMAKE_SOURCE_DIR}/third_party/softcam/src)

# --- DirectShow base classes (Microsoft, MIT) ---
add_library(baseclasses STATIC
    ${SC}/baseclasses/amfilter.cpp
    ${SC}/baseclasses/amvideo.cpp
    ${SC}/baseclasses/combase.cpp
    ${SC}/baseclasses/dllentry.cpp
    ${SC}/baseclasses/dllsetup.cpp
    ${SC}/baseclasses/mtype.cpp
    ${SC}/baseclasses/source.cpp
    ${SC}/baseclasses/winutil.cpp
    ${SC}/baseclasses/wxdebug.cpp
    ${SC}/baseclasses/wxlist.cpp
    ${SC}/baseclasses/wxutil.cpp
)
target_include_directories(baseclasses PUBLIC ${SC}/baseclasses)
target_compile_definitions(baseclasses PRIVATE NDEBUG _LIB)

# --- softcam core (tshino/softcam, MIT) ---
add_library(softcamcore STATIC
    ${SC}/softcamcore/DShowSoftcam.cpp
    ${SC}/softcamcore/FrameBuffer.cpp
    ${SC}/softcamcore/Misc.cpp
    ${SC}/softcamcore/SenderAPI.cpp
    ${SC}/softcamcore/Watchdog.cpp
)
target_include_directories(softcamcore PUBLIC ${SC} ${SC}/softcamcore)
target_link_libraries(softcamcore PUBLIC baseclasses)
target_compile_definitions(softcamcore PRIVATE NDEBUG)

# --- camera DLL: WVCCam.dll (x64) / WVCCam32.dll (x86) ---
add_library(wvccam SHARED ${SC}/softcam/softcam.cpp ${SC}/softcam/softcam.def ${SC}/softcam/softcam.rc)
target_include_directories(wvccam PRIVATE ${SC}/softcam)
target_compile_definitions(wvccam PRIVATE NDEBUG)
target_link_libraries(wvccam PRIVATE softcamcore baseclasses strmiids winmm ole32 oleaut32 uuid user32 gdi32 advapi32)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(wvccam PROPERTIES OUTPUT_NAME WVCCam)
else()
    set_target_properties(wvccam PROPERTIES OUTPUT_NAME WVCCam32)
endif()

# --- the app (64-bit only) ---
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(WVC WIN32 app/ui_main.cpp app/ui_props.cpp app/ui_settings.cpp app/model.cpp app/render.cpp
        app/inputs.cpp app/h264.cpp app/util.cpp app/transition.cpp app/log.cpp app/update.cpp app/undo.cpp app/docks.cpp
        app/app.rc app/app.manifest)
    target_compile_definitions(WVC PRIVATE NOMINMAX)
    target_link_libraries(WVC PRIVATE user32 gdi32 shell32 advapi32 comctl32 ole32 dwmapi comdlg32 windowscodecs uxtheme mfplat mf mfreadwrite mfuuid ws2_32 iphlpapi msimg32 winhttp strmiids)
    add_dependencies(WVC wvccam)
endif()
