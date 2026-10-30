#include "events.h"
#include "gui.h"
#include "tabs.h"
#include "executor.h"
#include <X11/Xutil.h>
#include <string>
#include <unistd.h>
#include <csignal>

using namespace std;

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

        unsigned mods = ev.xkey.state & (ControlMask | ShiftMask);
        bool ctrl = mods & ControlMask;
        bool ctrlShift = mods == (ControlMask | ShiftMask);

        // Ctrl+Shift+T: create tab
        if (ctrlShift && (keysym == XK_T || keysym == XK_t))
        {
            newTab();
            redraw();
            return;
        }
        // Ctrl+Right: next tab
        if (ctrl && keysym == XK_Right)
        {
            currentTab = (currentTab + 1) % tabs.size();
            redraw();
            return;
        }
        // Ctrl+Left: previous tab
        if (ctrl && keysym == XK_Left)
        {
            if (currentTab == 0)
                currentTab = tabs.size() - 1;
            else
                currentTab--;
            redraw();
            return;
        }
        if (ctrl && (keysym == XK_D || keysym == XK_d))
        {
            closeTab(currentTab);
            if (appRunning)
                redraw();
            return;
        }
        if (ctrl && (keysym == XK_C || keysym == XK_c))
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
                string p = ts.pending.empty() ? ts.prompt : "> ";
                ts.lines.push_back(p + ts.input + "^C");
                ts.input.clear();
                ts.cursor = 0;
                ts.pending.clear(); // cancels a half-typed multiline command
                ts.lastStatus = 130;
            }
            ts.scroll = 0;
            redraw();
            return;
        }

        // Ctrl+Z: stop the running command and turn it into a job
        if (ctrl && (keysym == XK_Z || keysym == XK_z))
        {
            TerminalState &ts = getCurrTab();
            if (ts.busy && ts.pgid > 0)
            {
                kill(-ts.pgid, SIGTSTP); // whole pipeline stops

                if (ts.jobs.empty() && ts.fgJobId == 0)
                    ts.nextJobId = 1;
                Job job;
                job.id = ts.fgJobId > 0 ? ts.fgJobId : ts.nextJobId++; // keep [n] if it came from fg
                ts.fgJobId = 0;
                job.pgid = ts.pgid;
                job.pids = ts.childPids;
                job.outFd = ts.outFd;
                job.cmd = ts.cmdText;
                job.stopped = true;
                ts.jobs.push_back(job);

                ts.partial += "^Z";
                ts.appendOutput("\n[" + to_string(job.id) + "]+  Stopped                 " + job.cmd + "\n");

                // the tab no longer runs it in the foreground: give the prompt back
                ts.outFd = -1;
                ts.childPids.clear();
                ts.pgid = -1;
                ts.busy = false;
                ts.lastStatus = 148; // 128 + SIGTSTP (20), like bash
            }
            ts.scroll = 0;
            redraw();
            return;
        }

        TerminalState &termState = getCurrTab();
        switch (keysym)
        {
        case XK_Return:
        {
            if (termState.busy)
                break;
            bool cont = !termState.pending.empty();
            termState.lines.push_back((cont ? "> " : termState.prompt) + termState.input);
            string full = cont ? termState.pending + "\n" + termState.input : termState.input;
            termState.pending.clear(); // runCommand sets it again if still incomplete
            runCommand(full, termState);
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
                newTab();
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
