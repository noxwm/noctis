#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// A single keybind: modifier mask + key name → action string
struct Keybind {
    uint32_t    modifiers;   // WLR_MODIFIER_* flags
    std::string key;         // e.g. "Return", "q", "1"
    std::string action;      // e.g. "exec kitty", "close", "workspace 1"
};

// Parsed config values
struct NoxConfig {
    // [general]
    int         gaps         = 8;
    int         border_width = 2;
    std::string border_color = "#cba6f7";   // focused border
    std::string border_color_inactive = "#313244";

    // [keybinds]
    std::vector<Keybind> keybinds;

    // [autostart]
    std::vector<std::string> autostart;

    // Load from file — returns false on parse error
    bool load(const std::string &path);

    // Default config path
    static std::string default_path();
};
