#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

    // Temporal stability, for tests (the Game's MDB_FLICKER hook): with the camera still, how much the final picture
    // changes from one frame to the next (TAA leftovers, per-frame jittered rays, shimmering edges, swaying grass).
    // Each frame after `skip` warm-up frames it reads the image back (a GPU stall: for measuring only) and compares it
    // with the frame before. After `frames` frames it logs the averages (share of pixels changing by more than 3 and 8
    // of 255, mean change) and writes <folder>/frame.bmp (the last frame) and <folder>/flicker.bmp (per pixel, how
    // often it changed by more than 8: black never, white every frame).
    class FlickerMeter {
    public:
        FlickerMeter(std::string folder, int skip = 120, int frames = 240);
        // texture: the final image (RGBA8, bottom-up), width x height
        void Frame(uint32_t texture, uint32_t width, uint32_t height);
        bool Done() const { return m_Done; }

    private:
        void Finish();
        std::string m_Folder;
        int m_Skip, m_Frames, m_Seen = 0, m_Measured = 0;
        uint32_t m_W = 0, m_H = 0;
        std::vector<uint8_t> m_Prev, m_Cur;
        std::vector<uint16_t> m_Heat;
        double m_Over3 = 0.0, m_Over8 = 0.0, m_Mean = 0.0;
        bool m_Done = false;
    };

} // namespace Engine
