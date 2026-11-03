#include "keys.h"
#include <X11/Xutil.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <strings.h>

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
    {"complete", 0, XK_Tab},
    {"copy", ControlMask | ShiftMask, XK_c},
    {"paste", ControlMask | ShiftMask, XK_v},
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
static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r");
    size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "Ctrl+Shift+L" -> mods = ControlMask | ShiftMask, sym = XK_l. False if not a valid key.
static bool parseKey(const std::string &text, unsigned &mods, KeySym &sym)
{
    std::vector<std::string> parts;
    std::stringstream ss(text);
    std::string part;
    while (std::getline(ss, part, '+'))
        parts.push_back(trim(part));
    if (parts.empty() || parts.back().empty())
        return false;

    mods = 0;
    for (size_t i = 0; i + 1 < parts.size(); i++) // everything before the last part is a modifier
    {
        if (strcasecmp(parts[i].c_str(), "Ctrl") == 0)
            mods |= ControlMask;
        else if (strcasecmp(parts[i].c_str(), "Shift") == 0)
            mods |= ShiftMask;
        else if (strcasecmp(parts[i].c_str(), "Alt") == 0)
            mods |= Mod1Mask;
        else
            return false;
    }

    KeySym ks = XStringToKeysym(parts.back().c_str()); // "L", "Up", "BackSpace", "F5"
    if (ks == NoSymbol)
        return false;
    KeySym upper;
    XConvertCase(ks, &sym, &upper); // store lowercase, the way findAction compares
    return true;
}

std::vector<std::string> loadKeyConfig()
{
    std::vector<std::string> errors;
    const char *home = getenv("HOME");
    std::ifstream file(std::string(home ? home : ".") + "/.terminatorrc");
    if (!file)
        return errors; // no config file: all defaults

    std::string line;
    int lineNo = 0;
    while (std::getline(file, line))
    {
        lineNo++;
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::string where = "~/.terminatorrc line " + std::to_string(lineNo) + ": ";

        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            errors.push_back(where + "expected 'action = key'");
            continue;
        }
        std::string name = trim(line.substr(0, eq));
        std::string keyText = trim(line.substr(eq + 1));

        int act = -1;
        for (int a = 0; a < ACT_COUNT; a++)
            if (bindings[a].name == name)
                act = a;
        if (act < 0)
        {
            errors.push_back(where + "unknown action '" + name + "'");
            continue;
        }

        unsigned mods;
        KeySym sym;
        if (!parseKey(keyText, mods, sym))
        {
            errors.push_back(where + "invalid key '" + keyText + "', " + name + " keeps its default");
            continue;
        }

        // a printable key without Ctrl/Alt would steal normal typing
        if (!(mods & (ControlMask | Mod1Mask)) && sym >= XK_space && sym <= XK_asciitilde)
        {
            errors.push_back(where + "'" + keyText + "' needs Ctrl or Alt, " + name + " keeps its default");
            continue;
        }

        int other = -1;
        for (int a = 0; a < ACT_COUNT; a++)
            if (a != act && bindings[a].mods == mods && bindings[a].sym == sym)
                other = a;
        if (other >= 0)
        {
            errors.push_back(where + keyText + " is already used by " + bindings[other].name +
                             ", " + name + " keeps its default");
            continue;
        }

        bindings[act].mods = mods; // valid: replace the default
        bindings[act].sym = sym;
    }
    return errors;
}
