#include "Core/SplashScreen.h"
#include "Core/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")     // GradientFill
#endif

namespace Engine::Splash {

    namespace {
        using Clock = std::chrono::steady_clock;

        struct State {
            std::mutex M;
            std::condition_variable Ready;
            std::string Title, Subtitle, Text;
            float Target = 0.0f;                                  // 0..1 of the whole bar
            std::vector<std::pair<float, float>> Ranges;          // the current Range stack (whole-bar units)
            std::thread Thread;
            std::atomic<bool> Active{ false };
            bool Created = false;                                 // the thread made its window (or failed)
            Clock::time_point Start;
            std::vector<std::pair<float, std::string>> Timeline;   // when each status began (MDB_PROFILE=1 logs it)
#ifdef _WIN32
            HWND Wnd = nullptr;
#endif
        };
        State& S() { static State s; return s; }

        void SetText(State& s, const std::string& text) {      // with the lock held
            if (text == s.Text) return;
            s.Text = text;
            s.Timeline.emplace_back(std::chrono::duration<float>(Clock::now() - s.Start).count(), text);
        }

        // The current range, in whole-bar units (call with the lock held)
        std::pair<float, float> Current(State& s) {
            return s.Ranges.empty() ? std::pair<float, float>(0.0f, 1.0f) : s.Ranges.back();
        }

#ifdef _WIN32
        std::wstring Wide(const std::string& s) {
            if (s.empty()) return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
            return w;
        }

        // ---- drawing (all sizes in 96-dpi pixels, scaled)

        constexpr int kWidth = 520, kHeight = 300, kPad = 32;
        constexpr COLORREF kTop = RGB(36, 39, 46), kBottom = RGB(24, 26, 31);
        constexpr COLORREF kAccent = RGB(86, 156, 255), kTrack = RGB(52, 56, 66);
        constexpr COLORREF kText = RGB(236, 238, 242), kDim = RGB(148, 154, 166), kFaint = RGB(98, 104, 116);

        struct Painter {
            float Scale = 1.0f;
            HFONT Title = nullptr, Subtitle = nullptr, Small = nullptr;
            HICON Icon = nullptr;
            float Shown = 0.0f;                  // the bar as drawn, easing towards the target
            Clock::time_point Last = Clock::now();

            int Px(float v) const { return (int)std::lround(v * Scale); }

            void Init(HWND wnd, float scale) {
                Scale = scale;
                auto font = [&](int px, int weight) {
                    return CreateFontW(-Px((float)px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
                };
                Title = font(30, FW_SEMIBOLD);
                Subtitle = font(16, FW_NORMAL);
                Small = font(13, FW_NORMAL);
                // The exe's icon if it has one (the editor's GLFW_ICON resource); none for a plain game
                Icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), L"GLFW_ICON", IMAGE_ICON, Px(72), Px(72), LR_DEFAULTCOLOR);
                if (Icon) {
                    SendMessageW(wnd, WM_SETICON, ICON_BIG, (LPARAM)Icon);
                    SendMessageW(wnd, WM_SETICON, ICON_SMALL, (LPARAM)Icon);
                }
            }
            void Free() {
                for (HFONT f : { Title, Subtitle, Small }) if (f) DeleteObject(f);
                if (Icon) DestroyIcon(Icon);
            }

            void Text(HDC dc, HFONT font, COLORREF color, const std::wstring& s, RECT r, UINT flags) {
                SelectObject(dc, font);
                SetTextColor(dc, color);
                DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_NOPREFIX | DT_SINGLELINE);
            }

            void Fill(HDC dc, RECT r, COLORREF c, int radius) {
                HBRUSH b = CreateSolidBrush(c);
                HPEN p = CreatePen(PS_SOLID, 1, c);
                HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
                if (radius > 0) RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
                else Rectangle(dc, r.left, r.top, r.right, r.bottom);
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(b); DeleteObject(p);
            }

            void Paint(HDC dc, int w, int h, const std::wstring& title, const std::wstring& subtitle, const std::wstring& status) {
                // Background: a soft vertical gradient and a thin accent line on top
                TRIVERTEX v[2] = {
                    { 0, 0, (COLOR16)(GetRValue(kTop) << 8), (COLOR16)(GetGValue(kTop) << 8), (COLOR16)(GetBValue(kTop) << 8), 0xFF00 },
                    { w, h, (COLOR16)(GetRValue(kBottom) << 8), (COLOR16)(GetGValue(kBottom) << 8), (COLOR16)(GetBValue(kBottom) << 8), 0xFF00 } };
                GRADIENT_RECT gr = { 0, 1 };
                GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
                Fill(dc, { 0, 0, w, Px(3) }, kAccent, 0);
                SetBkMode(dc, TRANSPARENT);

                // Icon, title and subtitle
                int x = Px(kPad);
                const int top = Px(52);
                if (Icon) {
                    DrawIconEx(dc, x, top, Icon, Px(72), Px(72), 0, nullptr, DI_NORMAL);
                    x += Px(72 + 20);
                }
                Text(dc, Title, kText, title, { x, top + Px(2), w - Px(kPad), top + Px(44) }, DT_LEFT | DT_BOTTOM | DT_END_ELLIPSIS);
                Text(dc, Subtitle, kDim, subtitle, { x, top + Px(46), w - Px(kPad), top + Px(72) }, DT_LEFT | DT_TOP | DT_END_ELLIPSIS);

                // Status line, percentage, and the bar
                const int barH = Px(6), barY = h - Px(kPad) - barH;
                const int textB = barY - Px(10), textT = textB - Px(20);
                wchar_t pct[16];
                swprintf(pct, 16, L"%d%%", (int)std::floor(Shown * 100.0f + 0.5f));
                Text(dc, Small, kFaint, pct, { w - Px(kPad + 60), textT, w - Px(kPad), textB }, DT_RIGHT | DT_BOTTOM);
                Text(dc, Small, kDim, status, { Px(kPad), textT, w - Px(kPad + 70), textB }, DT_LEFT | DT_BOTTOM | DT_END_ELLIPSIS);

                const RECT track = { Px(kPad), barY, w - Px(kPad), barY + barH };
                Fill(dc, track, kTrack, barH);
                const int fillW = (int)std::lround((track.right - track.left) * std::clamp(Shown, 0.0f, 1.0f));
                if (fillW > barH / 2) Fill(dc, { track.left, track.top, track.left + std::max(fillW, barH), track.bottom }, kAccent, barH);
            }
        };

        LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
            auto* p = (Painter*)GetWindowLongPtrW(wnd, GWLP_USERDATA);
            switch (msg) {
                case WM_ERASEBKGND: return 1;                       // all drawing is in WM_PAINT (no flicker)
                case WM_TIMER: {
                    if (!p) break;
                    float target;
                    { std::lock_guard<std::mutex> lk(S().M); target = S().Target; }
                    const auto now = Clock::now();
                    const float dt = std::chrono::duration<float>(now - p->Last).count();
                    p->Last = now;
                    // Ease towards the target: quick to follow, never jumping. Once there, creep slowly on (a few
                    // percent at most) so a long single step, like the sky bake, doesn't look like a hang. Never back.
                    float next = p->Shown + (target - p->Shown) * (1.0f - std::exp(-dt * 9.0f));
                    if (target - next < 0.002f) {
                        const float ceiling = std::min(target + 0.05f, 0.99f);
                        next = std::max(target, p->Shown) + (ceiling - std::max(target, p->Shown)) * (1.0f - std::exp(-dt * 0.6f));
                    }
                    p->Shown = std::max(p->Shown, std::min(next, 1.0f));
                    InvalidateRect(wnd, nullptr, FALSE);
                    return 0;
                }
                case WM_PAINT: {
                    PAINTSTRUCT ps;
                    HDC dc = BeginPaint(wnd, &ps);
                    RECT rc; GetClientRect(wnd, &rc);
                    if (p) {
                        std::wstring title, subtitle, status;
                        {
                            std::lock_guard<std::mutex> lk(S().M);
                            title = Wide(S().Title); subtitle = Wide(S().Subtitle); status = Wide(S().Text);
                        }
                        // Drawn off screen, then copied in one go
                        HDC mem = CreateCompatibleDC(dc);
                        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                        HGDIOBJ old = SelectObject(mem, bmp);
                        p->Paint(mem, rc.right, rc.bottom, title, subtitle, status);
                        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                        SelectObject(mem, old);
                        DeleteObject(bmp);
                        DeleteDC(mem);
                    }
                    EndPaint(wnd, &ps);
                    return 0;
                }
                case WM_CLOSE: DestroyWindow(wnd); return 0;
                case WM_DESTROY: PostQuitMessage(0); return 0;
            }
            return DefWindowProcW(wnd, msg, wp, lp);
        }

        void ThreadMain() {
            State& s = S();
            // Sharp text on scaled displays (GLFW makes the process per-monitor aware later; this thread needs it now)
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

            HINSTANCE inst = GetModuleHandleW(nullptr);
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.style = CS_DROPSHADOW;
            wc.lpfnWndProc = WndProc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32650)) /* IDC_APPSTARTING */;   // the "working in the background" arrow
            wc.lpszClassName = L"MDBEngineSplash";
            RegisterClassExW(&wc);

            // Centred on the monitor under the mouse (where the user just double-clicked)
            POINT cursor; GetCursorPos(&cursor);
            HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(mon, &mi);
            UINT dpiX = 96, dpiY = 96;
            using GetDpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);   // shcore, Windows 8.1+
            if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
                if (auto fn = (GetDpiFn)GetProcAddress(shcore, "GetDpiForMonitor")) fn(mon, 0, &dpiX, &dpiY);
                FreeLibrary(shcore);
            }
            const float scale = std::max(1.0f, dpiX / 96.0f);
            const int w = (int)std::lround(kWidth * scale), h = (int)std::lround(kHeight * scale);
            const RECT& wa = mi.rcWork;
            const int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) / 2;

            std::wstring caption;
            { std::lock_guard<std::mutex> lk(s.M); caption = Wide(s.Title); }
            HWND wnd = CreateWindowExW(0, wc.lpszClassName, caption.c_str(), WS_POPUP, x, y, w, h, nullptr, nullptr, inst, nullptr);
            Painter painter;
            if (wnd) {
                painter.Init(wnd, scale);
                SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)&painter);
                const int round = 2;                                  // DWMWCP_ROUND: Windows 11 rounded corners
                DwmSetWindowAttribute(wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
                ShowWindow(wnd, SW_SHOW);
                UpdateWindow(wnd);
                SetForegroundWindow(wnd);
                SetTimer(wnd, 1, 15, nullptr);
            }
            {
                std::lock_guard<std::mutex> lk(s.M);
                s.Wnd = wnd;
                s.Created = true;
            }
            s.Ready.notify_all();
            if (!wnd) return;

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            painter.Free();
            UnregisterClassW(wc.lpszClassName, inst);
        }
#endif
    }

    void Show(const std::string& title, const std::string& subtitle) {
#ifdef _WIN32
        State& s = S();
        if (s.Active) return;
        {
            std::lock_guard<std::mutex> lk(s.M);
            s.Title = title; s.Subtitle = subtitle; s.Text = "Starting";
            s.Target = 0.0f; s.Ranges.clear(); s.Created = false; s.Wnd = nullptr;
        }
        s.Timeline.clear();
        s.Start = Clock::now();
        s.Active = true;
        s.Thread = std::thread(ThreadMain);
        std::unique_lock<std::mutex> lk(s.M);
        s.Ready.wait(lk, [&] { return s.Created; });          // a few ms: the window is up before loading starts
        if (!s.Wnd) { lk.unlock(); s.Thread.join(); s.Active = false; }
#else
        (void)title; (void)subtitle;
#endif
    }

    void Close() {
#ifdef _WIN32
        State& s = S();
        if (!s.Active) return;
        HWND wnd;
        { std::lock_guard<std::mutex> lk(s.M); wnd = s.Wnd; s.Wnd = nullptr; }
        if (wnd) PostMessageW(wnd, WM_CLOSE, 0, 0);
        if (s.Thread.joinable()) s.Thread.join();
        s.Active = false;
        const float total = std::chrono::duration<float>(Clock::now() - s.Start).count();
        ENGINE_INFO("Started in {:.2f} s", total);
        if (const char* prof = std::getenv("MDB_PROFILE"); prof && std::string(prof) == "1") {
            for (size_t i = 0; i < s.Timeline.size(); i++) {
                const float end = i + 1 < s.Timeline.size() ? s.Timeline[i + 1].first : total;
                ENGINE_INFO("  start-up {:6.0f} ms  {}", (end - s.Timeline[i].first) * 1000.0f, s.Timeline[i].second);
            }
        }
#endif
    }

    bool Active() { return S().Active; }

    void Report(float fraction, const std::string& status) {
        State& s = S();
        if (!s.Active) return;
        std::lock_guard<std::mutex> lk(s.M);
        const auto [a, b] = Current(s);
        s.Target = std::max(s.Target, a + (b - a) * std::clamp(fraction, 0.0f, 1.0f));
        SetText(s, status);
    }

    void Report(float fraction) {
        State& s = S();
        if (!s.Active) return;
        std::lock_guard<std::mutex> lk(s.M);
        const auto [a, b] = Current(s);
        s.Target = std::max(s.Target, a + (b - a) * std::clamp(fraction, 0.0f, 1.0f));
    }

    void Status(const std::string& status) {
        State& s = S();
        if (!s.Active) return;
        std::lock_guard<std::mutex> lk(s.M);
        SetText(s, status);
    }

    Range::Range(float begin, float end) {
        State& s = S();
        std::lock_guard<std::mutex> lk(s.M);
        const auto [a, b] = Current(s);
        s.Ranges.emplace_back(a + (b - a) * begin, a + (b - a) * end);
    }

    Range::~Range() {
        State& s = S();
        std::lock_guard<std::mutex> lk(s.M);
        if (!s.Ranges.empty()) {
            s.Target = std::max(s.Target, s.Ranges.back().second);    // a finished step fills its part
            s.Ranges.pop_back();
        }
    }

} // namespace Engine::Splash
