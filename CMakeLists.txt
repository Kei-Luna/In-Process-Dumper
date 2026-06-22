cmake_minimum_required(VERSION 3.20)

project(InProcessDumper LANGUAGES CXX)

add_library(InProcessDumper SHARED
    src/dump_workflow.cpp
    src/dumper_common.cpp
    src/dumper.cpp
    src/pe_reconstruction.cpp
)

target_compile_features(InProcessDumper PRIVATE cxx_std_17)
target_compile_definitions(InProcessDumper PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)
target_link_libraries(InProcessDumper PRIVATE Dbghelp)

add_custom_command(TARGET InProcessDumper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/InProcessDumper.json"
        "$<TARGET_FILE_DIR:InProcessDumper>/InProcessDumper.json"
)

if(MSVC)
    target_compile_options(InProcessDumper PRIVATE /W4 /permissive-)
endif()
