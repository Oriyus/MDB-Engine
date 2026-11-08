# The editor app (MDBEditor.exe): the engine plus its editing UI
add_executable(MDBEditor src/main.cpp src/DemoLevels.cpp src/DemoLevels.h src/EditorHistory.cpp src/EditorHistory.h
    src/AssetBrowser.cpp src/AssetBrowser.h src/Platform.cpp src/Platform.h)
target_link_libraries(MDBEditor PRIVATE Engine)
target_compile_definitions(MDBEditor PRIVATE
    ASSET_DIR="${CMAKE_SOURCE_DIR}/Assets/"
)

# Stop GLFW from including its own GL header, so glad stays in charge
target_compile_definitions(Engine PUBLIC GLFW_INCLUDE_NONE)

if(MSVC)
    target_compile_options(Engine PUBLIC /utf-8)
endif()