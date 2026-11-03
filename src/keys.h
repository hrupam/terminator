#pragma once
#include <X11/Xlib.h>
#include <string>
#include <vector>

// Everything a hotkey can do. The order must match the bindings[] table in keys.cpp.
enum Action
{
    ACT_NONE = -1,
    ACT_LINE_START, // Ctrl+A
    ACT_LINE_END,   // Ctrl+E
    ACT_KILL_START, // Ctrl+U
    ACT_KILL_END,   // Ctrl+K
    ACT_DEL_WORD,   // Ctrl+W
    ACT_CLEAR,      // Ctrl+L
    ACT_CLOSE_TAB,  // Ctrl+D
    ACT_INTERRUPT,  // Ctrl+C
    ACT_SUSPEND,    // Ctrl+Z
    ACT_NEW_TAB,    // Ctrl+Shift+T
    ACT_NEXT_TAB,   // Ctrl+Right
    ACT_PREV_TAB,   // Ctrl+Left
    ACT_HIST_PREV,  // UP
    ACT_HIST_NEXT,  // DOWN
    ACT_COMPLETE,   // Tab
    ACT_COPY,       // Ctrl+Shift+C
    ACT_PASTE,      // Ctrl+Shift+V
    ACT_COUNT
};

struct Binding
{
    std::string name; // used in ~/.terminatorrc (step 3), e.g. "beginning-of-line"
    unsigned mods;    // ControlMask, ShiftMask, Mod1Mask (Alt), combined with |
    KeySym sym;       // always the lowercase key, e.g. XK_a
};

extern Binding bindings[ACT_COUNT];

// Which action does this key press trigger? ACT_NONE if it is not a hotkey.
Action findAction(XKeyEvent &e);

// Reads ~/.terminatorrc ("action = key" per line) and changes bindings[] accordingly.
// Returns one message per invalid or conflicting line; those actions keep their defaults.
std::vector<std::string> loadKeyConfig();