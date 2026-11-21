#pragma once
#include "Core/Application.h"
#include "Core/Log.h"

// Include this header in exactly ONE .cpp file of your game.
int main(int argc, char** argv) {
    Engine::Log::Init();
    ENGINE_INFO("MDB Engine starting");

    Engine::Application::SetCommandLineArgs(argc, argv);
#ifdef __EMSCRIPTEN__
    // The browser calls the frame loop after main has returned (Application::Run): the app must outlive main, and
    // the "unwind" Emscripten throws to leave main runs destructors, so nothing here owns it
    Engine::Application* app = Engine::CreateApplication().release();
    app->Run();
    return 0;
#else
    auto app = Engine::CreateApplication();
    app->Run();
    app.reset();
#endif

    ENGINE_INFO("MDB Engine shut down cleanly");
    return 0;
}