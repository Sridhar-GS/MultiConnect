cmake_minimum_required(VERSION 3.24)
project(MultiConnect LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC-specific flags
if(MSVC)
    add_compile_options(
        /W4 /WX
        /permissive-
        /Zc:__cplusplus
        /EHsc
        /arch:AVX2
        /fp:fast
    )
    add_compile_definitions(
        _WIN32_WINNT=0x0A00
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

add_executable(${PROJECT_NAME} WIN32
    src/main.cpp
    src/Gui.cpp
    src/VolumeController.cpp
    src/EngineOrchestrator.cpp
    src/CaptureEngine.cpp
    src/RenderEngine.cpp
    src/SyncController.cpp
    src/DeviceManager.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE include)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ole32
    oleaut32
    uuid
    winmm
    avrt
    ksuser
    d2d1
    dwrite
    gdiplus
    Propsys
)

# Copy asset/ folder next to the executable for runtime icon loading
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/asset"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/asset"
)

# -----------------------------------------------------------------------------
# CPack Configuration (Installer)
# -----------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(DIRECTORY "asset" DESTINATION bin)
install(FILES "README.md" DESTINATION .)

set(CPACK_PACKAGE_VENDOR "Sridhar-GS")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "MultiConnect - Bluetooth Audio Engine")
set(CPACK_PACKAGE_VERSION_MAJOR 1)
set(CPACK_PACKAGE_VERSION_MINOR 0)
set(CPACK_PACKAGE_VERSION_PATCH 0)

if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_NSIS_DISPLAY_NAME "${PROJECT_NAME}")
    set(CPACK_NSIS_PACKAGE_NAME "${PROJECT_NAME}")
    set(CPACK_NSIS_MODIFY_PATH ON)
    
    # Icon for the installer (optional, using default if not available)
    # set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/asset/logo.ico")
endif()

include(CPack)
