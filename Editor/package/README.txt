MDB Engine
==========

A 3D game engine and editor for Windows.

Start
-----
Run MDBEditor.exe. The first time, it opens the Examples project (File > Example Levels has the
example games). File > New Project makes your own project in any folder.

Needs Windows 10 or 11 (64-bit), a graphics card with OpenGL 4.5, and the Microsoft Visual C++
Redistributable (x64), which most PCs already have. If the editor does not start, install it from:
https://aka.ms/vs/17/release/vc_redist.x64.exe

What is in this folder
----------------------
MDBEditor.exe   the editor
Player/         the game player that File > Build Game puts into your finished games
EngineAssets/   the engine's shaders and Lua libraries (keep them next to MDBEditor.exe)
Examples/       the example project
Docs/index.html the documentation (Engine, Editor, Scripting)

Sharing your game
-----------------
File > Build Game makes a folder with <your title>.exe and everything it needs. Zip that folder
and send it; players do not need the editor.

Controls (editor)
-----------------
Alt + left mouse orbit, Alt + middle mouse pan, Alt + right mouse or the wheel zoom.
Q select, W move, E rotate, R scale, F frame the selection. Ctrl+P plays, Esc stops.
Help > Controls in the editor lists the rest.
