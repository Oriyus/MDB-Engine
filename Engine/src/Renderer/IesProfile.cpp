#include "Renderer/IesProfile.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace Engine {

    namespace {
        // Value at x in a rising list of angles: index below and the share towards the next (false outside)
        bool Locate(const std::vector<float>& a, float x, size_t& i, float& t) {
            if (a.empty() || x < a.front() - 1e-4f || x > a.back() + 1e-4f) return false;
            if (a.size() == 1) { i = 0; t = 0.0f; return true; }
            i = (size_t)(std::upper_bound(a.begin(), a.end(), x) - a.begin());
            i = std::clamp(i, (size_t)1, a.size() - 1) - 1;
            const float span = a[i + 1] - a[i];
            t = span > 0.0f ? std::clamp((x - a[i]) / span, 0.0f, 1.0f) : 0.0f;
            return true;
        }
    }

    float IesProfile::Sample(float v, float h) const {
        if (Vertical.empty() || Horizontal.empty()) return 0.0f;
        // Unfold the symmetry the file uses
        h = std::fmod(h, 360.0f);
        if (h < 0.0f) h += 360.0f;
        const float hMax = Horizontal.back();
        if (Horizontal.size() == 1) h = Horizontal[0];                       // the same all round
        else if (hMax <= 90.0f + 1e-3f) {                                     // one quadrant mirrored both ways
            if (h > 180.0f) h = 360.0f - h;
            if (h > 90.0f) h = 180.0f - h;
        } else if (hMax <= 180.0f + 1e-3f) {                                  // one side mirrored
            if (h > 180.0f) h = 360.0f - h;
        }
        size_t vi, hi;
        float vt, ht;
        if (!Locate(Vertical, v, vi, vt)) return 0.0f;
        if (!Locate(Horizontal, h, hi, ht)) { hi = 0; ht = 0.0f; }
        const size_t nv = Vertical.size();
        const size_t vj = std::min(vi + 1, nv - 1), hj = std::min(hi + 1, Horizontal.size() - 1);
        auto c = [&](size_t hh, size_t vv) { return Candela[hh * nv + vv]; };
        const float a = c(hi, vi) + (c(hi, vj) - c(hi, vi)) * vt;
        const float b = c(hj, vi) + (c(hj, vj) - c(hj, vi)) * vt;
        return a + (b - a) * ht;
    }

    std::vector<uint8_t> IesProfile::Bake(int w, int h) const {
        std::vector<uint8_t> px((size_t)w * h * 4, 0);
        const float peak = MaxCandela > 0.0f ? MaxCandela : 1.0f;
        for (int y = 0; y < h; y++) {
            const float v = ((float)y + 0.5f) / (float)h * 180.0f;
            for (int x = 0; x < w; x++) {
                const float hz = ((float)x + 0.5f) / (float)w * 360.0f;
                const float lin = std::clamp(Sample(v, hz) / peak, 0.0f, 1.0f);
                const float s = lin <= 0.0031308f ? lin * 12.92f : 1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f;
                const uint8_t g = (uint8_t)std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f);
                uint8_t* p = &px[((size_t)y * w + x) * 4];
                p[0] = p[1] = p[2] = g;
                p[3] = 255;
            }
        }
        return px;
    }

    bool ParseIesProfile(const std::string& text, IesProfile& out, std::string* error) {
        auto fail = [&](const char* why) { if (error) *error = why; return false; };
        // Header lines (the format line, [KEYWORD] lines) up to TILT=
        std::istringstream in(text);
        std::string line;
        std::string tilt;
        while (std::getline(in, line)) {
            const size_t at = line.find("TILT=");
            if (at != std::string::npos) { tilt = line.substr(at + 5); break; }
        }
        if (in.fail() && tilt.empty()) return fail("no TILT= line (not an IES file?)");
        while (!tilt.empty() && (tilt.back() == '\r' || tilt.back() == ' ')) tilt.pop_back();
        // The rest is numbers separated by spaces, commas or new lines
        std::string rest((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::replace(rest.begin(), rest.end(), ',', ' ');
        std::istringstream nums(rest);
        auto next = [&](float& v) { return (bool)(nums >> v); };
        float f;
        if (tilt == "INCLUDE") {        // lamp geometry, pairs, angles, factors: not used
            float pairs;
            if (!next(f) || !next(pairs) || pairs < 0.0f || pairs > 10000.0f) return fail("bad TILT data");
            for (int i = 0; i < (int)pairs * 2; i++) if (!next(f)) return fail("bad TILT data");
        }
        float lamps, lumens, mult, nv, nh, type, units, wid, len, hgt, ballast, future, watts;
        if (!next(lamps) || !next(lumens) || !next(mult) || !next(nv) || !next(nh) || !next(type) || !next(units) ||
            !next(wid) || !next(len) || !next(hgt) || !next(ballast) || !next(future) || !next(watts))
            return fail("the header numbers are cut short");
        if (nv < 1.0f || nh < 1.0f || nv > 10000.0f || nh > 10000.0f) return fail("bad angle counts");
        if ((int)type != 1) return fail("only type C photometry is supported");
        out = IesProfile{};
        out.Vertical.resize((size_t)nv);
        out.Horizontal.resize((size_t)nh);
        for (float& a : out.Vertical) if (!next(a)) return fail("the vertical angles are cut short");
        for (float& a : out.Horizontal) if (!next(a)) return fail("the horizontal angles are cut short");
        out.Candela.resize(out.Vertical.size() * out.Horizontal.size());
        for (float& c : out.Candela) {
            if (!next(c)) return fail("the candela values are cut short");
            c = std::max(c * mult * ballast, 0.0f);
            out.MaxCandela = std::max(out.MaxCandela, c);
        }
        if (!std::is_sorted(out.Vertical.begin(), out.Vertical.end()) || !std::is_sorted(out.Horizontal.begin(), out.Horizontal.end()))
            return fail("the angles don't rise");
        if (out.MaxCandela <= 0.0f) return fail("all candela values are 0");
        return true;
    }

    bool LoadIesProfile(const std::string& path, IesProfile& out, std::string* error) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { if (error) *error = "can't open the file"; return false; }
        std::stringstream ss;
        ss << f.rdbuf();
        return ParseIesProfile(ss.str(), out, error);
    }

} // namespace Engine
