#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *display_name = getenv("DISPLAY");
  if (!display_name || !display_name[0])
    display_name = ":0";

  int attempts = 50;
  if (argc > 1) {
    int parsed = atoi(argv[1]);
    if (parsed > 0)
      attempts = parsed;
  }

  Display *dpy = NULL;
  for (int i = 0; i < attempts; i++) {
    dpy = XOpenDisplay(display_name);
    if (dpy)
      break;
    usleep(100000);
  }

  if (!dpy) {
    fprintf(stderr, "xrootcursor: could not open display %s\n", display_name);
    return 1;
  }

  Cursor cursor = XCreateFontCursor(dpy, XC_left_ptr);
  if (cursor == None) {
    fprintf(stderr, "xrootcursor: could not create XC_left_ptr cursor\n");
    XCloseDisplay(dpy);
    return 1;
  }

  Window root = DefaultRootWindow(dpy);
  XDefineCursor(dpy, root, cursor);
  XFlush(dpy);

  for (;;)
    sleep(3600);

  XCloseDisplay(dpy);
  return 0;
}
