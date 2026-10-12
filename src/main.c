#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>

struct term
{
    char **lines; /* finished output lines */
    int nlines, cap;
    char partial[4096]; /* output line still being written (no \n yet) */
    int plen;
    char input[4096]; /* the line being typed */
    int len, cursor;  /* length and cursor position in input */
    int scroll;       /* how many lines scrolled back, 0 = bottom */
    char prompt[256]; /* "user@terminator> " or "> " */
};

int main(void)
{
    Display *disp = XOpenDisplay(NULL);
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

    Window win = XCreateSimpleWindow(disp, rootWin, x, y, width, height, border_width, border, background);
    XStoreName(disp, win, "Terminator");

    XSelectInput(disp, win, ExposureMask | KeyPressMask | StructureNotifyMask | ButtonPressMask);

    Atom wmDeleteMessage = XInternAtom(disp, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(disp, win, &wmDeleteMessage, 1);

    XFontStruct *font = XLoadQueryFont(disp, "fixed");
    GC gc = XCreateGC(disp, win, 0, NULL);
    XSetForeground(disp, gc, WhitePixel(disp, scr));
    XSetFont(disp, gc, font->fid);

    XMapWindow(disp, win);

    while (1)
    {
        XEvent ev;
        XNextEvent(disp, &ev);
        if (ev.type == Expose && ev.xexpose.count == 0)
        {
            const char *p = "rupam@terminator> ";
            XClearWindow(disp, win);
            XDrawString(disp, win, gc, 4, font->ascent, p, strlen(p));
        }
        if (ev.type == KeyPress)
        {
            char buf[128] = {0};
            KeySym keysym;
            int len = XLookupString(&ev.xkey, buf, sizeof(buf), &keysym, NULL);
            if (len > 0)
            {
                buf[len] = '\0';

                printf("key %s, %d byte(s), state=0x%x\n",
                       XKeysymToString(keysym), len, ev.xkey.state);
            }
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