cmake_minimum_required(VERSION 3.18)
project(kuni VERSION 1.0.0)

set(OPENAICHAT_ADDRESS "localhost:11434" CACHE STRING "Address to privateGPT")
option(BUILD_SHARED_LIBS OFF)

set(AUI_VERSION 6d0908466c44959edffba506e850948f9c50d087)

# Use AUI.Boot
file(
        DOWNLOAD
        https://raw.githubusercontent.com/aui-framework/aui/${AUI_VERSION}/aui.boot.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/aui.boot.cmake)
include(${CMAKE_CURRENT_BINARY_DIR}/aui.boot.cmake)

set(AUI_COROUTINES "Use C++20 coroutines" ON)
auib_mark_var_forwardable(AUI_COROUTINES)

# import AUI
auib_import(aui https://github.com/aui-framework/aui
        COMPONENTS core json curl crypt image views
        VERSION ${AUI_VERSION})


auib_import(date https://github.com/HowardHinnant/date)

set(AUIB_TD_VALIDATE OFF)
auib_import(Td https://github.com/tdlib/td
        VERSION e597838)

aui_executable(${PROJECT_NAME})
target_compile_definitions(${PROJECT_NAME} PRIVATE OPENAICHAT_ADDRESS=${OPENAICHAT_ADDRESS})
target_compile_definitions(Tests PRIVATE OPENAICHAT_ADDRESS=${OPENAICHAT_ADDRESS})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/secrets)

# Link required libs
aui_link(${PROJECT_NAME} PUBLIC aui::core aui::json aui::curl aui::crypt aui::image aui::views Td::TdStatic)

aui_enable_tests(${PROJECT_NAME})
