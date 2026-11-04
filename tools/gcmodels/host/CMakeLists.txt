# PC build of the app's converter (runtime/src/gcmodel) for comparing with the Python tools:
#   cmake -S tools/gcmodels/host -B <dir> -G Ninja && ninja -C <dir>
#   <dir>/gcmodel_host Link.arc permanent_3d.pack out_permanent_3d.pack
cmake_minimum_required(VERSION 3.16)
project(gcmodel_host CXX)
set(CMAKE_CXX_STANDARD 20)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(CEMU ${ROOT}/runtime/third_party/cemu)
file(GLOB SRC ${ROOT}/runtime/src/gcmodel/*.cpp)
add_executable(gcmodel_host ${SRC} ${CEMU}/Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.cpp
               ${CEMU}/Cafe/HW/Latte/LatteAddrLib/LatteAddrLib_Coord.cpp host_main.cpp)
target_include_directories(gcmodel_host PRIVATE ${ROOT}/runtime/src ${CEMU} ${CEMU}/Cafe ${ROOT}/runtime/third_party/fmt/include)
target_compile_options(gcmodel_host PRIVATE "SHELL:-include ${CEMU}/cemu_shim.h" -O2 -Wall -Wno-unused-function)
