/**
 * @file main.cpp
 * @brief Entry point for storage-wiper-cli
 */

#include "cli/CliApplication.hpp"

int main(int argc, char* argv[]) {
    cli::CliApplication app;
    return app.run(argc, argv);
}
