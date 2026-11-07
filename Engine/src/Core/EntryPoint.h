#pragma once
#include "Core/Application.h"
#include "Core/Log.h"

// Include this header in exactly ONE .cpp file of your game.
int main(int argc, char** argv) {
    Engine::Log::Init();
    ENGINE_INFO("MDB Engine starting");

    Engine::Application::SetCommandLineArgs(argc, argv);
    auto app = Engine::CreateApplication();
    app->Run();
    app.reset();

    ENGINE_INFO("MDB Engine shut down cleanly");
    return 0;
}