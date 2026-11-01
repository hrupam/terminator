#include "events.h"
#include "gui.h"
#include "tabs.h"
#include <X11/Xutil.h>
#include <string>
#include <unistd.h>
#include <csignal>
#include "keys.h"

using namespace std;

static void handleAction(Action action)
{
    switch (action)
    {
    case ACT_NEW_TAB:
        newTab(getCurrTab().cwd);
        break;

    case ACT_NEXT_TAB:
        currentTab = (currentTab + 1) % tabs.size();
        break;

    case ACT_PREV_TAB:
        currentTab = (currentTab == 0) ? tabs.size() - 1 : currentTab - 1;
        break;

    case ACT_CLOSE_TAB:
        closeTab(currentTab);
        if (!appRunning)
            return; // last tab closed: nothing left to draw
        break;

    case ACT_INTERRUPT: // Ctrl+C
    {
        TerminalState &ts = getCurrTab();
        if (ts.busy)
        {
            if (ts.pgid > 0)
                kill(-ts.pgid, SIGINT);
            ts.partial += "^C";
            ts.appendOutput("\n");
        }
        else
        {
            string p = ts.more ? "> " : ts.prompt;
            ts.lines.push_back(p + ts.input + "^C");
            ts.input.clear();
            ts.cursor = 0;
            ts.more = false;
            sendToEngine(ts, "K");
        }
        ts.scroll = 0;
        break;
    }

    case ACT_SUSPEND: // Ctrl+Z
    {
        TerminalState &ts = getCurrTab();
        if (ts.busy && ts.pgid > 0)
        {
            kill(-ts.pgid, SIGTSTP);
            ts.partial += "^Z";
            ts.appendOutput("\n");
        }
        ts.scroll = 0;
        break;
    }

    // ---- line editing ----
    case ACT_LINE_START: // Ctrl+A
        getCurrTab().cursor = 0;
        break;

    case ACT_LINE_END: // Ctrl+E
        getCurrTab().cursor = getCurrTab().input.size();
        break;

    case ACT_KILL_START: // Ctrl+U: delete from the start up to the cursor
    {
        TerminalState &ts = getCurrTab();
        ts.input.erase(0, ts.cursor);
        ts.cursor = 0;
        break;
    }

    case ACT_KILL_END: // Ctrl+K: delete from the cursor to the end
    {
        TerminalState &ts = getCurrTab();
        ts.input.erase(ts.cursor);
        break;
    }

    case ACT_DEL_WORD: // Ctrl+W: skip spaces backwards, then the word, delete that range
    {
        TerminalState &ts = getCurrTab();
        int start = ts.cursor;
        while (start > 0 && ts.input[start - 1] == ' ')
            start--;
        while (start > 0 && ts.input[start - 1] != ' ')
            start--;
        ts.input.erase(start, ts.cursor - start);
        ts.cursor = start;
        break;
    }

    case ACT_CLEAR: // Ctrl+L: clear the screen, keep the line being typed
    {
        TerminalState &ts = getCurrTab();
        ts.lines.clear();
        ts.scroll = 0;
        break;
    }

    case ACT_HIST_PREV: // Up: one command older
    {
        if (history.empty())
            break;
        TerminalState &ts = getCurrTab();
        if (ts.histPos == -1) // start browsing: remember what was being typed
        {
            ts.histDraft = ts.input;
            ts.histPos = history.size(); // one past the newest; the line below moves onto it
        }
        if (ts.histPos > 0)
            ts.histPos--;               // move first...
        ts.input = history[ts.histPos]; // ...then show: histPos = what is on screen
        ts.cursor = ts.input.size();
        break;
    }

    case ACT_HIST_NEXT:
    {
        TerminalState &ts = getCurrTab();
        if (ts.histPos == -1)
            break; // not browsing
        ts.histPos++;
        if (ts.histPos >= (int)history.size())
        {
            ts.input = ts.histDraft;
            ts.histPos = -1;
        }
        else
            ts.input = history[ts.histPos];
        ts.cursor = ts.input.size();
        break;
    }

    default:
        break;
    }
    redraw();
}

void handleEvent(XEvent &ev)
{

    if (ev.type == Expose && ev.xexpose.count == 0)
    {
        redraw();
        return;
    }

    if (ev.type == KeyPress)
    {
        char buf[128] = {0};
        KeySym keysym;
        int len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &keysym, NULL);

        Action act = findAction(ev.xkey);
        if (act != ACT_NONE)
        {
            handleAction(act);
            return;
        }

        TerminalState &termState = getCurrTab();
        switch (keysym)
        {
        case XK_Return:
        {
            if (termState.busy)
                break;
            if (!termState.input.empty() &&
                (history.empty() || history.back() != termState.input))
                history.push_back(termState.input);
            termState.histPos = -1;

            termState.lines.push_back((termState.more ? "> " : termState.prompt) + termState.input);
            sendToEngine(termState, "L " + termState.input); // the engine runs it
            termState.busy = true;                           // until the engine answers R or M
            termState.input.clear();
            termState.cursor = 0;
            termState.scroll = 0;
            break;
        }

        case XK_BackSpace:
            if (termState.input.length() > 0 && termState.cursor > 0)
            {
                termState.input.erase(termState.cursor - 1, 1);
                termState.cursor--;
            }
            break;

        case XK_Left:
            if (termState.cursor > 0)
            {
                termState.cursor--;
            }
            break;

        case XK_Right:
            if (termState.cursor < (int)termState.input.length())
            {
                termState.cursor++;
            }
            break;

        case XK_Prior: // Page Up
            if (ev.xkey.state & ShiftMask)
                termState.scroll += 10;
            break;
        case XK_Next: // Page Down
            if (ev.xkey.state & ShiftMask)
                termState.scroll -= 10;
            break;

        default:
            if (len == 1 && (unsigned char)buf[0] >= 32 &&
                !(ev.xkey.state & (ControlMask | Mod1Mask)))
            {
                termState.scroll = 0;
                termState.appendInput(buf);
            }
        }
        redraw();
        return;
    }
    if (ev.type == ButtonPress && ev.xbutton.button == 1) // left click
    {
        int lineHeight = font->ascent + font->descent;
        if (ev.xbutton.y < TABHEIGHT * lineHeight) // inside the tab bar
        {
            int cx = ev.xbutton.x;

            // tab's x button click
            for (int i = 0; i < (int)tabCloseBounds.size(); i++)
            {
                if (cx >= tabCloseBounds[i].first && cx <= tabCloseBounds[i].second)
                {
                    closeTab(i);
                    if (appRunning)
                        redraw();
                    return;
                }
            }

            int tabi = -1;
            for (int i = 0; i < (int)tabBounds.size(); i++)
            {
                if (cx >= tabBounds[i].first && cx <= tabBounds[i].second)
                {
                    tabi = i;
                    break;
                }
            }
            if (tabi != -1 && currentTab != tabi)
            {
                currentTab = tabi;
                redraw();
                return;
            }

            if (cx >= plusBounds.first && cx <= plusBounds.second)
            {
                newTab(getCurrTab().cwd);
                redraw();
            }
        }
        return;
    }
    TerminalState &termState = getCurrTab();
    if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wmDeleteMessage)
    {
        appRunning = false;
        return;
    }
    if (ev.type == ConfigureNotify)
    {
        width = ev.xconfigure.width;
        height = ev.xconfigure.height;
        redraw();
        return;
    }
    if (ev.type == ButtonPress)
    {
        if (ev.xbutton.button == 4)
            termState.scroll += 3; // wheel up
        if (ev.xbutton.button == 5)
            termState.scroll -= 3; // wheel down
        redraw();
        return;
    }
}
