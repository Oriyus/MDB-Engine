add_executable(Sandbox src/main.cpp src/DemoLevels.cpp src/DemoLevels.h src/EditorHistory.cpp src/EditorHistory.h)
target_link_libraries(Sandbox PRIVATE Engine)
target_compile_definitions(Sandbox PRIVATE
    ASSET_DIR="${CMAKE_SOURCE_DIR}/Assets/"
)

# Stop GLFW from including its own GL header, so glad stays in charge
target_compile_definitions(Engine PUBLIC GLFW_INCLUDE_NONE)

if(MSVC)
    target_compile_options(Engine PUBLIC /utf-8)
endif()