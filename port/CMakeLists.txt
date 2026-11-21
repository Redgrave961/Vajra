cmake_minimum_required(VERSION 3.20)
project(AsuraRecomp LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT (MINGW OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    message(FATAL_ERROR "Build with build.ps1 (llvm-mingw, or -Target linux) or build.sh")
endif()

cmake_host_system_information(RESULT ASURA_CORES QUERY NUMBER_OF_LOGICAL_CORES)
set(ASURA_ARCH "x86-64-v3" CACHE STRING "-march level")
set(ASURA_LTO "thin" CACHE STRING "thin or full")
set(ASURA_PGO "auto" CACHE STRING "auto: use pgo/asura.profdata, merged from pgo/raw when newer; gen: instrument; off")
set(ASURA_JOBS "${ASURA_CORES}" CACHE STRING "LTO backend jobs")
set(ASURA_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "PGO profiles")
set(PS3RECOMP_OPT_FLAG "-O3" CACHE STRING "Optimisation flag of the runtime and the lifted SPU code" FORCE)

set(ASURA_FLAGS "-O3 -DNDEBUG -march=${ASURA_ARCH} -fno-math-errno -ffunction-sections -fdata-sections")
set(ASURA_LINK "-Wl,--gc-sections -Wl,--icf=all -Wl,--lto-O3 -Wl,--lto-CGO3")
if(MINGW)
    string(APPEND ASURA_FLAGS " -fno-emulated-tls")
    string(APPEND ASURA_LINK " -Wl,--pdb= -Wl,-s")
endif()
if(ASURA_LTO STREQUAL "full")
    string(APPEND ASURA_FLAGS " -flto")
    if(MINGW)
        string(APPEND ASURA_LINK " -Wl,--Xlink=-opt:lldltopartitions=${ASURA_JOBS}")
    else()
        string(APPEND ASURA_LINK " -Wl,--lto-partitions=${ASURA_JOBS}")
    endif()
else()
    string(APPEND ASURA_FLAGS " -flto=thin")
    string(APPEND ASURA_LINK " -Wl,--thinlto-jobs=${ASURA_JOBS} -Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache -Wl,--thinlto-cache-policy=cache_size_bytes=8g")
endif()

set(ASURA_PROFILE "${ASURA_PGO_DIR}/asura.profdata")
set(ASURA_PGO_NAMES "-mllvm -static-func-full-module-prefix=false")
if(ASURA_PGO STREQUAL "gen")
    file(GLOB ASURA_RAW "${ASURA_PGO_DIR}/raw/*.profraw")
    if(ASURA_RAW)
        file(REMOVE ${ASURA_RAW})
    endif()
    file(MAKE_DIRECTORY "${ASURA_PGO_DIR}/raw")
    string(APPEND ASURA_FLAGS " -fprofile-instr-generate=${ASURA_PGO_DIR}/raw/asura-%p.profraw -DASURA_PGO_GEN ${ASURA_PGO_NAMES}")
    string(APPEND ASURA_LINK " -Wl,-mllvm,-static-func-full-module-prefix=false")
    set(ASURA_PROFILE "")
elseif(ASURA_PGO STREQUAL "auto")
    file(GLOB ASURA_RAW "${ASURA_PGO_DIR}/raw/*.profraw")
    foreach(raw IN LISTS ASURA_RAW)
        if("${raw}" IS_NEWER_THAN "${ASURA_PROFILE}")
            get_filename_component(ASURA_LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
            find_program(ASURA_PROFDATA llvm-profdata HINTS "${ASURA_LLVM_BIN}" REQUIRED)
            execute_process(COMMAND "${ASURA_PROFDATA}" merge -o "${ASURA_PROFILE}" ${ASURA_RAW} COMMAND_ERROR_IS_FATAL ANY)
            break()
        endif()
    endforeach()
    if(EXISTS "${ASURA_PROFILE}")
        string(APPEND ASURA_FLAGS " -fprofile-instr-use=${ASURA_PROFILE} -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin ${ASURA_PGO_NAMES}")
        string(APPEND ASURA_LINK " -Wl,-mllvm,-static-func-full-module-prefix=false")
    else()
        set(ASURA_PROFILE "")
    endif()
else()
    set(ASURA_PROFILE "")
endif()

set(CMAKE_C_FLAGS_RELEASE "${ASURA_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "${ASURA_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${ASURA_LINK}")

set(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" CACHE PATH "ps3recomp toolkit")
set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH "Lifted PPU output (lift.py)")
set(PRX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled_prx" CACHE PATH "Lifted firmware PRX output (lift_firmware.py)")
set(PORT_SPU_INPUTS "" CACHE STRING "Decrypted ELF(s) to discover and lift SPU programs from")
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)
get_filename_component(PRX_DIR "${PRX_DIR}" ABSOLUTE)

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR "No lifted code in ${RECOMP_DIR}: run lift.py")
endif()
if(NOT PORT_SPU_INPUTS)
    message(FATAL_ERROR "PORT_SPU_INPUTS must name the decrypted EBOOT.elf")
endif()

file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS "${RECOMP_DIR}/*.cpp")

set(PRX_MODULES libsre libresc)
set(PRX_SOURCES "")
foreach(m IN LISTS PRX_MODULES)
    if(NOT EXISTS "${PRX_DIR}/${m}/${m}_recomp.cpp")
        message(FATAL_ERROR "No lifted ${m} in ${PRX_DIR}: run lift_firmware.py")
    endif()
    list(APPEND PRX_SOURCES "${PRX_DIR}/${m}/${m}_recomp.cpp" "${PRX_DIR}/${m}/${m}_module.c")
    set_source_files_properties("${PRX_DIR}/${m}/${m}_recomp.cpp" PROPERTIES INCLUDE_DIRECTORIES "${PRX_DIR}/${m}")
endforeach()
if(NOT EXISTS "${PRX_DIR}/libsre_spu/sources.cmake")
    message(FATAL_ERROR "No lifted libsre SPU code in ${PRX_DIR}: run lift_firmware.py")
endif()
set(FIRMWARE_SPU_SOURCES "")
include("${PRX_DIR}/libsre_spu/sources.cmake")
set(PS3RECOMP_SPU_RETURN_ENTRIES ON)
set(PS3RECOMP_SPU_GUEST_REGIONS ON)
set(PS3RECOMP_SPU_MERGE_CHUNKS ON)

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
list(GET PORT_SPU_INPUTS 0 PORT_EBOOT)
ps3recomp_generate_spu_jobs("${CMAKE_CURRENT_BINARY_DIR}/generated_spujobs" "${PORT_EBOOT}")
set(SPU_SOURCES ${SPU_GENERATED_SOURCES} ${SPU_JOB_SOURCES} ${FIRMWARE_SPU_SOURCES})

get_filename_component(PORT_GAME_DIR "${PORT_EBOOT}" DIRECTORY)
get_filename_component(PORT_GAME_DIR "${PORT_GAME_DIR}" DIRECTORY)
if(WIN32)
    set(PORT_ICON "${CMAKE_CURRENT_BINARY_DIR}/AsurasWrath.ico")
else()
    set(PORT_STAGE "${CMAKE_CURRENT_BINARY_DIR}/appimage")
    set(PORT_ICON "${PORT_STAGE}/AsurasWrath.png")
endif()
add_custom_command(
    OUTPUT "${PORT_ICON}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icon.py" "${PORT_GAME_DIR}/ICON0.PNG" "${PORT_ICON}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icon.py" "${PORT_GAME_DIR}/ICON0.PNG"
    COMMENT "Building the icon from the game's ICON0.PNG")
set(PORT_PLATFORM_SOURCES "")
if(WIN32)
    set(PORT_RC "${CMAKE_CURRENT_BINARY_DIR}/AsurasWrath.rc")
    file(WRITE "${PORT_RC}"
"1 ICON \"AsurasWrath.ico\"
1 VERSIONINFO
FILEVERSION 1,0,0,0
PRODUCTVERSION 1,0,0,0
FILEOS 0x40004
FILETYPE 0x1
BEGIN
  BLOCK \"StringFileInfo\"
  BEGIN
    BLOCK \"040904B0\"
    BEGIN
      VALUE \"FileDescription\", \"Asura's Wrath\"
      VALUE \"ProductName\", \"Asura's Wrath\"
      VALUE \"InternalName\", \"AsurasWrath\"
      VALUE \"OriginalFilename\", \"AsurasWrath.exe\"
      VALUE \"FileVersion\", \"1.0.0.0\"
      VALUE \"ProductVersion\", \"1.0.0.0\"
    END
  END
  BLOCK \"VarFileInfo\"
  BEGIN
    VALUE \"Translation\", 0x409, 1200
  END
END
")
    set_source_files_properties("${PORT_RC}" PROPERTIES OBJECT_DEPENDS "${PORT_ICON}")
    set(PORT_PLATFORM_SOURCES "${PORT_RC}")
endif()

add_executable(${PROJECT_NAME}
    ${PORT_PLATFORM_SOURCES}
    main.cpp
    button_prompts.cpp
    frame_time.cpp
    game_files.cpp
    firmware.cpp
    setup_window.cpp
    native_frame.cpp
    native_octree.cpp
    native_engine.cpp
    native_fence.cpp
    native_ring.cpp
    native_script.cpp
    native_script_core.cpp
    pc_options.cpp
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

find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE ps3recomp_runtime SDL3::SDL3)
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX WIN32_EXECUTABLE TRUE OUTPUT_NAME AsurasWrath)
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE dbghelp winmm)
    target_link_options(${PROJECT_NAME} PRIVATE -static)
else()
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,--wrap=pthread_create)
endif()

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fno-strict-aliasing
    -fms-volatile
    -fwrapv
    -ffp-contract=off)

file(SHA256 "${PORT_EBOOT}" PORT_EBOOT_SHA256)
set_source_files_properties(game_files.cpp PROPERTIES COMPILE_DEFINITIONS PORT_EBOOT_SHA256="${PORT_EBOOT_SHA256}")
set_source_files_properties(${RECOMP_SOURCES} ${PRX_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties(${SPU_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
    COMPILE_OPTIONS "${PS3RECOMP_OPT_FLAG}")

if(ASURA_PROFILE)
    foreach(target IN ITEMS ${PROJECT_NAME} ps3recomp_runtime)
        get_target_property(sources ${target} SOURCES)
        set_property(SOURCE ${sources} TARGET_DIRECTORY ${target} APPEND PROPERTY OBJECT_DEPENDS "${ASURA_PROFILE}")
    endforeach()
endif()

if(NOT WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PORT_STAGE}")
    set(PORT_RUNTIME "${PORT_STAGE}/runtime-x86_64")
    file(DOWNLOAD https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 "${PORT_RUNTIME}"
        EXPECTED_HASH SHA256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d)
    set(PORT_DESKTOP "${PORT_STAGE}/AsurasWrath.desktop")
    file(WRITE "${PORT_DESKTOP}" "[Desktop Entry]\nType=Application\nName=Asura's Wrath\nExec=AsurasWrath\nIcon=AsurasWrath\nCategories=Game;\nTerminal=false\nStartupWMClass=AsurasWrath\n")
    set(PORT_STRIPPED "${PORT_STAGE}/AsurasWrath.stripped")
    set(PORT_DEBUG "${PORT_STAGE}/AsurasWrath.debug")
    file(WRITE "${PORT_STAGE}/AppDir.list"
"file /AsurasWrath 0755 0 0 ${PORT_STRIPPED}
slink /AppRun 0777 0 0 AsurasWrath
file /AsurasWrath.desktop 0644 0 0 ${PORT_DESKTOP}
file /AsurasWrath.png 0644 0 0 ${PORT_ICON}
slink /.DirIcon 0777 0 0 AsurasWrath.png
")
    find_program(ASURA_GENSQUASHFS gensquashfs REQUIRED)
    set(PORT_SQUASHFS "${PORT_STAGE}/AppDir.squashfs")
    set(PORT_APPIMAGE "${CMAKE_CURRENT_BINARY_DIR}/AsurasWrath-x86_64.AppImage")
    add_custom_command(
        OUTPUT "${PORT_APPIMAGE}"
        COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:${PROJECT_NAME}>" "${PORT_DEBUG}"
        COMMAND "${CMAKE_OBJCOPY}" --strip-all "--add-gnu-debuglink=${PORT_DEBUG}" "$<TARGET_FILE:${PROJECT_NAME}>" "${PORT_STRIPPED}"
        COMMAND "${ASURA_GENSQUASHFS}" --pack-file "${PORT_STAGE}/AppDir.list" --compressor zstd --defaults mode=0755 --force "${PORT_SQUASHFS}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_appimage.py" "${PORT_RUNTIME}" "${PORT_SQUASHFS}" "${PORT_APPIMAGE}"
        DEPENDS ${PROJECT_NAME} "${PORT_ICON}" "${PORT_DESKTOP}" "${PORT_STAGE}/AppDir.list" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_appimage.py"
        COMMENT "Packing AsurasWrath-x86_64.AppImage")
    add_custom_target(AppImage ALL DEPENDS "${PORT_APPIMAGE}")
endif()