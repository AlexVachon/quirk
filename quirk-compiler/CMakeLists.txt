# 1. Configure the project and download/compile dependencies
# cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=./vcpkg/scripts/buildsystems/vcpkg.cmake

# 2. Compile Quirk! (This replaces your 'make' command)
# cmake --build build


cmake_minimum_required(VERSION 3.15)
project(QuirkCompiler CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Find LLVM 14 (Using your OS installation to save compile time)
set(CMAKE_PREFIX_PATH "/usr/lib/llvm-14/cmake;${CMAKE_PREFIX_PATH}")
find_package(LLVM 14 REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(llvm_libs core orcjit native)

# 2. Find our vcpkg dependencies
find_package(CURL REQUIRED)
# --- FIX: Match the exact capitalization exported by the library ---
find_package(BDWgc CONFIG REQUIRED)

# 3. Include local headers
include_directories(include)

# 4. Define C++ source files
set(QUIRK_SOURCES
    src/Compiler.cpp
    src/Core/Lexer.cpp
    src/Core/Parser.cpp
    src/Core/Sema.cpp
    # (Codegen.cpp is excluded here because it is #included directly into Compiler.cpp)
)

# 5. Build the Compiler Executable (bin/quirk)
add_executable(quirk ${QUIRK_SOURCES})
# --- FIX: Link against BDWgc::gc ---
target_link_libraries(quirk PRIVATE ${llvm_libs} CURL::libcurl BDWgc::gc)

# 6. Build the C Runtime Library (bin/runtime.so)
add_library(runtime SHARED src/Runtime/runtime.c)
# --- FIX: Link against BDWgc::gc ---
target_link_libraries(runtime PRIVATE BDWgc::gc CURL::libcurl)
set_target_properties(runtime PROPERTIES PREFIX "") 

# 7. Output everything directly to the bin/ directory
set_target_properties(quirk runtime PROPERTIES 
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin" 
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)