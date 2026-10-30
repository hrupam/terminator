#include "gui.h"
#include "tabs.h"
#include <cstdio>
#include <string>

using namespace std;

Display *disp;
Window win;
GC gc;
XFontStruct *font;
unsigned int width = 1200, height = 800;
unsigned long bgColor, fgColor, accentColor, dimColor; // pixel values from namedColor()
Atom wmDeleteMessage;

vector<pair<int, int>> tabBounds;
pair<int, int> plusBounds;
vector<pair<int, int>> tabCloseBounds;

static unsigned long
namedColor(const char *name, unsigned long fallback)
{
    XColor screenColor, exactColor;
    Colormap cmap = DefaultColormap(disp, DefaultScreen(disp));
    if (XAllocNamedColor(disp, cmap, name, &screenColor, &exactColor))
        return screenColor.pixel;
    return fallback;
}

bool initGui()
{
    disp = XOpenDisplay(NULL);
    if (!disp)
    {
        fprintf(stderr, "Unable to open display\n");
        return false;
    }

    int scr = DefaultScreen(disp);
    Window rootWin = RootWindow(disp, scr);

    bgColor = namedColor("gray12", BlackPixel(disp, scr));
    fgColor = namedColor("gray85", WhitePixel(disp, scr));
    accentColor = namedColor("SteelBlue", WhitePixel(disp, scr));
    dimColor = namedColor("gray40", WhitePixel(disp, scr));

    int x = 0, y = 0;
    unsigned int border_width = 1;
    unsigned long border = dimColor;

    win = XCreateSimpleWindow(disp, rootWin, x, y, width, height, border_width, border, bgColor);
    XStoreName(disp, win, "Terminator");

    XSelectInput(disp, win, ExposureMask | KeyPressMask | StructureNotifyMask | ButtonPressMask);

    wmDeleteMessage = XInternAtom(disp, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(disp, win, &wmDeleteMessage, 1);

    const char *fontNames[] = {"12x24", "10x20", "9x15", "fixed"};
    font = nullptr;
    for (const char *name : fontNames)
    {
        font = XLoadQueryFont(disp, name);
        if (font)
            break;
    }
    if (!font)
    {
        fprintf(stderr, "Unable to load any font\n");
        return false;
    }

    gc = XCreateGC(disp, win, 0, NULL);
    XSetForeground(disp, gc, fgColor);
    XSetFont(disp, gc, font->fid);

    XMapWindow(disp, win);
    return true;
}

static void drawTabBar(int lineHeight, int charw)
{
    int top = 2;                        // small gap from the window edge
    int boxH = lineHeight + 4;          // label box height
    int textY = top + 2 + font->ascent; // baseline of the label text
    int x = 4;

    tabBounds.clear();
    tabCloseBounds.clear();
    for (int i = 0; i < (int)tabs.size(); i++)
    {
        string path = tabs[i].getShortCwdPath();
        if (path.size() > 15) // keep labels short
            path = "..." + path.substr(path.size() - 12);
        string label = " " + to_string(i + 1) + ": " + path + (tabs[i].busy ? " *" : "") + " ";
        string closeLabel = "x ";
        int w = (label.size() + closeLabel.size()) * charw;
        int closeX = x + label.size() * charw; // where the 'x' starts

        if (i == currentTab) // highlighted: accent-filled box, text on top
        {
            XSetForeground(disp, gc, accentColor);
            XFillRectangle(disp, win, gc, x, top, w, boxH);
        }
        else // others: dim outline
        {
            XSetForeground(disp, gc, dimColor);
            XDrawRectangle(disp, win, gc, x, top, w, boxH);
        }
        XSetForeground(disp, gc, fgColor);
        XDrawString(disp, win, gc, x, textY, label.c_str(), label.size());
        XDrawString(disp, win, gc, closeX, textY, closeLabel.c_str(), closeLabel.size());

        tabBounds.push_back({x, x + w});
        tabCloseBounds.push_back({closeX - charw / 2, closeX + charw + charw / 2}); // a little wider than the 'x', easier to hit
        x += w + charw;                                                             // gap between labels
    }

    // [ + ] button for a new tab
    string plus = " + ";
    int pw = plus.size() * charw;
    XSetForeground(disp, gc, dimColor);
    XDrawRectangle(disp, win, gc, x, top, pw, boxH);
    XSetForeground(disp, gc, fgColor);
    XDrawString(disp, win, gc, x, textY, plus.c_str(), plus.size());
    plusBounds = {x, x + pw};

    // separator line between the bar and the text area
    int sepY = TABHEIGHT * lineHeight - lineHeight / 2;
    XSetForeground(disp, gc, dimColor);
    XDrawLine(disp, win, gc, 0, sepY, width, sepY);

    XSetForeground(disp, gc, fgColor); // back to text colour for the rest of redraw
}

void redraw()
{
    TerminalState &termState = getCurrTab();
    XClearWindow(disp, win);
    int lineHeight = font->ascent + font->descent;
    int charw = font->max_bounds.width;

    drawTabBar(lineHeight, charw);

    int rows = height / lineHeight - TABHEIGHT; // how many lines fit
    if (rows < 1)
        rows = 1;
    int total = termState.lines.size() + 1;      // +1 for the prompt line
    int first = total > rows ? total - rows : 0; // first line that fits

    // scroll
    int maxScroll = total - rows; // how far up you can go
    if (maxScroll < 0)
        maxScroll = 0;
    if (termState.scroll > maxScroll)
        termState.scroll = maxScroll;
    if (termState.scroll < 0)
        termState.scroll = 0;

    first -= termState.scroll;
    int last = first + rows; // draw rows lines, not up to total
    if (last > total)
        last = total;

    for (int v = first; v < last; v++)
    {
        int y = (v - first + TABHEIGHT) * lineHeight + font->ascent;

        if (v < (int)termState.lines.size()) // an output line
        {
            const string &line = termState.lines[v];
            XDrawString(disp, win, gc, 4, y, line.c_str(), line.length());
        }
        else if (termState.busy)
        {
            const string &p = termState.partial;
            XDrawString(disp, win, gc, 4, y, p.c_str(), p.length());
        }
        else // the prompt line
        {
            string p = termState.pending.empty() ? termState.prompt : "> ";
            string displayText = p + termState.input;
            XDrawString(disp, win, gc, 4, y, displayText.c_str(), displayText.length());

            int col = p.size() + termState.cursor;
            XSetForeground(disp, gc, accentColor); // cursor in accent colour
            XFillRectangle(disp, win, gc, 4 + col * charw, y + 1, charw, 2);
            XSetForeground(disp, gc, fgColor);
        }
    }
}

void closeTerminal()
{
    unsigned long colors[] = {bgColor, fgColor, accentColor, dimColor};
    XFreeColors(disp, DefaultColormap(disp, DefaultScreen(disp)), colors, 4, 0);
    XFreeFont(disp, font);
    XFreeGC(disp, gc);
    XDestroyWindow(disp, win);
    XCloseDisplay(disp);
}
