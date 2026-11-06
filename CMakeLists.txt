cmake_minimum_required(VERSION 3.16)

project(lockdetect
    VERSION 1.0.0
    DESCRIPTION "Find which process is holding a file or directory on Windows"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(lockdetect src/lockdetect.c)

target_compile_definitions(lockdetect PRIVATE
    _WIN32_WINNT=0x0600
    UNICODE
    _UNICODE)

# rstrtmgr.dll -> Restart Manager (RmStartSession / RmRegisterResources / RmGetList)
# shell32.dll  -> CommandLineToArgvW
target_link_libraries(lockdetect PRIVATE rstrtmgr shell32)

if(MSVC)
    target_compile_options(lockdetect PRIVATE /W4)
else()
    target_compile_options(lockdetect PRIVATE -Wall -Wextra)
endif()
