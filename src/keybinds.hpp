#pragma once

#include <xkbcommon/xkbcommon.h>
#include <functional>
#include <vector>

struct Server;

struct Keybind {
    uint32_t     modifiers; // xkb modifier mask
    xkb_keysym_t sym;
    std::function<void(Server *)> action;
};

class KeybindManager {
public:
    void add(uint32_t mods, xkb_keysym_t sym, std::function<void(Server *)> fn);

    // Returns true if the key was consumed
    bool handle_key(Server *server, uint32_t modifiers, xkb_keysym_t sym);

    void setup_defaults(Server *server);

private:
    std::vector<Keybind> binds_;
};
