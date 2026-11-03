cmake_minimum_required(VERSION 3.23)

# CardBridge - Koikatsu character card to PMX, offline (no game process).
#
# Two ways in: configure this folder on its own (scripts\cardbridge.ps1 does,
# into build-cardbridge\), or - Stage 2 - add_subdirectory() it from the
# engine's top-level CMakeLists. Standalone, it fetches the two header
# dependencies itself; as a subproject it reuses the engine's `json` and
# `stb_image` targets and only adds stb_image_write.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(CardBridge LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CARDBRIDGE_STANDALONE ON)
endif()

include(FetchContent)

if(CARDBRIDGE_STANDALONE)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_Declare(
        stb_image
        URL https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h
        URL_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_MakeAvailable(json stb_image)
    add_library(stb_image INTERFACE)
    target_include_directories(stb_image INTERFACE ${stb_image_SOURCE_DIR})
endif()

# Same pinned stb commit as the engine's stb_image.
FetchContent_Declare(
    stb_image_write
    URL https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image_write.h
    URL_HASH SHA256=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(stb_image_write)
add_library(cardbridge_stb_write INTERFACE)
target_include_directories(cardbridge_stb_write INTERFACE ${stb_image_write_SOURCE_DIR})

# The core: no UI, no renderer, no engine headers (plan section 16.1). The
# standalone exe below and the Stage 2 engine importer are both shells over it.
add_library(cardbridge_core STATIC
    src/core/FileIo.cpp
    src/core/Lz4.cpp
    src/core/Inflate.cpp
    src/core/MsgPack.cpp
    src/core/ZipArchive.cpp
    src/card/CardFile.cpp
    src/card/MaterialEdits.cpp
    src/card/Abmx.cpp
    src/unity/UnityBundle.cpp
    src/unity/UnitySerialized.cpp
    src/unity/UnityScene.cpp
    src/unity/TextureDecode.cpp
    src/resolve/ModIndex.cpp
    src/resolve/ListTable.cpp
    src/resolve/Resolver.cpp
    src/assemble/Assembler.cpp
    src/assemble/DynamicBone.cpp
    src/pmx/PmxWriter.cpp
    src/export/Exporter.cpp
    src/assemble/Pose.cpp
    src/assemble/PhysicsSim.cpp
    src/export/BoneLabels.cpp
    src/export/PmxPhysics.cpp
    src/export/SkeletonOverlay.cpp
    src/export/Preview.cpp
    src/export/GpuView.cpp
    src/export/ShaderLayer.cpp
    src/export/ColourGrade.cpp
    src/library/Library.cpp
    src/shape/Shape.cpp
    src/shader/Dxbc.cpp
    src/shader/UnityShader.cpp
    src/shader/Sm4.cpp
    third_party/bc7decomp/bc7decomp.cpp
)
target_include_directories(cardbridge_core PUBLIC src PRIVATE third_party/bc7decomp)
target_link_libraries(cardbridge_core PUBLIC nlohmann_json::nlohmann_json PRIVATE stb_image cardbridge_stb_write)
if(WIN32)
    target_link_libraries(cardbridge_core PUBLIC d3d11)   # the GPU view runs the game's DXBC shaders
endif()
if(MSVC)
    target_compile_options(cardbridge_core PRIVATE /W4)
else()
    target_compile_options(cardbridge_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

add_executable(cardbridge cli/main.cpp)
target_link_libraries(cardbridge PRIVATE cardbridge_core cardbridge_stb_write)
if(WIN32 AND NOT MSVC)
    # The exe has to start from a plain folder with nothing beside it.
    target_link_options(cardbridge PRIVATE -static)
endif()


# The viewer: a Win32 window over the same core (GDI blit of a frame drawn by the software renderer or by Direct3D 11).
if(WIN32)
    add_executable(cardbridge_viewer WIN32 viewer/main.cpp)
    target_link_libraries(cardbridge_viewer PRIVATE cardbridge_core)
    if(NOT MSVC)
        target_link_options(cardbridge_viewer PRIVATE -municode -static)
        target_link_libraries(cardbridge_viewer PRIVATE comdlg32 shell32 ole32)
    endif()
endif()

add_executable(cardbridge_tests tests/cardbridge_tests.cpp)
target_link_libraries(cardbridge_tests PRIVATE cardbridge_core stb_image)
if(WIN32 AND NOT MSVC)
    target_link_options(cardbridge_tests PRIVATE -static)
endif()

# Independent PMX read-back with the engine's own loader (shares no code with
# our writer). Built only when the engine sources are next to this folder.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../engine/src/assets/PmxFile.cpp)
    if(CARDBRIDGE_STANDALONE)
        FetchContent_Declare(
            glm
            GIT_REPOSITORY https://github.com/g-truc/glm.git
            GIT_TAG        1.0.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(glm)
    endif()
    add_executable(cardbridge_pmx_crosscheck
        tests/pmx_crosscheck.cpp
        ../engine/src/assets/PmxFile.cpp
        ../engine/src/core/FileIo.cpp)
    target_include_directories(cardbridge_pmx_crosscheck PRIVATE ../engine/src)
    target_link_libraries(cardbridge_pmx_crosscheck PRIVATE glm::glm nlohmann_json::nlohmann_json)
    if(WIN32 AND NOT MSVC)
        target_link_options(cardbridge_pmx_crosscheck PRIVATE -static)
    endif()
endif()
