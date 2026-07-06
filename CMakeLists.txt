cmake_minimum_required(VERSION 3.20)

project(OOTMMPCNative LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OOTMMPC_BUILD_TESTS "Build OOTMMPC tests" ON)

add_subdirectory(src/ootmm)

# Dedicated multiworld server: the relay with no GUI and no game processes, for always-on hosting.
add_executable(ootmm_server src/server/main.cpp src/launcher/RelayServer.cpp)
target_link_libraries(ootmm_server PRIVATE ootmm_core)
target_include_directories(ootmm_server PRIVATE src/launcher)
target_compile_features(ootmm_server PRIVATE cxx_std_20)
if(WIN32)
    target_link_libraries(ootmm_server PRIVATE ws2_32)
endif()

# The foreign-model mods builder is table-driven from the ports' generated dispatch tables:
# extract the referenced object dirs at configure time so a regenerated .inc flows into the
# launcher on the next build with no hand-maintained lists.
function(ootmm_extract_model_dirs INC_PATH OUT_VAR)
    set(entries "")
    if(EXISTS ${INC_PATH})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${INC_PATH})
        file(READ ${INC_PATH} _inc_text)
        string(REGEX MATCHALL "__OTR__objects/[A-Za-z0-9_.-]+/" _matches "${_inc_text}")
        list(REMOVE_DUPLICATES _matches)
        list(SORT _matches)
        foreach(_m ${_matches})
            string(REGEX REPLACE "^__OTR__(objects/[A-Za-z0-9_.-]+)/$" "\\1" _dir "${_m}")
            string(APPEND entries "    \"${_dir}\",\n")
        endforeach()
    endif()
    set(${OUT_VAR} "${entries}" PARENT_SCOPE)
endfunction()
ootmm_extract_model_dirs(
    ${CMAKE_SOURCE_DIR}/external/Shipwright/soh/soh/Enhancements/ootmm/OotmmSohForeignModels.inc
    OOTMM_SOH_MODEL_DIRS)
ootmm_extract_model_dirs(
    ${CMAKE_SOURCE_DIR}/external/2ship2harkinian/mm/2s2h/Enhancements/Ootmm/Ootmm2s2hForeignModels.inc
    OOTMM_2S2H_MODEL_DIRS)
configure_file(src/launcher/ForeignModelTables.h.in ${CMAKE_BINARY_DIR}/generated/ForeignModelTables.h @ONLY)

set(OOTMM_LAUNCHER_SOURCES src/launcher/main.cpp src/launcher/RelayServer.cpp)
if(WIN32)
    # The GUI launcher (config screen + runtime manager sidebar) renders with a
    # vendored Dear ImGui on Direct3D 11; Windows-only, the console path needs none of it.
    list(APPEND OOTMM_LAUNCHER_SOURCES
        src/launcher/GameConfig.cpp
        src/launcher/LauncherConfig.cpp
        src/launcher/LauncherUi.cpp
        src/launcher/LogicSolver.cpp
        src/launcher/ModelRepack.cpp
        src/launcher/ForeignModelMods.cpp
        src/launcher/CsmcArtData.gen.cpp
        src/launcher/Tracker.cpp
        third_party/miniz/miniz.c
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/backends/imgui_impl_win32.cpp
        third_party/imgui/backends/imgui_impl_dx11.cpp
    )
endif()
add_executable(ootmm_launcher ${OOTMM_LAUNCHER_SOURCES})
target_link_libraries(ootmm_launcher PRIVATE ootmm_core)
target_compile_features(ootmm_launcher PRIVATE cxx_std_20)
if(WIN32)
    target_include_directories(ootmm_launcher PRIVATE third_party third_party/imgui src/ootmm/src
                                                       ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(ootmm_launcher PRIVATE ws2_32 d3d11 dxgi comdlg32 dwmapi)
    # GUI subsystem (no console window); main() is kept as the entry point, and CLI
    # modes reattach to the parent console for log/usage output.
    target_link_options(ootmm_launcher PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

if(OOTMMPC_BUILD_TESTS)
    enable_testing()
    add_executable(ootmm_core_tests tests/ootmm_core_tests.cpp)
    target_link_libraries(ootmm_core_tests PRIVATE ootmm_core)
    add_test(NAME ootmm_core_tests COMMAND ootmm_core_tests)

    # Headless stand-in port for coordinator integration testing.
    add_executable(ootmm_stub_port tests/ootmm_stub_port.cpp)
    target_link_libraries(ootmm_stub_port PRIVATE ootmm_core)
    target_compile_features(ootmm_stub_port PRIVATE cxx_std_20)

    # Standalone repro harness for loading a real seed file through ootmm_core.
    add_executable(ootmm_load_testseed tests/load_testseed.cpp)
    target_link_libraries(ootmm_load_testseed PRIVATE ootmm_core)
    target_compile_features(ootmm_load_testseed PRIVATE cxx_std_20)
endif()
