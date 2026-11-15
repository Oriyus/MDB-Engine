#pragma once
#include <string>

namespace Engine {

    // A splash window with a load bar, shown while an app starts (Windows; elsewhere every call does nothing).
    // It runs on its own thread with its own message loop, so it keeps drawing (and the bar keeps easing towards
    // its target) while the main thread is busy loading. Application opens it when WindowProps::Splash is set,
    // keeps the main window hidden through OnStart and the first frame, then shows that window and closes this.
    //
    // Progress is reported from anywhere with Splash::Report(fraction, status); every call is cheap and safe
    // without a splash. Splash::Range maps the reports inside a step onto part of the bar, so code that knows
    // nothing about the whole start (the scene loader) can still say how far it is:
    //     { Splash::Range r(0.45f, 0.85f); scene.Load(...); }   // the loader's 0..1 lands in 45% .. 85%
    // The bar never goes back.
    namespace Splash {
        void Show(const std::string& title, const std::string& subtitle);
        void Close();
        bool Active();

        void Report(float fraction, const std::string& status);   // fraction 0..1 of the current range
        void Report(float fraction);                               // the same, keeping the status text
        void Status(const std::string& status);                    // text only

        class Range {
        public:
            Range(float begin, float end);      // part of the current range (nested ranges multiply)
            ~Range();
            Range(const Range&) = delete;
            Range& operator=(const Range&) = delete;
        };
    }

} // namespace Engine
