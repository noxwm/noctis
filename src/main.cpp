#include "server.hpp"
#include "util.hpp"

int main(int /*argc*/, char * /*argv*/[]) {
    Server server;

    if (!server.init()) {
        LOG_ERROR("Server initialization failed");
        return 1;
    }

    server.run();
    server.destroy();

    return 0;
}
