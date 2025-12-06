#include "Application.hpp"
#include <glib.h>

int main(int argc, char* argv[]) {
    // Set program name and application name for proper desktop integration
    // This is crucial for GNOME Shell to match the window to its desktop file on Wayland
    g_set_prgname("com.github.storage-wiper");
    g_set_application_name("Storage Wiper");

    StorageWiperApp app;
    return app.run(argc, argv);
}