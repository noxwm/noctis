#include "config.hpp"
#include "util.hpp"

#include <toml++/toml.hpp>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

extern "C" {
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_keyboard.h>
}

Config g_config;

// ── Defaults ──────────────────────────────────────────────────────────────────

void Config::load_defaults() {
    terminal     = "kitty";
    gap          = 6;
    master_ratio = 0.55f;
    border_width = 2;

    bg_color[0]  = 0.10f; bg_color[1]  = 0.10f;
    bg_color[2]  = 0.15f; bg_color[3]  = 1.00f;

    active_border[0]   = 0.67f; active_border[1]   = 0.42f;
    active_border[2]   = 0.42f; active_border[3]   = 1.00f;

    inactive_border[0] = 0.20f; inactive_border[1] = 0.20f;
    inactive_border[2] = 0.20f; inactive_border[3] = 1.00f;

    autostart.clear();
    keybinds.clear();

    // Built-in default keybinds (used when no [keybinds] section present)
    keybinds.push_back(parse_keybind("Super+Return",  "exec:kitty"));
    keybinds.push_back(parse_keybind("Super+Q",       "close"));
    keybinds.push_back(parse_keybind("Super+J",       "focus_next"));
    keybinds.push_back(parse_keybind("Super+K",       "focus_prev"));
    keybinds.push_back(parse_keybind("Super+Shift+Q", "exit"));
}

// ── Hex color parser ──────────────────────────────────────────────────────────
// Accepts "#RRGGBB" or "#RRGGBBAA"

void Config::parse_hex_color(const std::string &hex,
                              float out[4],
                              const std::string &key_name) {
    if (hex.empty() || hex[0] != '#') {
        fprintf(stderr,
            "[noctis] ERROR: config: '%s' must start with '#' (got \"%s\")\n",
            key_name.c_str(), hex.c_str());
        exit(1);
    }

    std::string h = hex.substr(1);

    if (h.size() != 6 && h.size() != 8) {
        fprintf(stderr,
            "[noctis] ERROR: config: '%s' must be #RRGGBB or #RRGGBBAA (got \"%s\")\n",
            key_name.c_str(), hex.c_str());
        exit(1);
    }

    // Validate all hex digits
    for (char c : h) {
        if (!isxdigit(c)) {
            fprintf(stderr,
                "[noctis] ERROR: config: '%s' contains invalid hex character '%c' (got \"%s\")\n",
                key_name.c_str(), c, hex.c_str());
            exit(1);
        }
    }

    unsigned int r = 0, g = 0, b = 0, a = 255;
    if (h.size() == 6) {
        sscanf(h.c_str(), "%02x%02x%02x", &r, &g, &b);
    } else {
        sscanf(h.c_str(), "%02x%02x%02x%02x", &r, &g, &b, &a);
    }

    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

// ── Keybind parser ────────────────────────────────────────────────────────────
// Parses "Super+Shift+Return" → {modifiers, sym}

KeybindEntry Config::parse_keybind(const std::string &combo,
                                    const std::string &action) {
    KeybindEntry entry{};
    entry.action = action;
    entry.modifiers = 0;
    entry.sym = XKB_KEY_NoSymbol;

    // Split by '+'
    std::vector<std::string> tokens;
    std::stringstream ss(combo);
    std::string token;
    while (std::getline(ss, token, '+')) {
        if (!token.empty()) tokens.push_back(token);
    }

    if (tokens.empty()) {
        fprintf(stderr,
            "[noctis] ERROR: config: empty keybind combo \"%s\"\n",
            combo.c_str());
        exit(1);
    }

    // All tokens except the last are modifiers; last is the key
    for (size_t i = 0; i < tokens.size() - 1; ++i) {
        const std::string &mod = tokens[i];
        if (mod == "Super" || mod == "Logo") {
            entry.modifiers |= WLR_MODIFIER_LOGO;
        } else if (mod == "Shift") {
            entry.modifiers |= WLR_MODIFIER_SHIFT;
        } else if (mod == "Ctrl" || mod == "Control") {
            entry.modifiers |= WLR_MODIFIER_CTRL;
        } else if (mod == "Alt") {
            entry.modifiers |= WLR_MODIFIER_ALT;
        } else {
            fprintf(stderr,
                "[noctis] ERROR: config: unknown modifier \"%s\" in keybind \"%s\"\n",
                mod.c_str(), combo.c_str());
            exit(1);
        }
    }

    // Last token is the key name
    const std::string &keyname = tokens.back();
    xkb_keysym_t sym = xkb_keysym_from_name(keyname.c_str(),
                                              XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol) {
        fprintf(stderr,
            "[noctis] ERROR: config: unknown key \"%s\" in keybind \"%s\"\n",
            keyname.c_str(), combo.c_str());
        exit(1);
    }

    entry.sym = sym;
    return entry;
}

// ── Config::load ──────────────────────────────────────────────────────────────

void Config::load() {
    load_defaults();

    // Resolve config path: $XDG_CONFIG_HOME/noctis/config.toml
    // or ~/.config/noctis/config.toml
    std::string config_path;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && strlen(xdg) > 0) {
        config_path = std::string(xdg) + "/noctis/config.toml";
    } else {
        const char *home = getenv("HOME");
        if (!home) {
            LOG_WARN("HOME not set, using built-in defaults");
            return;
        }
        config_path = std::string(home) + "/.config/noctis/config.toml";
    }

    if (!std::filesystem::exists(config_path)) {
        LOG_INFO("No config file found at %s — using defaults", config_path.c_str());
        return;
    }

    LOG_INFO("Loading config: %s", config_path.c_str());

    toml::table tbl;
    try {
        tbl = toml::parse_file(config_path);
    } catch (const toml::parse_error &e) {
        fprintf(stderr,
            "[noctis] ERROR: config parse error in %s:\n  %s (line %d)\n",
            config_path.c_str(),
            std::string(e.description()).c_str(),
            (int)e.source().begin.line);
        exit(1);
    }

    // ── [general] ─────────────────────────────────────────────────────────────

    if (auto general = tbl["general"].as_table()) {

        if (auto v = (*general)["terminal"].value<std::string>()) {
            terminal = *v;
        }

        if (auto v = (*general)["gap"].value<int64_t>()) {
            if (*v < 0 || *v > 200) {
                fprintf(stderr,
                    "[noctis] ERROR: config: general.gap must be between 0 and 200 (got %lld)\n",
                    (long long)*v);
                exit(1);
            }
            gap = (int)*v;
        }

        if (auto v = (*general)["master_ratio"].value<double>()) {
            if (*v <= 0.0 || *v >= 1.0) {
                fprintf(stderr,
                    "[noctis] ERROR: config: general.master_ratio must be between 0.0 and 1.0 (got %.3f)\n",
                    *v);
                exit(1);
            }
            master_ratio = (float)*v;
        }

        if (auto v = (*general)["border_width"].value<int64_t>()) {
            if (*v < 0 || *v > 50) {
                fprintf(stderr,
                    "[noctis] ERROR: config: general.border_width must be between 0 and 50 (got %lld)\n",
                    (long long)*v);
                exit(1);
            }
            border_width = (int)*v;
        }
    }

    // ── [colors] ──────────────────────────────────────────────────────────────

    if (auto colors = tbl["colors"].as_table()) {

        if (auto v = (*colors)["background"].value<std::string>()) {
            parse_hex_color(*v, bg_color, "colors.background");
        }

        if (auto v = (*colors)["active_border"].value<std::string>()) {
            parse_hex_color(*v, active_border, "colors.active_border");
        }

        if (auto v = (*colors)["inactive_border"].value<std::string>()) {
            parse_hex_color(*v, inactive_border, "colors.inactive_border");
        }
    }

    // ── [autostart] ───────────────────────────────────────────────────────────

    autostart.clear();
    if (auto as = tbl["autostart"].as_table()) {
        if (auto apps = (*as)["apps"].as_array()) {
            for (auto &elem : *apps) {
                if (auto s = elem.value<std::string>()) {
                    autostart.push_back({*s});
                } else {
                    fprintf(stderr,
                        "[noctis] ERROR: config: autostart.apps must be an array of strings\n");
                    exit(1);
                }
            }
        }
    }

    // ── [keybinds] ────────────────────────────────────────────────────────────
    // If section exists, replace all defaults entirely.

    if (auto kb = tbl["keybinds"].as_table()) {
        keybinds.clear();
        for (auto &[combo, val] : *kb) {
            auto action = val.value<std::string>();
            if (!action) {
                fprintf(stderr,
                    "[noctis] ERROR: config: keybind value for \"%s\" must be a string\n",
                    std::string(combo).c_str());
                exit(1);
            }
            keybinds.push_back(
                parse_keybind(std::string(combo), *action));
        }
    }

    LOG_INFO("Config loaded — terminal=%s gap=%d master_ratio=%.2f keybinds=%zu autostart=%zu",
        terminal.c_str(), gap, master_ratio,
        keybinds.size(), autostart.size());
}
