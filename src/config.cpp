#include "config.hpp"
#include <xkbcommon/xkbcommon.h>

Config g_config;

void Config::load_defaults() {
    terminal     = "kitty";
    border_width = 2;
    gap          = 6;
    bg_color[0]  = 0.10f; bg_color[1]  = 0.10f;
    bg_color[2]  = 0.15f; bg_color[3]  = 1.00f;
    active_border[0]   = 0.3f; active_border[1]   = 0.6f;
    active_border[2]   = 1.0f; active_border[3]   = 1.0f;
    inactive_border[0] = 0.3f; inactive_border[1] = 0.3f;
    inactive_border[2] = 0.3f; inactive_border[3] = 1.0f;
    master_ratio = 0.55f;
}
