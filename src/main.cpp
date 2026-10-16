#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <bits/stdc++.h>
#include <unistd.h>
#include <sys/wait.h>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>

#include "string_helper.h"

using namespace std;

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

    void setPrompt()
    {
        string cwdPath = this->cwd;
        string homePath = getenv("HOME");
        string pathText = cwdPath;
        if (startsWith(cwdPath, homePath))
        {
            pathText = "~" + cwdPath.substr(homePath.length());
        }
        string promptText = "rupam@terminator: " + pathText + "> ";
        this->prompt = promptText;
    }
};

Display *disp;
Window win;
GC gc;
XFontStruct *font;
unsigned int width = 1200, height = 800;

Atom wmDeleteMessage;

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
            XFillRectangle(disp, win, gc, 4 + col * charw, y + 1, charw, 2);
        }
    }
}

void closeTerminal()
{
    XFreeFont(disp, font);
    XFreeGC(disp, gc);
    XCloseDisplay(disp);
}

int builtinCd(vector<string> &words)
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

void runCommand(const string &str)
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

    if (words[0] == "exit")
    {
        termState.isRunning = false;
        return;
    }

    if (words[0] == "cd")
    {
        termState.lastStatus = builtinCd(words);
        return;
    }

    vector<char *> argv;
    for (string &w : words)
        argv.push_back(w.data());
    argv.push_back(nullptr);

    string cmdNotFound = "terminator: " + words[0] + ": command not found\n";
    string badDir = "terminator: cannot enter " + termState.cwd + "\n";

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

void finishCommand()
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

void handleEvent(XEvent &ev)
{
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
            if (termState.busy)
                break;
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
        termState.isRunning = false;
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
    XSetForeground(disp, gc, WhitePixel(disp, scr));
    XSetFont(disp, gc, font->fid);

    XMapWindow(disp, win);

    // getting initial working directory
    char cwdbuf[PATH_MAX];
    if (getcwd(cwdbuf, sizeof(cwdbuf)))
        termState.cwd = cwdbuf;
    termState.setPrompt();

    int xfd = ConnectionNumber(disp);
    fcntl(xfd, F_SETFD, FD_CLOEXEC);

    while (termState.isRunning)
    {
        while (XPending(disp))
        {
            XEvent ev;
            XNextEvent(disp, &ev);
            handleEvent(ev);
        }
        if (!termState.isRunning)
            break;
        XFlush(disp);

        vector<pollfd> fds;
        fds.push_back({xfd, POLLIN, 0});
        if (termState.outFd != -1)
            fds.push_back({termState.outFd, POLLIN, 0});
        poll(fds.data(), fds.size(), -1);

        // to handle child's command execution
        if (fds.size() > 1 && fds[1].revents & (POLLIN | POLLHUP))
        {
            char buffer[4096];
            ssize_t n = read(termState.outFd, buffer, sizeof(buffer));
            if (n > 0)
                termState.appendOutput(string(buffer, n));
            else if (n == 0 || errno != EINTR)
                finishCommand();
            redraw();
        }
    }

    closeTerminal();

    return 0;
}