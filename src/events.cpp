#include "events.h"
#include "gui.h"
#include "tabs.h"
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <string>
#include <unistd.h>
#include <csignal>
#include "keys.h"
#include "autocomplete.h"
#include <dirent.h>
#include <filesystem>

using namespace std;

// Replaces the word being completed (from wordStart to the cursor) with text.
static void replaceWord(TerminalState &ts, const string &text)
{
    ts.input.replace(ts.wordStart, ts.cursor - ts.wordStart, text);
    ts.cursor = ts.wordStart + text.size();
}

// Tab: complete the word before the cursor with file names from the tab's directory.
static void completeWord(TerminalState &ts)
{
    // the word before the cursor
    int start = ts.cursor;
    while (start > 0 && ts.input[start - 1] != ' ')
        start--;
    string word = ts.input.substr(start, ts.cursor - start);

    // "src/ma": look in <cwd>/src for names starting with "ma"
    size_t slash = word.rfind('/');
    string dirPart = (slash == string::npos) ? "" : word.substr(0, slash + 1);
    string prefix = (slash == string::npos) ? word : word.substr(slash + 1);
    string dir = dirPart.empty() ? ts.cwd
                                 : (dirPart[0] == '/' ? dirPart : ts.cwd + "/" + dirPart);

    // put every name in that directory into a trie
    Trie trie;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent *e = readdir(d))
    {
        string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        if (name[0] == '.' && (prefix.empty() || prefix[0] != '.'))
            continue; // hidden files only when the user typed a '.'
        error_code ec;
        if (filesystem::is_directory(dir + "/" + name, ec))
            name += '/'; // directories complete with a trailing /
        trie.insert(name);
    }
    closedir(d);

    vector<string> matches = trie.getWords(prefix);
    if (matches.empty())
        return; // no match: do nothing

    ts.wordStart = start + dirPart.size(); // only the part after the last '/' is replaced
    if (matches.size() == 1)               // one match: complete it
    {
        replaceWord(ts, matches[0]);
        return;
    }

    // several: extend to the longest common prefix, then show a numbered list to pick from
    replaceWord(ts, trie.autocomplete(prefix));
    ts.lines.push_back(ts.prompt + ts.input); // keep the typed line visible, like the spec's example
    string list;
    for (size_t i = 0; i < matches.size(); i++)
        list += to_string(i + 1) + ". " + matches[i] + "  ";
    ts.lines.push_back(list);
    ts.choices = matches; // the next digit key picks one
}

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
        TerminalState &ts = getCurrTab(); // GUI's Terminal State
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

    case ACT_COMPLETE: // Tab
    {
        TerminalState &ts = getCurrTab();
        if (!ts.busy)
            completeWord(ts);
        break;
    }

    case ACT_COPY: // Ctrl+Shift+C: the line being typed, or else the last output line
    {
        TerminalState &ts = getCurrTab();
        if (!ts.input.empty())
            clipboardText = ts.input;
        else if (!ts.lines.empty())
            clipboardText = ts.lines.back();
        else
            break;
        XSetSelectionOwner(disp, clipboardAtom, win, CurrentTime); // "I own the clipboard now"
        break;
    }

    case ACT_PASTE: // Ctrl+Shift+V: ask the clipboard's owner; the text arrives as SelectionNotify
        XConvertSelection(disp, clipboardAtom, utf8Atom, pasteProp, win, CurrentTime);
        break;

    default:
        break;
    }
    redraw();
}

// Someone (another app, or Terminator itself) wants the text we copied.
static void answerSelectionRequest(XSelectionRequestEvent &r)
{
    if (r.property == None) // very old clients: use the target as the property name
        r.property = r.target;

    XSelectionEvent reply = {};
    reply.type = SelectionNotify;
    reply.requestor = r.requestor;
    reply.selection = r.selection;
    reply.target = r.target;
    reply.property = r.property;
    reply.time = r.time;

    if (r.target == targetsAtom) // "which formats do you offer?"
    {
        Atom offered[] = {utf8Atom, XA_STRING};
        XChangeProperty(disp, r.requestor, r.property, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)offered, 2);
    }
    else if (r.target == utf8Atom || r.target == XA_STRING) // "give me the text"
        XChangeProperty(disp, r.requestor, r.property, r.target, 8, PropModeReplace,
                        (const unsigned char *)clipboardText.c_str(), clipboardText.size());
    else
        reply.property = None; // a format we don't offer

    XSendEvent(disp, r.requestor, False, 0, (XEvent *)&reply); // "done, look at your property"
}

// The clipboard's owner has put the text on our window (answer to Ctrl+Shift+V).
static void insertPastedText(XSelectionEvent &s)
{
    if (s.property == None)
        return; // clipboard empty, or no text form available

    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *data = nullptr;
    XGetWindowProperty(disp, win, pasteProp, 0, 1 << 20, True, AnyPropertyType,
                       &type, &format, &count, &remaining, &data); // True = delete after reading
    if (!data)
        return;

    string text;
    for (unsigned long i = 0; i < count; i++)
    {
        char c = data[i];
        if (c == '\n' || c == '\r' || c == '\t')
            text += ' '; // the input line is one line: newlines become spaces
        else if ((unsigned char)c >= 32)
            text += c;
    }
    XFree(data);
    getCurrTab().appendInput(text); // inserted at the cursor
}

void handleEvent(XEvent &ev)
{

    if (ev.type == Expose && ev.xexpose.count == 0)
    {
        redraw();
        return;
    }

    if (ev.type == SelectionRequest) // another app pastes what we copied
    {
        answerSelectionRequest(ev.xselectionrequest);
        return;
    }
    if (ev.type == SelectionNotify) // the text we asked for (paste) has arrived
    {
        insertPastedText(ev.xselection);
        redraw();
        return;
    }

    if (ev.type == KeyPress)
    {
        char buf[128] = {0};
        KeySym keysym;
        int len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &keysym, NULL);

        // a numbered completion list is showing: a digit picks, any other key cancels it
        TerminalState &cur = getCurrTab();
        if (!cur.choices.empty())
        {
            int k = (len == 1 && buf[0] >= '1' && buf[0] <= '9') ? buf[0] - '0' : 0;
            if (k >= 1 && k <= (int)cur.choices.size())
            {
                replaceWord(cur, cur.choices[k - 1]);
                cur.choices.clear();
                redraw();
                return;
            }
            cur.choices.clear(); // not a valid pick: drop the list, handle the key normally
        }

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
