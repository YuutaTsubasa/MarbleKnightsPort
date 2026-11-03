cmake_minimum_required(VERSION 3.22)
project(marbleknights_loader C CXX ASM)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wno-unused-function -Wno-incompatible-function-pointer-types
                    -Wno-int-conversion -fno-omit-frame-pointer)

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
