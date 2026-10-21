#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/wait.h>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>

#include "string_helper.h"

#define TABHEIGHT 2

using namespace std;

Display *disp;
Window win;
GC gc;
XFontStruct *font;
unsigned int width = 1200, height = 800;
unsigned long bgColor, fgColor, accentColor, dimColor; // pixel values from namedColor()

Atom wmDeleteMessage;

unsigned long namedColor(const char *name, unsigned long fallback)
{
    XColor screenColor, exactColor;
    Colormap cmap = DefaultColormap(disp, DefaultScreen(disp));
    if (XAllocNamedColor(disp, cmap, name, &screenColor, &exactColor))
        return screenColor.pixel;
    return fallback;
}

class TerminalState
{
public:
    vector<string> lines;  /* finished output lines */
    string partial;        /* output line still being written (no \n yet) */
    string input;          /* the line being typed */
    int cursor = 0;        /* length and cursor position in input */
    int scroll = 0;        /* how many lines scrolled back, 0 = bottom */
    string prompt;         /* "user@terminator> " or "> " */
    string cwd;            /* shell's current directory */
    int lastStatus = 0;    /* $? : exit status of the last command */
    bool isRunning = true; /* running state of the terminal */
    int outFd = -1;        // read end of the running command's pipe, -1 if none
    pid_t childPid = -1;   // the running command
    bool busy = false;     // a command is running: don't draw the prompt

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

    string getShortCwdPath()
    {
        string cwdPath = this->cwd;
        string homePath = getenv("HOME");
        string pathText = cwdPath;
        if (startsWith(cwdPath, homePath))
            pathText = "~" + cwdPath.substr(homePath.length());
        return pathText;
    }
    void setPrompt()
    {
        string pathText = this->getShortCwdPath();
        string promptText = "rupam@terminator: " + pathText + "> ";
        this->prompt = promptText;
    }
};

vector<TerminalState> tabs;
int currentTab = 0;

vector<pair<int, int>> tabBounds; // x start / x end of each tab label, filled in redraw
pair<int, int> plusBounds;        // the [ + ] button

bool appRunning = true;

TerminalState &getCurrTab()
{
    return tabs[currentTab];
}

void drawTabBar(int lineHeight, int charw)
{
    int top = 2;                        // small gap from the window edge
    int boxH = lineHeight + 4;          // label box height
    int textY = top + 2 + font->ascent; // baseline of the label text
    int x = 4;

    tabBounds.clear();
    for (int i = 0; i < (int)tabs.size(); i++)
    {
        string path = tabs[i].getShortCwdPath();
        if (path.size() > 15) // keep labels short
            path = "..." + path.substr(path.size() - 12);
        string label = " " + to_string(i + 1) + ": " + path + (tabs[i].busy ? " *" : "") + " ";
        int w = label.size() * charw;

        if (i == currentTab) // highlighted: accent-filled box, text on top
        {
            XSetForeground(disp, gc, accentColor);
            XFillRectangle(disp, win, gc, x, top, w, boxH);
            XSetForeground(disp, gc, fgColor);
            XDrawString(disp, win, gc, x, textY, label.c_str(), label.size());
        }
        else // others: dim outline, normal text
        {
            XSetForeground(disp, gc, dimColor);
            XDrawRectangle(disp, win, gc, x, top, w, boxH);
            XSetForeground(disp, gc, fgColor);
            XDrawString(disp, win, gc, x, textY, label.c_str(), label.size());
        }
        tabBounds.push_back({x, x + w});
        x += w + charw; // gap between labels
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

void redraw(void)
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
            string displayText = termState.prompt + termState.input;
            XDrawString(disp, win, gc, 4, y, displayText.c_str(), displayText.length());

            int col = termState.prompt.size() + termState.cursor;
            XSetForeground(disp, gc, accentColor); // cursor in accent colour
            XFillRectangle(disp, win, gc, 4 + col * charw, y + 1, charw, 2);
            XSetForeground(disp, gc, fgColor);
        }
    }
}

void closeTab(int tabIndex)
{
    TerminalState &tab = tabs[tabIndex];
    if (tab.busy)
    {
        kill(tab.childPid, SIGHUP);
        close(tab.outFd);
        waitpid(tab.childPid, nullptr, 0);
    }

    int n = tabs.size();
    if (n == 1)
    {
        appRunning = false;
        return;
    }

    tabs.erase(tabs.begin() + tabIndex);

    if (currentTab > tabIndex)
        currentTab--;
    else if (currentTab == tabIndex && tabIndex == n - 1)
        currentTab--;
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

int builtinCd(vector<string> &words, TerminalState &termState)
{
    if (words.size() > 2)
    {
        termState.appendOutput("terminator: cd: too many arguments\n");
        return 1;
    }
    string home = getenv("HOME") ? getenv("HOME") : "/";
    string arg = words.size() == 1 ? "~" : words[1];
    string path;
    if (arg == "~") // "cd" or "cd ~"
    {
        path = home;
    }
    else
    {
        if (startsWith(arg, "~/"))
        {
            path = home + arg.substr(1);
        }
        else if (startsWith(arg, "/"))
        {
            path = arg;
        }
        else
            path = termState.cwd + "/" + arg;
    }

    // check if path is valid directory
    error_code ec;
    if (!filesystem::is_directory(path, ec))
    {
        string errMsg = ec ? ec.message() : "Not a directory"; // for - cd /etc/passwd
        termState.appendOutput("terminator: cd: " + arg + ": " + errMsg + "\n");
        return 1;
    }

    if (access(path.c_str(), X_OK) != 0)
    {
        termState.appendOutput("terminator: cd: " + arg + ": Permission denied\n");
        return 1;
    }

    string resolvedPath = filesystem::canonical(path, ec).string();
    if (ec)
    {
        termState.appendOutput("terminator: cd: " + arg + ": " + ec.message() + "\n");
        return 1;
    }
    termState.cwd = resolvedPath;
    termState.setPrompt();

    return 0;
}

void runCommand(const string &str, TerminalState &termState)
{
    string cmd = processCommand(str); // removes whitespaces

    if (cmd.empty())
        return;

    stringstream ss(cmd);
    vector<string> words;
    for (string token; ss >> token;)
        words.push_back(token);

    for (string &w : words)
    {
        if (w == "$?")
            w = to_string(termState.lastStatus);
    }

    vector<string> cmdWords;
    string inFile, outFile;
    bool isAppendMode = false;
    for (size_t i = 0; i < words.size(); i++)
    {
        string w = words[i];
        if (w == ">" || w == ">>" || w == "<")
        {
            if (i + 1 >= words.size())
            {
                termState.appendOutput("terminator: syntax error: missing file after " + w + "\n");
                termState.lastStatus = 2;
                return;
            }
            if (w == "<")
                inFile = words[i + 1];
            else
            {
                outFile = words[i + 1];
                if (w == ">>")
                    isAppendMode = true;
            }
            i++;
        }
        else
        {
            cmdWords.push_back(w);
        }
    }

    if (cmdWords.empty())
        return;

    if (cmdWords[0] == "exit")
    {
        termState.isRunning = false;
        return;
    }

    if (cmdWords[0] == "cd")
    {
        termState.lastStatus = builtinCd(cmdWords, termState);
        return;
    }

    vector<char *> argv;
    for (string &w : cmdWords)
        argv.push_back(w.data());
    argv.push_back(nullptr);

    string cmdNotFound = "terminator: " + cmdWords[0] + ": command not found\n";
    string badDir = "terminator: cannot enter " + termState.cwd + "\n";

    // Redirection
    auto resolve = [&](const string &f)
    { return f[0] == '/' ? f : termState.cwd + "/" + f; };
    string inPath = inFile.empty() ? "" : resolve(inFile);
    string outPath = outFile.empty() ? "" : resolve(outFile);

    if (!inPath.empty() && access(inPath.c_str(), R_OK) != 0)
    {
        termState.appendOutput("terminator: " + inFile + ": " + strerror(errno) + "\n");
        termState.lastStatus = 1;
        return;
    }
    string openFailMsg = "terminator: cannot open " + (outFile.empty() ? inFile : outFile) + "\n";

    int pipefd[2]; // pipefd[0] - read end, pipefd[1] - write end
    int pipeStatus = pipe2(pipefd, O_CLOEXEC);
    if (pipeStatus < 0)
    {
        termState.lastStatus = 1;
        perror("pipe error");
        return;
    }

    pid_t pid = fork(); // in parent - pid is child's pid, in child - pid is 0
    if (pid < 0)
    {
        termState.lastStatus = 1;
        perror("fork error");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0)
    {
        // child
        int devnullFd = open("/dev/null", O_RDONLY);
        dup2(devnullFd, STDIN_FILENO);
        close(devnullFd);

        close(pipefd[0]); // close read end
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]); // close write end after dup2

        // Redirection
        if (!inPath.empty())
        {
            int inFileFd = open(inPath.c_str(), O_RDONLY);
            if (inFileFd < 0)
            {
                write(STDERR_FILENO, openFailMsg.c_str(), openFailMsg.size());
                _exit(1);
            }
            dup2(inFileFd, STDIN_FILENO);
            close(inFileFd);
        }

        if (!outPath.empty())
        {
            auto flags = O_WRONLY | O_CREAT | (isAppendMode ? O_APPEND : O_TRUNC);
            int outFileFd = open(outPath.c_str(), flags, 0644);

            if (outFileFd < 0)
            {
                write(STDERR_FILENO, openFailMsg.c_str(), openFailMsg.size());
                _exit(1);
            }
            dup2(outFileFd, STDOUT_FILENO);
            close(outFileFd);
        }

        if (chdir(termState.cwd.c_str()) < 0)
        {
            write(STDERR_FILENO, badDir.c_str(), badDir.size());
            _exit(1);
        }
        execvp(argv[0], argv.data());

        write(STDERR_FILENO, cmdNotFound.c_str(), cmdNotFound.size());
        _exit(127);
    }
    else
    {
        // parent
        close(pipefd[1]); // close write end
        termState.childPid = pid;
        termState.outFd = pipefd[0];
        termState.busy = true;
    }
}

void finishCommand(TerminalState &termState)
{
    close(termState.outFd); // close read end
    int status;
    waitpid(termState.childPid, &status, 0);

    if (WIFEXITED(status))
        termState.lastStatus = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        termState.lastStatus = 128 + WTERMSIG(status);

    if (!termState.partial.empty()) // output without a final \n
    {
        termState.lines.push_back(termState.partial);
        termState.partial.clear();
    }
    termState.outFd = -1;
    termState.childPid = -1;
    termState.busy = false;
}

void newTab()
{
    TerminalState newTermState;
    newTermState.cwd = getCurrTab().cwd;
    newTermState.setPrompt();
    tabs.push_back(newTermState);
    currentTab = tabs.size() - 1;
}

bool closeTabsBasedOnRunning()
{
    bool isClosed = false;
    for (int i = (int)tabs.size() - 1; i >= 0; i--)
    {
        if (!tabs[i].isRunning)
        {
            closeTab(i);
            isClosed = true;
        }
    }
    return isClosed;
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

        TerminalState &termState = getCurrTab();
        switch (keysym)
        {
        case XK_Return:
            if (termState.busy)
                break;
            termState.lines.push_back(termState.prompt + termState.input);
            runCommand(termState.input, termState);
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
        return;
    }
    if (ev.type == ButtonPress && ev.xbutton.button == 1) // left click
    {
        int lineHeight = font->ascent + font->descent;
        if (ev.xbutton.y < TABHEIGHT * lineHeight) // inside the tab bar
        {
            int cx = ev.xbutton.x;
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
        return 1;
    }

    gc = XCreateGC(disp, win, 0, NULL);
    XSetForeground(disp, gc, fgColor);
    XSetFont(disp, gc, font->fid);

    XMapWindow(disp, win);

    tabs.emplace_back();

    // getting initial working directory
    char cwdbuf[PATH_MAX];
    if (getcwd(cwdbuf, sizeof(cwdbuf)))
        getCurrTab().cwd = cwdbuf;
    getCurrTab().setPrompt();

    int xfd = ConnectionNumber(disp);
    fcntl(xfd, F_SETFD, FD_CLOEXEC);

    while (appRunning)
    {
        while (XPending(disp))
        {
            XEvent ev;
            XNextEvent(disp, &ev);
            handleEvent(ev);
        }
        bool isTabClosed = closeTabsBasedOnRunning();
        if (!appRunning)
            break;
        if (isTabClosed)
            redraw();
        XFlush(disp);

        vector<pollfd> fds;
        fds.push_back({xfd, POLLIN, 0});
        vector<int> fdTab;
        for (size_t i = 0; i < tabs.size(); i++)
        {
            if (tabs[i].outFd != -1)
            {
                fds.push_back({tabs[i].outFd, POLLIN, 0});
                fdTab.push_back(i);
            }
        }

        poll(fds.data(), fds.size(), -1);

        // to handle child's command execution in Tabs
        bool changed = false;
        for (size_t k = 1; k < fds.size(); k++)
        {
            if (fds[k].revents & (POLLIN | POLLHUP))
            {
                char buffer[4096];
                TerminalState &tab = tabs[fdTab[k - 1]];
                ssize_t n = read(tab.outFd, buffer, sizeof(buffer));
                if (n > 0)
                    tab.appendOutput(string(buffer, n));
                else if (n == 0 || errno != EINTR)
                    finishCommand(tab);
                changed = true;
            }
        }
        if (changed)
            redraw();
    }

    closeTerminal();

    return 0;
}