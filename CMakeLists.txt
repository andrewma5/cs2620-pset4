cmake_minimum_required(VERSION 3.16)
project(cs2620-pset4 CXX)

option(SAN "Enable AddressSanitizer+UBSanitizer" OFF)
option(ASAN "Enable AddressSanitizer" OFF)
option(UBSAN "Enable UBSanitizer" OFF)
option(TSAN "Enable ThreadSanitizer" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_compile_options(-g -O3 -W -Wall)
add_link_options(-g -O3 -W -Wall)

if((SAN AND NOT TSAN) OR UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
endif()
if((SAN AND NOT TSAN) OR ASAN)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
endif()
if(TSAN)
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# xxhash (required by cotamer)
find_path(XXHASH_INCLUDE_DIR xxhash.h HINTS /opt/homebrew/include)
find_library(XXHASH_LIBRARY xxhash HINTS /opt/homebrew/lib)
if(NOT XXHASH_INCLUDE_DIR OR NOT XXHASH_LIBRARY)
    message(FATAL_ERROR "xxhash not found. Install with: brew install xxhash")
endif()

# llhttp (required for tm-server)
find_path(LLHTTP_INCLUDE_DIR llhttp.h HINTS /opt/homebrew/include)
find_library(LLHTTP_LIBRARY llhttp HINTS /opt/homebrew/lib)
if(NOT LLHTTP_INCLUDE_DIR OR NOT LLHTTP_LIBRARY)
    message(FATAL_ERROR "llhttp not found. Install with: brew install llhttp")
endif()

# nlohmann_json (required for JSON serialization)
find_package(nlohmann_json REQUIRED)
get_target_property(_njson_inc nlohmann_json::nlohmann_json
                    INTERFACE_INCLUDE_DIRECTORIES)
if(_njson_inc)
    include_directories(${_njson_inc})
endif()

# Generate cotamer config header (needed because cotamer/http.hh checks
# COTAMER_HAVE_NLOHMANN_JSON)
set(COTAMER_HAVE_LLHTTP 1)
set(COTAMER_HAVE_NLOHMANN_JSON 1)
set(COTAMER_HAVE_CURL 0)
set(COTAMER_HAVE_ZLIB 0)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../cotamer/config.hh.in
    ${CMAKE_CURRENT_BINARY_DIR}/cotamer/config.hh
    @ONLY
)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_library(Cotamer OBJECT
    ../cotamer/cotamer.cc
    ../cotamer/io.cc
)

add_library(CotamerHttp OBJECT
    ../cotamer/http.cc
)
target_include_directories(CotamerHttp PUBLIC ${LLHTTP_INCLUDE_DIR})

# tm SM library (created in later tasks; tm.cc and task_manager_db.cc
# don't exist yet, but listing them here lets us add them as we go)
add_library(Tm OBJECT
    tm.cc
    task_manager_db.cc
)

add_executable(tm-server
    tm-server.cc
    $<TARGET_OBJECTS:Cotamer>
    $<TARGET_OBJECTS:CotamerHttp>
    $<TARGET_OBJECTS:Tm>
)
target_include_directories(tm-server PRIVATE ${LLHTTP_INCLUDE_DIR})
target_link_libraries(tm-server PRIVATE
    ${LLHTTP_LIBRARY}
    nlohmann_json::nlohmann_json
)

add_executable(tm-tests
    tm-tests.cc
    $<TARGET_OBJECTS:Cotamer>
    $<TARGET_OBJECTS:Tm>
)
target_link_libraries(tm-tests PRIVATE
    nlohmann_json::nlohmann_json
)

# tm-replay: read a tm-server JSONL log, replay through task_manager_db,
# emit a snapshots.json for the visualizer. No HTTP, no networking.
add_executable(tm-replay
    tm-replay.cc
    $<TARGET_OBJECTS:Tm>
)
target_include_directories(tm-replay PRIVATE ${XXHASH_INCLUDE_DIR})
target_link_libraries(tm-replay PRIVATE
    nlohmann_json::nlohmann_json
    ${XXHASH_LIBRARY}
)
