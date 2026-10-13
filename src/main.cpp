#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/wait.h>
#include "string_helper.h"

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

    void appendOutput(const string &data)
    {
        for (char c : data)
        {
            if (c == '\n')
            {
                lines.push_back(partial);
                partial.clear();
            }
            else if (c == '\t')
                do
                    partial += ' ';
                while (partial.size() % 8);
            else if ((unsigned char)c >= 32)
                partial += c;
        }
    }
};

Display *disp;
Window win;
GC gc;
XFontStruct *font;
unsigned int width = 800, height = 600;

TerminalState termState;

void redraw(void)
{
    XClearWindow(disp, win);
    int lineHeight = font->ascent + font->descent;
    int charw = font->max_bounds.width;

    int rows = height / lineHeight; // how many lines fit
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
        int y = (v - first) * lineHeight + font->ascent;

        if (v < (int)termState.lines.size()) // an output line
        {
            const string &line = termState.lines[v];
            XDrawString(disp, win, gc, 4, y, line.c_str(), line.length());
        }
        else // the prompt line
        {
            string displayText = termState.prompt + termState.input;
            XDrawString(disp, win, gc, 4, y, displayText.c_str(), displayText.length());

            int col = termState.prompt.size() + termState.cursor;
            XFillRectangle(disp, win, gc, 4 + col * charw, y + 1, charw, 2);
        }
    }
}

void runCommand(const string &str)
{
    string cmd = processCommand(str);
    if (cmd.empty())
        return;
    stringstream ss(cmd);
    vector<string> words;
    for (string token; ss >> token;)
        words.push_back(token);

    vector<char *> argv;
    for (string &w : words)
        argv.push_back(w.data());
    argv.push_back(nullptr);
    string notFound = "terminator: " + words[0] + ": command not found\n";

    int pipefd[2]; // pipefd[0] - read end, pipefd[1] - write end
    int pipeStatus = pipe(pipefd);
    if (pipeStatus < 0)
    {
        perror("pipe error");
        return;
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork error");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0)
    {
        // child
        close(pipefd[0]); // close read end
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]); // close write end after dup2
        execvp(argv[0], argv.data());

        write(STDERR_FILENO, notFound.c_str(), notFound.size());
        _exit(127);
    }
    else
    {
        // parent
        close(pipefd[1]); // close write end
        char buffer[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0)
            termState.appendOutput(string(buffer, n));

        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);
        if (!termState.partial.empty()) // output without a final \n
        {
            termState.lines.push_back(termState.partial);
            termState.partial.clear();
        }
    }
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
    unsigned int border_width = 1;
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
                runCommand(termState.input);
                termState.input.clear();
                termState.cursor = 0;
                termState.scroll = 0;
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
        }
        else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wmDeleteMessage)
        {
            break;
        }
        else if (ev.type == ConfigureNotify)
        {
            width = ev.xconfigure.width;
            height = ev.xconfigure.height;
            redraw();
        }
        else if (ev.type == ButtonPress)
        {
            if (ev.xbutton.button == 4)
                termState.scroll += 3; // wheel up
            if (ev.xbutton.button == 5)
                termState.scroll -= 3; // wheel down
            redraw();
        }
    }

    XFreeFont(disp, font);
    XFreeGC(disp, gc);
    XCloseDisplay(disp);

    return 0;
}