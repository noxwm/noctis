#include "config/config.hpp"
#include "config/parser.hpp"

extern "C" {
#include <wlr/types/wlr_keyboard.h>  // WLR_MODIFIER_*
}

#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdlib>

// ─── Modifier string → bitmask ────────────────────────────────────────────

static uint32_t parse_modifiers(const std::string &combo) {
    uint32_t mods = 0;
    if (combo.find("Super") != std::string::npos) mods |= WLR_MODIFIER_LOGO;
    if (combo.find("Alt")   != std::string::npos) mods |= WLR_MODIFIER_ALT;
    if (combo.find("Ctrl")  != std::string::npos) mods |= WLR_MODIFIER_CTRL;
    if (combo.find("Shift") != std::string::npos) mods |= WLR_MODIFIER_SHIFT;
    return mods;
}

// Extract the key name from "Super+Shift+Return" → "Return"
static std::string parse_key(const std::string &combo) {
    auto pos = combo.rfind('+');
    if (pos == std::string::npos) return combo;
    return combo.substr(pos + 1);
}

// ─── NoxConfig ────────────────────────────────────────────────────────────

std::string NoxConfig::default_path() {
    const char *home = getenv("HOME");
    if (!home) home = "/root";
    return std::string(home) + "/.config/noxwm/config.nox";
}

bool NoxConfig::load(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[noxwm] config not found at " << path
                  << " — using defaults\n";
        return false;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    NoxLexer  lexer(source);
    NoxParser parser(lexer.tokenize());
    BlockMap  blocks = parser.parse();

    // ── [general] ──────────────────────────────────────────────────────
    if (blocks.count("general")) {
        auto &b = blocks["general"][0];
        if (b.count("gaps"))                gaps                 = std::stoi(b["gaps"]);
        if (b.count("border_width"))        border_width         = std::stoi(b["border_width"]);
        if (b.count("border_color"))        border_color         = b["border_color"];
        if (b.count("border_color_inactive")) border_color_inactive = b["border_color_inactive"];
    }

    // ── [keybinds] ─────────────────────────────────────────────────────
    if (blocks.count("keybinds")) {
        for (auto &b : blocks["keybinds"]) {
            for (auto &[combo, action] : b) {
                Keybind kb;
                kb.modifiers = parse_modifiers(combo);
                kb.key       = parse_key(combo);
                kb.action    = action;
                keybinds.push_back(kb);
            }
        }
    }

    // ── [autostart] ────────────────────────────────────────────────────
    if (blocks.count("autostart")) {
        for (auto &b : blocks["autostart"]) {
            for (auto &[k, v] : b) {
                if (k == "exec") autostart.push_back(v);
            }
        }
    }

    return true;
}
