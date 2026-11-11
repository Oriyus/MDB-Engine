# The editor app (MDBEditor.exe): the engine plus its editing UI
add_executable(MDBEditor src/main.cpp src/DemoLevels.cpp src/DemoLevels.h src/EditorHistory.cpp src/EditorHistory.h
    src/AssetBrowser.cpp src/AssetBrowser.h src/Platform.cpp src/Platform.h src/DialogueEditor.cpp src/DialogueEditor.h
    src/Projects.cpp src/Projects.h src/GameBuilder.cpp src/GameBuilder.h src/TerrainEditor.cpp src/TerrainEditor.h
    resources/editor.rc)                                    # the icon
target_link_libraries(MDBEditor PRIVATE Engine)
target_compile_definitions(MDBEditor PRIVATE
    ENGINE_ASSET_DIR="${CMAKE_SOURCE_DIR}/EngineAssets/"    # shaders, Scripts/Lib, the sample model
    EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/Examples/"            # the project opened when there is no other
    MDB_SOURCE_DIR="${CMAKE_SOURCE_DIR}/"
    MDB_BUILD_DIR="${CMAKE_BINARY_DIR}/"                    # Build Game compiles Game.exe (Release) here
    MDB_CMAKE_COMMAND="${CMAKE_COMMAND}"
)

# Stop GLFW from including its own GL header, so glad stays in charge
target_compile_definitions(Engine PUBLIC GLFW_INCLUDE_NONE)

if(MSVC)
    target_compile_options(Engine PUBLIC /utf-8)
endif()
