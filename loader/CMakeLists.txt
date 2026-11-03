cmake_minimum_required(VERSION 3.22)
project(marbleknights_loader C CXX ASM)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wno-unused-function -Wno-incompatible-function-pointer-types
                    -Wno-int-conversion -fno-omit-frame-pointer -fvisibility=hidden)

add_library(mkdarwin STATIC
    src/macho_loader.c
    src/macho_unwind.c
    src/darwin_libsystem.c
    src/darwin_cxx.c
    src/darwin_cf.c
    src/trap.S
    src/cxx_runtime.cpp)
target_include_directories(mkdarwin PUBLIC src)
target_link_libraries(mkdarwin PUBLIC log)

add_executable(il2cpp_probe probe/il2cpp_probe.c)
target_link_libraries(il2cpp_probe mkdarwin)
set_target_properties(il2cpp_probe PROPERTIES LINKER_LANGUAGE CXX)

# Proxy libil2cpp.so for the official Unity Android player.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(API_STUBS ${CMAKE_CURRENT_BINARY_DIR}/il2cpp_api_stubs.S)
add_custom_command(OUTPUT ${API_STUBS}
    COMMAND Python3::Interpreter -I ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_api_stubs.py
            ${CMAKE_CURRENT_SOURCE_DIR}/proxy/il2cpp_api.txt ${API_STUBS}
    DEPENDS proxy/il2cpp_api.txt ../tools/gen_api_stubs.py)
add_library(il2cpp SHARED proxy/proxy.c ${API_STUBS})
target_link_libraries(il2cpp mkdarwin)
target_link_options(il2cpp PRIVATE -Wl,--exclude-libs,ALL -Wl,-Bsymbolic)
set_target_properties(il2cpp PROPERTIES LINKER_LANGUAGE CXX)
