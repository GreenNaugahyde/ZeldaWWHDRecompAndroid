# PC build of the app's randomizer (runtime/third_party/twwhd_rando with WWHD_APP) for testing releases:
#   cmake -S tools/rando/host -B <dir> -G Ninja && ninja -C <dir>
#   <dir>/rando_host DATA WORK GAME OUT   (DATA as the app's assets/rando: asm/, asm_eur/, asm_jpn/, assets/, logic/, customizer/)
cmake_minimum_required(VERSION 3.16)
project(rando_host CXX C)
set(CMAKE_CXX_STANDARD 20)
set(RANDO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../runtime/third_party/twwhd_rando)
file(GLOB RANDO_SOURCES
  ${RANDO_DIR}/*.cpp ${RANDO_DIR}/command/*.cpp ${RANDO_DIR}/customizer/*.cpp ${RANDO_DIR}/filetypes/*.cpp
  ${RANDO_DIR}/filetypes/util/*.cpp ${RANDO_DIR}/filetypes/shared/*.cpp ${RANDO_DIR}/filetypes/texture/*.cpp
  ${RANDO_DIR}/filetypes/subfiles/*.cpp ${RANDO_DIR}/logic/*.cpp ${RANDO_DIR}/logic/flatten/*.cpp ${RANDO_DIR}/seedgen/*.cpp
  ${RANDO_DIR}/utility/*.cpp ${RANDO_DIR}/libs/yaml.cpp ${RANDO_DIR}/libs/hash-library/sha1.cpp ${RANDO_DIR}/libs/hash-library/sha256.cpp
  ${RANDO_DIR}/libs/tinyxml2/tinyxml2.cpp ${RANDO_DIR}/libs/base64pp/base64pp/base64pp.cpp ${RANDO_DIR}/libs/yaml-cpp/src/*.cpp
  ${RANDO_DIR}/libs/yaml-cpp/src/contrib/*.cpp)
set(GIT_TAG 1.2.0)
set(SEED_KEY "")
configure_file(${RANDO_DIR}/version.hpp.in ${CMAKE_BINARY_DIR}/rando/version.hpp @ONLY)
configure_file(${RANDO_DIR}/keys/keys.hpp.in ${CMAKE_BINARY_DIR}/rando/keys/keys.hpp @ONLY)
add_executable(rando_host ${RANDO_SOURCES} host_main.cpp)
target_include_directories(rando_host PRIVATE ${RANDO_DIR} ${CMAKE_BINARY_DIR}/rando ${RANDO_DIR}/libs/yaml-cpp/include
                                             ${RANDO_DIR}/libs/base64pp/base64pp/include)
string(LENGTH "${RANDO_DIR}/" RANDO_PATH_SIZE)
target_compile_definitions(rando_host PRIVATE WWHD_APP SOURCE_PATH_SIZE=${RANDO_PATH_SIZE} YAML_CPP_STATIC_DEFINE)
target_compile_options(rando_host PRIVATE -w)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(rando_host PRIVATE ZLIB::ZLIB Threads::Threads)
