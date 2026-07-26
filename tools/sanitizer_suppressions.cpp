// Baked-in LeakSanitizer suppressions for leaks we do not own and cannot fix.
//
// On Wayland, SDL draws window decorations through libdecor's GTK plugin, which
// pulls in GTK, pango, and fontconfig. Those libraries hold font and config
// caches that are never freed at exit, producing thousands of "leaks" that have
// nothing to do with our code. The graphics driver keeps similar process-global
// state. Suppressing these by module keeps ASan strict about everything we write
// while ignoring third-party noise.
//
// Compiled only into the windowed executable. LeakSanitizer calls this weak hook
// at startup; in non-sanitised builds it is simply unused.

#include <dlfcn.h>

// The Vulkan loader dlcloses the driver before LeakSanitizer runs, leaving its
// leak frames as unknown modules that no suppression can match. Pinning the
// driver resident keeps the frames attributable so the suppressions work. A
// missing library is harmless.
__attribute__((constructor)) static void pin_driver_modules() {
    (void)dlopen("libvulkan_intel.so", RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE);
}

extern "C" const char* __lsan_default_suppressions(void);

extern "C" const char* __lsan_default_suppressions(void) {
    return "leak:libdecor\n"
           "leak:libgtk-3\n"
           "leak:libgdk-3\n"
           "leak:libglib-2.0\n"
           "leak:libgobject-2.0\n"
           "leak:libgio-2.0\n"
           "leak:libpango\n"
           "leak:libcairo\n"
           "leak:libfontconfig\n"
           "leak:libharfbuzz\n"
           "leak:libEGL\n"
           "leak:libGLX\n"
           "leak:libgallium\n"
           "leak:libLLVM\n"
           "leak:swrast\n"
           "leak:iris_dri\n"
           "leak:libvulkan_intel\n"
           "leak:libvulkan.so\n"
           // The pulseaudio client library parks per-process context state in
           // its own threads and never frees it.
           "leak:libpulse\n";
}
