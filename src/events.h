#pragma once
#include <X11/Xlib.h>

// Handles one X event: keys, mouse, resize, expose, window close.
void handleEvent(XEvent &ev);
