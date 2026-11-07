# The standalone game: plays a scene with no editor (see src/main.cpp).
add_executable(Game src/main.cpp)
target_link_libraries(Game PRIVATE Engine)
target_compile_definitions(Game PRIVATE
    ASSET_DIR="${CMAKE_SOURCE_DIR}/Assets/"     # fallback when there's no Assets folder next to the exe
)

if(MSVC)
    # Release builds are a normal Windows app (no console window); Debug keeps the console for the log
    target_link_options(Game PRIVATE $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)
endif()

# A folder you can zip and ship: build/dist/<Config>/ with Game.exe and the Assets folder.
#   cmake --build build --config Release --target package_game
set(GAME_DIST_DIR ${CMAKE_BINARY_DIR}/dist/$<CONFIG>)
add_custom_target(package_game
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${GAME_DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GAME_DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:Game> ${GAME_DIST_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Assets ${GAME_DIST_DIR}/Assets
    COMMAND ${CMAKE_COMMAND} -E echo "Game packaged in ${GAME_DIST_DIR}"
    DEPENDS Game
    VERBATIM)
