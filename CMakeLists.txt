# Standalone build (out-of-tree). In-tree builds use FreeSWITCH autotools.
# mkdir build && cd build && cmake .. && make
#
# Optional WebSocket support requires libks or libks2 (pkg-config).

cmake_minimum_required(VERSION 3.18)
project(mod_janus)
set(CMAKE_C_STANDARD 11)
set(CMAKE_SHARED_LIBRARY_PREFIX "")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FreeSWITCH REQUIRED IMPORTED_TARGET freeswitch)
pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)
message(STATUS "FreeSWITCH modules dir: ${FS_MOD_DIR}")

set(JANUS_SOURCES
	globals.c
	cJSON.c
	http.c
	api.c
	servers.c
	hash.c
	mod_janus.c
)

add_library(mod_janus SHARED ${JANUS_SOURCES})
set_property(TARGET mod_janus PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(mod_janus PRIVATE PkgConfig::FreeSWITCH pthread)

pkg_check_modules(KS2 libks2 IMPORTED_TARGET)
if(KS2_FOUND)
	message(STATUS "libks2 found: WebSocket support enabled")
	target_sources(mod_janus PRIVATE janus_ws.c)
	target_link_libraries(mod_janus PRIVATE PkgConfig::KS2)
	target_compile_definitions(mod_janus PRIVATE HAVE_MOD_JANUS_WS=1)
	target_compile_options(mod_janus PRIVATE ${KS2_CFLAGS})
else()
	pkg_check_modules(KS1 libks IMPORTED_TARGET)
	if(KS1_FOUND)
		message(STATUS "libks found: WebSocket support enabled")
		target_sources(mod_janus PRIVATE janus_ws.c)
		target_link_libraries(mod_janus PRIVATE PkgConfig::KS1)
		target_compile_definitions(mod_janus PRIVATE HAVE_MOD_JANUS_WS=1)
		target_compile_options(mod_janus PRIVATE ${KS1_CFLAGS})
	else()
		message(STATUS "libks not found: build is HTTP-only; ws:// URLs in config will be rejected at load time")
	endif()
endif()

install(TARGETS mod_janus DESTINATION ${FS_MOD_DIR})
