#pragma once
#include <X11/Xlib.h>
#include <utility>
#include <vector>

#define TABHEIGHT 2 // rows reserved at the top for the tab bar

extern Display *disp;
extern Window win;
extern GC gc;
extern XFontStruct *font;
extern unsigned int width, height;
extern unsigned long bgColor, fgColor, accentColor, dimColor;
extern Atom wmDeleteMessage;

extern std::vector<std::pair<int, int>> tabBounds;      // x start / x end of each tab label, filled in redraw
extern std::vector<std::pair<int, int>> tabCloseBounds; // x start / x end of each tab's [x]
extern std::pair<int, int> plusBounds;                  // the [ + ] button

bool initGui();       // open display, window, colours, font, GC; false on failure
void redraw();        // draw tab bar + current tab
void closeTerminal(); // free X resources
