add_library(ootmm_core
    src/AnchorBridge.cpp
    src/BootConfig.cpp
    src/Camc.cpp
    src/FilePacketBridge.cpp
    src/GrantApplier.cpp
    src/Hints.cpp
    src/ItemGrant.cpp
    src/NativeItem.cpp
    src/NativeGrantAction.cpp
    src/NativePortSession.cpp
    src/PortBridge.cpp
    src/Presence.cpp
    src/Runtime.cpp
    src/Seed.cpp
    src/SimpleJson.cpp
)

target_include_directories(ootmm_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(ootmm_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ootmm_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(ootmm_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
