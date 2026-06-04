#include <stdio.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xinerama.h>

#define FAIL(...)                     \
    do {                              \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr);          \
        exit(1);                      \
    } while (0)

#define CHECK(cond, ...)       \
    do {                       \
        if (!(cond))           \
            FAIL(__VA_ARGS__); \
    } while (0)

int main(void)
{
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "dummy", 1);

    Display *display = XOpenDisplay(NULL);
    CHECK(display != NULL, "XOpenDisplay failed");

    int event_base = -1;
    int error_base = -1;
    CHECK(XineramaQueryExtension(display, &event_base, &error_base),
          "XineramaQueryExtension failed");
    CHECK(event_base == 0 && error_base == 0,
          "unexpected Xinerama event/error bases: %d/%d", event_base,
          error_base);

    int major = 0;
    int minor = 0;
    CHECK(XineramaQueryVersion(display, &major, &minor),
          "XineramaQueryVersion failed");
    CHECK(major == 1 && minor == 1, "unexpected Xinerama version: %d.%d", major,
          minor);
    CHECK(XineramaIsActive(display), "XineramaIsActive returned false");

    int screen_count = 0;
    XineramaScreenInfo *screens = XineramaQueryScreens(display, &screen_count);
    CHECK(screens != NULL, "XineramaQueryScreens returned NULL");
    CHECK(screen_count == ScreenCount(display),
          "unexpected Xinerama screen count: %d vs %d", screen_count,
          ScreenCount(display));
    for (int i = 0; i < screen_count; i++) {
        CHECK(screens[i].screen_number == i,
              "unexpected Xinerama screen number: %d",
              screens[i].screen_number);
        CHECK(screens[i].x_org == 0 && screens[i].y_org == 0,
              "unexpected Xinerama origin: %d,%d", screens[i].x_org,
              screens[i].y_org);
        CHECK(screens[i].width == DisplayWidth(display, i) &&
                  screens[i].height == DisplayHeight(display, i),
              "unexpected Xinerama geometry for screen %d: %dx%d", i,
              screens[i].width, screens[i].height);
    }
    free(screens);

    XCloseDisplay(display);
    puts("test_xinerama_link: ok");
    return 0;
}
