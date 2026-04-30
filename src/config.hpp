#pragma once

#include <string>
#include <vector>
#include <xkbcommon/xkbcommon.h>

struct AutostartEntry {
    std::string cmd;
};

struct KeybindEntry {
    uint32_t     modifiers;
    xkb_keysym_t sym;
    std::string  action; // "exec:cmd", "close", "focus_next", "focus_prev", "exit"
};

struct Config {
    // [general]
    std::string terminal     = "kitty";
    int         gap          = 6;
    float       master_ratio = 0.55f;
    int         border_width = 2;

    // [colors] RGBA float[4]
    float bg_color[4]        = {0.10f, 0.10f, 0.15f, 1.0f};
    float active_border[4]   = {0.67f, 0.42f, 0.42f, 1.0f};
    float inactive_border[4] = {0.20f, 0.20f, 0.20f, 1.0f};

    // [autostart]
    std::vector<AutostartEntry> autostart;

    // [keybinds]
    std::vector<KeybindEntry> keybinds;

    // Load defaults then overlay with ~/.config/noctis/config.toml
    // Prints error + exits on bad values.
    // Silent fallback to defaults if file missing.
    void load();
    void load_defaults();

private:
    static void parse_hex_color(const std::string &hex,
                                float out[4],
                                const std::string &key_name);

    static KeybindEntry parse_keybind(const std::string &combo,
                                      const std::string &action);
};

extern Config g_config;
