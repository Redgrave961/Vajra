cmake_minimum_required(VERSION 3.20)
project(AsuraRecomp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Build with clang-cl (see build.ps1)")
endif()

set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" CACHE PATH "ps3recomp toolkit")
set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH "Lifted PPU output (lift.ps1)")
set(PRX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled_prx" CACHE PATH "Lifted firmware PRX output (lift_libsre.ps1)")
set(PORT_SPU_INPUTS "" CACHE STRING "Decrypted ELF(s) to discover and lift SPU programs from")
option(PORT_LIBSRE "Run the lifted firmware libsre instead of the HLE SPURS library" ON)
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)
get_filename_component(PRX_DIR "${PRX_DIR}" ABSOLUTE)

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR "No lifted code in ${RECOMP_DIR}: run lift.ps1")
endif()
if(NOT PORT_SPU_INPUTS)
    message(FATAL_ERROR "PORT_SPU_INPUTS must name the decrypted EBOOT.elf")
endif()

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS "${RECOMP_DIR}/*.cpp")

set(PRX_SOURCES "")
set(FIRMWARE_SPU_SOURCES "")
if(PORT_LIBSRE)
    if(NOT EXISTS "${PRX_DIR}/libsre/libsre_recomp.cpp" OR NOT EXISTS "${PRX_DIR}/libsre_spu/sources.cmake")
        message(FATAL_ERROR "No lifted libsre in ${PRX_DIR}: run lift_libsre.ps1 or set -DPORT_LIBSRE=OFF")
    endif()
    set(PRX_SOURCES "${PRX_DIR}/libsre/libsre_recomp.cpp" "${PRX_DIR}/libsre/libsre_module.c")
    include("${PRX_DIR}/libsre_spu/sources.cmake")
    set(PS3RECOMP_SPU_RETURN_ENTRIES ON)
    set(PS3RECOMP_SPU_GUEST_REGIONS ON)
endif()

add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*.c")
add_custom_command(
    OUTPUT "${HLE_NID_TABLE}"
    COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/gen_hle_nids.py" --out "${HLE_NID_TABLE}" --all
    DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py" "${PS3RECOMP_DIR}/tools/nid_database.py" ${HLE_NID_SOURCES}
    COMMENT "Generating the HLE NID table")

include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/generated_spu" ${PORT_SPU_INPUTS})
set(SPU_SOURCES ${SPU_GENERATED_SOURCES} ${FIRMWARE_SPU_SOURCES} "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")

add_executable(${PROJECT_NAME}
    "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
    "${PS3RECOMP_DIR}/runtime/prx/prx_loader.c"
    prx_init.cpp
    "${HLE_NID_TABLE}"
    ${RECOMP_SOURCES}
    ${PRX_SOURCES}
    ${SPU_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"
    "${PS3RECOMP_DIR}/runtime/prx"
    "${PS3RECOMP_DIR}/libs/video")

if(PORT_LIBSRE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE PORT_HAVE_LIBSRE)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ps3recomp_runtime dbghelp user32 gdi32 winmm)
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

target_compile_options(${PROJECT_NAME} PRIVATE
    /W4 -Wno-unused-parameter
    -fno-strict-aliasing
    -fwrapv
    /clang:-ffp-contract=off)

set_source_files_properties(${RECOMP_SOURCES} ${PRX_SOURCES} PROPERTIES COMPILE_OPTIONS "/w;/bigobj")
if(PORT_LIBSRE)
    set_source_files_properties("${PRX_DIR}/libsre/libsre_recomp.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${PRX_DIR}/libsre")
endif()
set_source_files_properties(${SPU_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
    COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT
    COMPILE_OPTIONS "${PS3RECOMP_OPT_FLAG}")
