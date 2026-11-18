#include "Renderer/FlickerMeter.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace Engine {

    namespace {
        // A 24-bit BMP from bottom-up RGBA8 rows (BMP rows are bottom-up too)
        bool WriteBmp(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h) {
            const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
            std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
            if (!f) return false;
            auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
            auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
            f.write("BM", 2); u32(size); u32(0); u32(54);
            u32(40); u32(w); u32(h); u16(1); u16(24); u32(0); u32(row * h); u32(2835); u32(2835); u32(0); u32(0);
            std::vector<uint8_t> line(row, 0);
            for (uint32_t y = 0; y < h; y++) {
                for (uint32_t x = 0; x < w; x++) {
                    const uint8_t* p = rgba + (size_t(y) * w + x) * 4;
                    line[x * 3 + 0] = p[2]; line[x * 3 + 1] = p[1]; line[x * 3 + 2] = p[0];
                }
                f.write(reinterpret_cast<const char*>(line.data()), row);
            }
            return (bool)f;
        }
    }

    FlickerMeter::FlickerMeter(std::string folder, int skip, int frames)
        : m_Folder(std::move(folder)), m_Skip(std::max(skip, 1)), m_Frames(std::max(frames, 2)) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(m_Folder), ec);
    }

    void FlickerMeter::Frame(uint32_t texture, uint32_t width, uint32_t height) {
        if (m_Done || !texture || width == 0 || height == 0) return;
        if (++m_Seen < m_Skip) return;
        if (width != m_W || height != m_H) {                 // (re)start on a new size
            m_W = width; m_H = height;
            m_Prev.clear();
            m_Heat.assign(size_t(width) * height, 0);
            m_Measured = 0; m_Over3 = m_Over8 = m_Mean = 0.0;
        }
        m_Cur.resize(size_t(width) * height * 4);
        glGetTextureImage(texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, (GLsizei)m_Cur.size(), m_Cur.data());
        if (!m_Prev.empty()) {
            size_t over3 = 0, over8 = 0;
            double sum = 0.0;
            const size_t n = size_t(width) * height;
            for (size_t i = 0; i < n; i++) {
                const uint8_t* a = &m_Cur[i * 4];
                const uint8_t* b = &m_Prev[i * 4];
                const int d = std::max({ std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2]) });
                sum += d;
                if (d > 3) over3++;
                if (d > 8) { over8++; m_Heat[i]++; }
            }
            m_Over3 += double(over3) / n;
            m_Over8 += double(over8) / n;
            m_Mean += sum / n;
            m_Measured++;
        }
        m_Prev.swap(m_Cur);
        if (m_Measured >= m_Frames) Finish();
    }

    void FlickerMeter::Finish() {
        m_Done = true;
        const double n = std::max(m_Measured, 1);
        ENGINE_INFO("Flicker over {} frames ({}x{}): {:.2f}% of pixels change by more than 3/255, {:.2f}% by more than 8/255, mean change {:.3f}/255",
            m_Measured, m_W, m_H, 100.0 * m_Over3 / n, 100.0 * m_Over8 / n, m_Mean / n);
        WriteBmp(m_Folder + "/frame.bmp", m_Prev.data(), m_W, m_H);
        std::vector<uint8_t> heat(size_t(m_W) * m_H * 4, 255);
        for (size_t i = 0; i < m_Heat.size(); i++) {
            const uint8_t v = (uint8_t)std::min(255.0, 255.0 * m_Heat[i] / n * 2.0);   // half the frames or more = white
            heat[i * 4 + 0] = heat[i * 4 + 1] = heat[i * 4 + 2] = v;
        }
        WriteBmp(m_Folder + "/flicker.bmp", heat.data(), m_W, m_H);
    }

} // namespace Engine
