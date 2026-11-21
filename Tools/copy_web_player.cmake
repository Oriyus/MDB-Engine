# package_editor: puts the web player (if it has been built: Tools/build_web.ps1) into the packaged editor's
# Player/Web, so its File > Build Game > Web works without Emscripten.
#   cmake -DSRC=<build-web/bin> -DLAUNCHER=<Game/web> -DDST=<dist/MDB Engine/Player/Web> -P copy_web_player.cmake
if(EXISTS "${SRC}/Game.wasm")
    file(MAKE_DIRECTORY "${DST}")
    file(COPY "${SRC}/Game.html" "${SRC}/Game.js" "${SRC}/Game.wasm" "${LAUNCHER}/Play.bat" "${LAUNCHER}/serve.ps1"
         DESTINATION "${DST}")
    message(STATUS "Web player copied to ${DST}")
else()
    message(STATUS "No web player in ${SRC} (build it with Tools/build_web.ps1): the packaged editor can't build web games")
endif()
