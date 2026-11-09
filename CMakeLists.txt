cmake_minimum_required(VERSION 3.25)
project(MyEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)

FetchContent_Declare(glfw  GIT_REPOSITORY https://github.com/glfw/glfw.git  GIT_TAG 3.4)
FetchContent_Declare(glm   GIT_REPOSITORY https://github.com/g-truc/glm.git  GIT_TAG 1.0.1)
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.14.1)
FetchContent_Declare(entt  GIT_REPOSITORY https://github.com/skypjack/entt.git GIT_TAG v3.13.2)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0-docking)
FetchContent_Declare(assimp GIT_REPOSITORY https://github.com/assimp/assimp.git GIT_TAG v5.4.3)
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_Declare(lua  GIT_REPOSITORY https://github.com/lua/lua.git GIT_TAG v5.4.6)   # no CMake: built in Engine/CmakeLists.txt
FetchContent_Declare(sol2 GIT_REPOSITORY https://github.com/ThePhD/sol2.git GIT_TAG v3.3.0)
# miniaudio: only the header is used (implementation compiled in Engine/src/Audio/AudioEngine.cpp).
# SOURCE_SUBDIR points at a folder without a CMakeLists.txt, so its own CMake build is skipped.
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG 0.11.22
    SOURCE_SUBDIR header-only)

FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.2.0
    SOURCE_SUBDIR Build)          # Jolt's CMakeLists lives in Build/

# Jolt defaults to the *static* MSVC runtime, which would clash with the rest of the project
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)   # warnings-as-errors would break on a very new MSVC, like assimp did
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw glm spdlog entt imgui assimp json JoltPhysics lua sol2 miniaudio)

add_subdirectory(vendor/glad)
add_subdirectory(Engine)
add_subdirectory(Editor)
add_subdirectory(Game)

# Debug builds of the code that runs every frame inline what is declared inline (/Ob1 instead of /Ob0): glm,
# EnTT, the standard library, small accessors. Still unoptimised (/Od) and debuggable, but maths-heavy loops no
# longer pay a call for every vector operation. No ABI change (runtime and iterator checks stay). The later
# /Ob1 overrides the /Ob0 of the Debug flags (MSVC warning D9025). Libraries used only while loading (assimp,
# glfw, spdlog, Lua) keep their defaults.
if(MSVC)
    foreach(t Engine MDBEditor Game Jolt imgui)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE $<$<CONFIG:Debug>:/Ob1>)
        endif()
    endforeach()
endif()

# Visual Studio: F5 runs the editor
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT MDBEditor)
# The editor as a folder you can zip and give to someone who will not build it:
# build/dist/MDB Engine/ with MDBEditor.exe, Player/Game.exe (the prebuilt player File > Build Game copies),
# EngineAssets, the Examples project and the documentation. The editor finds them next to its exe
# (Projects::Packaged). Build it in Release:
#   cmake --build build --config Release --target package_editor
set(EDITOR_DIST_DIR "${CMAKE_BINARY_DIR}/dist/MDB Engine")
add_custom_target(package_editor
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${EDITOR_DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${EDITOR_DIST_DIR}/Player ${EDITOR_DIST_DIR}/Examples
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:MDBEditor> ${EDITOR_DIST_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Game> ${EDITOR_DIST_DIR}/Player/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/EngineAssets ${EDITOR_DIST_DIR}/EngineAssets
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Examples/Assets ${EDITOR_DIST_DIR}/Examples/Assets
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/Examples/project.json ${EDITOR_DIST_DIR}/Examples/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Docs ${EDITOR_DIST_DIR}/Docs
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/Editor/package/README.txt ${EDITOR_DIST_DIR}/
    COMMAND ${CMAKE_COMMAND} -E echo "Editor packaged in ${EDITOR_DIST_DIR}"
    DEPENDS MDBEditor Game
    VERBATIM)
