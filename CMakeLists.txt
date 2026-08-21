cmake_minimum_required(VERSION 3.25)

project(TestService
    VERSION 1.0.0
    DESCRIPTION "Test Service"
    LANGUAGES CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- C++ standard ----
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Executable ----
add_executable(TestService
    src/TestService.cpp
)

target_compile_definitions(TestService PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(TestService PRIVATE
    Advapi32
    Dbghelp
    Shell32
    Wtsapi32
)

# ---- MSVC runtime ----
if(MSVC)
    set_property(TARGET TestService PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# ---- Compiler options ----
if(MSVC)
    target_compile_options(TestService PRIVATE
        /W4
        /permissive-
        $<$<CONFIG:Release>:/O2>
    )
else()
    target_compile_options(TestService PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        $<$<CONFIG:Release>:-O3>
    )
endif()

# ---- Output directory ----
set_target_properties(TestService PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---- Install ----
install(TARGETS TestService
    RUNTIME DESTINATION bin
)

# ---- Configuration summary ----
message(STATUS "")
message(STATUS "TestService Configuration:")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  C++ standard:   C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:       ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "")