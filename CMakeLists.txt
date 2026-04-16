# to build module you will need
# apt-get update && apt-get install -y build-essential cmake pkg-config libfreeswitch-dev libspeexdsp-dev 
# mkdir -p build 
# cd build
# cmake -DCMAKE_BUILD_TYPE=Release ..
# make

cmake_minimum_required(VERSION 3.18)
project(mod_janus)
set(CMAKE_C_STANDARD 11)
set(CMAKE_SHARED_LIBRARY_PREFIX "")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FreeSWITCH REQUIRED IMPORTED_TARGET freeswitch)
pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)
message("FreeSWITCH modules dir: ${FS_MOD_DIR}")

add_library(mod_janus SHARED globals.c cJSON.c http.c api.c servers.c hash.c mod_janus.c)


set_property(TARGET mod_janus PROPERTY POSITION_INDEPENDENT_CODE ON)


target_link_libraries(mod_janus PRIVATE PkgConfig::FreeSWITCH pthread)


install(TARGETS mod_janus DESTINATION ${FS_MOD_DIR})
