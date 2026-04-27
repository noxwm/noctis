#pragma once

#include <string>
#include <xkbcommon/xkbcommon.h>

struct Config {
    // Modifier key (default: Super/Logo)
    uint32_t mod_key = 0; // filled at runtime from xkb

    // Terminal to launch
    std::string terminal = "alacritty";

    // Border width (px)
    int border_width = 2;

    // Gap between windows (px)
    int gap = 6;

    // Background color RGBA
    float bg_color[4] = {0.1f, 0.1f, 0.15f, 1.0f};

    // Active border color RGBA
    float active_border[4] = {0.3f, 0.6f, 1.0f, 1.0f};

    // Inactive border color RGBA
    float inactive_border[4] = {0.3f, 0.3f, 0.3f, 1.0f};

    // Master ratio for tiling
    float master_ratio = 0.55f;

    void load_defaults();
};

extern Config g_config;
