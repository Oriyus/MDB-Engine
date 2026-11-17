#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

    // IES light profiles (IESNA LM-63, type C photometry: the files lamp makers publish). A profile is how bright a
    // light is in each direction: candela over vertical angles (0 = straight along the light's axis, 180 = straight
    // back) and horizontal angles round it. Symmetric files (one horizontal angle, 0..90, 0..180) are unfolded.
    struct IesProfile {
        std::vector<float> Vertical, Horizontal;     // degrees, rising
        std::vector<float> Candela;                  // Horizontal.size() rows of Vertical.size() values
        float MaxCandela = 0.0f;

        // Candela towards (vertical, horizontal) degrees, linearly interpolated (0 outside the vertical range)
        float Sample(float vertical, float horizontal) const;
        // As an image for the shaders (RGBA8, the brightness relative to the peak, sRGB-encoded so an sRGB texture
        // reads it back as linear): u = horizontal angle 0..360, v = vertical angle 0..180 (bottom row = the axis)
        std::vector<uint8_t> Bake(int width, int height) const;
    };

    // Parse a file's text. False with a message on malformed or unsupported data (TILT from another file is ignored)
    bool ParseIesProfile(const std::string& text, IesProfile& out, std::string* error = nullptr);
    bool LoadIesProfile(const std::string& path, IesProfile& out, std::string* error = nullptr);

} // namespace Engine
