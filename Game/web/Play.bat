@echo off
rem Plays this web game in your browser: starts a small local web server (serve.ps1) for this folder.
rem To share the game, upload the whole folder to a web host instead (itch.io: zip it as an HTML game).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve.ps1"
