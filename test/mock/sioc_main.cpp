#include "sioc.h"

#include <csignal>
#include <iostream>
#include <thread>

static volatile sig_atomic_t g_running = 1;

void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        g_running = 0;
    }
}

int main(int argc, char* argv[])
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "[mock-sioc] Starting PV server..." << std::endl;

    PVServer server;

    std::cout << "[mock-sioc] Server is running. Press Ctrl+C to stop." << std::endl;

    while (g_running)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "[mock-sioc] Shutting down..." << std::endl;
    return 0;
}