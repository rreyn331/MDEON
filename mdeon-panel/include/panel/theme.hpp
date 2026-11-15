#pragma once

struct RgbColor {
    double red;
    double green;
    double blue;
};

namespace PanelTheme {
inline constexpr RgbColor background{238.0 / 255.0, 238.0 / 255.0, 238.0 / 255.0};
inline constexpr RgbColor border{187.0 / 255.0, 187.0 / 255.0, 187.0 / 255.0};
inline constexpr RgbColor text{34.0 / 255.0, 34.0 / 255.0, 34.0 / 255.0};
}