#include "keys.h"
#include <X11/Xutil.h>

// Default bindings, in the same order as enum Action.
Binding bindings[ACT_COUNT] = {
    {"beginning-of-line", ControlMask, XK_a},
    {"end-of-line", ControlMask, XK_e},
    {"kill-to-start", ControlMask, XK_u},
    {"kill-to-end", ControlMask, XK_k},
    {"delete-word", ControlMask, XK_w},
    {"clear-screen", ControlMask, XK_l},
    {"close-tab", ControlMask, XK_d},
    {"interrupt", ControlMask, XK_c},
    {"suspend", ControlMask, XK_z},
    {"new-tab", ControlMask | ShiftMask, XK_t},
    {"next-tab", ControlMask, XK_Right},
    {"prev-tab", ControlMask, XK_Left},
    {"history-prev", 0, XK_Up},
    {"history-next", 0, XK_Down},
};

Action findAction(XKeyEvent &e)
{
    // only these modifiers matter; ignore NumLock / CapsLock bits
    unsigned mods = e.state & (ControlMask | ShiftMask | Mod1Mask);

    // the key's unshifted symbol, lowercased: with Shift held X would report 'T', not 't'
    KeySym lower, upper;
    XConvertCase(XLookupKeysym(&e, 0), &lower, &upper);

    for (int a = 0; a < ACT_COUNT; a++)
        if (bindings[a].sym == lower && bindings[a].mods == mods)
            return (Action)a;
    return ACT_NONE;
}