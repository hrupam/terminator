#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <bits/stdc++.h>

using namespace std;

class TerminalState
{
public:
    vector<string> lines; /* finished output lines */
    string partial;       /* output line still being written (no \n yet) */
    string input;         /* the line being typed */
    int cursor = 0;       /* length and cursor position in input */
    int scroll = 0;       /* how many lines scrolled back, 0 = bottom */
    string prompt;        /* "user@terminator> " or "> " */

public:
    void appendInput(const string &s)
    {
        input.insert(cursor, s);
        cursor += s.length();
    }
};

Display *disp;
Window win;
GC gc;
XFontStruct *font;

TerminalState termState;

void redraw(void)
{
    XClearWindow(disp, win);
    int lineHeight = font->ascent + font->descent;
    int row = 0;
    for (const string &line : termState.lines)
    {
        XDrawString(disp, win, gc, 4, row * lineHeight + font->ascent, line.c_str(), line.length());
        row++;
    }
    string displayText = termState.prompt + termState.input;

    int y = row * lineHeight + font->ascent;

    XDrawString(disp, win, gc, 4, y, displayText.c_str(), displayText.length());

    // Draw the cursor
    int charw = font->max_bounds.width;
    int col = termState.prompt.size() + termState.cursor;
    XFillRectangle(disp, win, gc, 4 + col * charw, y + 1, charw, 2);
}

int main(void)
{
    disp = XOpenDisplay(NULL);
    if (!disp)
    {
        fprintf(stderr, "Unable to open display\n");
        return 1;
    }

    int scr = DefaultScreen(disp);
    Window rootWin = RootWindow(disp, scr);
    int x = 0, y = 0;
    unsigned int width = 800, height = 600, border_width = 1;
    unsigned long border = WhitePixel(disp, scr);
    unsigned long background = BlackPixel(disp, scr);

    win = XCreateSimpleWindow(disp, rootWin, x, y, width, height, border_width, border, background);
    XStoreName(disp, win, "Terminator");

    XSelectInput(disp, win, ExposureMask | KeyPressMask | StructureNotifyMask | ButtonPressMask);

    Atom wmDeleteMessage = XInternAtom(disp, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(disp, win, &wmDeleteMessage, 1);

    font = XLoadQueryFont(disp, "fixed");
    gc = XCreateGC(disp, win, 0, NULL);
    XSetForeground(disp, gc, WhitePixel(disp, scr));
    XSetFont(disp, gc, font->fid);

    XMapWindow(disp, win);

    termState.prompt = "rupam@terminator> ";
    while (1)
    {
        XEvent ev;
        XNextEvent(disp, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0)
        {
            redraw();
        }
        else if (ev.type == KeyPress)
        {
            char buf[128] = {0};
            KeySym keysym;
            int len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &keysym, NULL);

            switch (keysym)
            {
            case XK_Return:
                termState.lines.push_back(termState.prompt + termState.input);
                termState.lines.push_back("You typed: " + termState.input);
                termState.input.clear();
                termState.cursor = 0;
                break;

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

            default:

                if (len == 1 && (unsigned char)buf[0] >= 32 &&
                    !(ev.xkey.state & (ControlMask | Mod1Mask)))
                    termState.appendInput(buf);
            }
            redraw();
        }
        else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wmDeleteMessage)
        {
            break;
        }
    }

    XFreeFont(disp, font);
    XFreeGC(disp, gc);
    XCloseDisplay(disp);

    return 0;
}