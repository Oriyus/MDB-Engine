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

if(EMSCRIPTEN)
    # The web player: Game.html (the page, from web/shell.html), Game.js and Game.wasm. It holds no game: the page
    # downloads game.pak (the project's files, Game/web/README.md) and puts it in /game before main runs.
    # Saves and settings go to /persist (IndexedDB, synced by the page).
    set_target_properties(Game PROPERTIES SUFFIX ".html")
    target_link_options(Game PRIVATE
        -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
        -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sMAXIMUM_MEMORY=4294967296
        -sSTACK_SIZE=8388608
        -sFORCE_FILESYSTEM=1 -lidbfs.js
        "-sEXPORTED_RUNTIME_METHODS=['FS','ENV','addRunDependency','removeRunDependency']"
        -sENVIRONMENT=web
        --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
    set_property(TARGET Game APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)

    # build-web/dist/web: a playable folder of the Examples project (or -DGAME_PROJECT=<folder>):
    #   cmake --build build-web --target package_game_web    then    build-web/dist/web/Play.bat
    set(GAME_PROJECT "${CMAKE_SOURCE_DIR}/Examples" CACHE PATH "The project package_game_web packages")
    set(WEB_DIST_DIR ${CMAKE_BINARY_DIR}/dist/web)
    add_custom_target(package_game_web
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/Tools/package_web.ps1
            -Player $<TARGET_FILE_DIR:Game> -Project ${GAME_PROJECT} -Engine ${CMAKE_SOURCE_DIR}/EngineAssets
            -Out ${WEB_DIST_DIR} -Launcher ${CMAKE_CURRENT_SOURCE_DIR}/web
        DEPENDS Game
        VERBATIM)
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
