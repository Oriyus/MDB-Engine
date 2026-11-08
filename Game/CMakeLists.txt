# The standalone game: plays a project with no editor (see src/main.cpp).
add_executable(Game src/main.cpp)
target_link_libraries(Game PRIVATE Engine)
target_compile_definitions(Game PRIVATE
    # used when the exe is not in a packaged game (no project.json next to it): running from the build
    ENGINE_ASSET_DIR="${CMAKE_SOURCE_DIR}/EngineAssets/"
    EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/Examples/"
)

if(MSVC)
    # Release builds are a normal Windows app (no console window); Debug keeps the console for the log
    target_link_options(Game PRIVATE $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)
endif()

# A folder you can zip and ship: build/dist/<Config>/ with Game.exe, EngineAssets, the project's Assets and
# project.json. The editor's File > Build Game does the same for the open project (and names the exe after
# its title, leaving out the project's "exclude" files).
#   cmake --build build --config Release --target package_game                 (the Examples project)
#   cmake -S . -B build -DGAME_PROJECT=D:/Games/MyGame  then the same build line (another project)
set(GAME_PROJECT "${CMAKE_SOURCE_DIR}/Examples" CACHE PATH "The project package_game packages")
set(GAME_DIST_DIR ${CMAKE_BINARY_DIR}/dist/$<CONFIG>)
add_custom_target(package_game
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${GAME_DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GAME_DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Game> ${GAME_DIST_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/EngineAssets ${GAME_DIST_DIR}/EngineAssets
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${GAME_PROJECT}/Assets ${GAME_DIST_DIR}/Assets
    COMMAND ${CMAKE_COMMAND} -E copy ${GAME_PROJECT}/project.json ${GAME_DIST_DIR}/
    COMMAND ${CMAKE_COMMAND} -E echo "Game packaged in ${GAME_DIST_DIR}"
    DEPENDS Game
    VERBATIM)
