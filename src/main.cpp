#include "core/server.hpp"

extern "C" {
#include <wlr/util/log.h>
}

#include <cstdlib>
#include <iostream>

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, nullptr);

    NoxServer server;

    if (!server.init()) {
        std::cerr << "[noxwm] failed to initialise compositor\n";
        return EXIT_FAILURE;
    }

    std::cout << "[noxwm] starting — socket: "
              << getenv("WAYLAND_DISPLAY") << "\n";

    server.run();

    return EXIT_SUCCESS;
}
