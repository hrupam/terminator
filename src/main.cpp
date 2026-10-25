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

struct Token
{
    string text;
    bool isOp = false; // unquoted | < > >>
};

class TerminalState
{
public:
    vector<string> lines;    /* finished output lines */
    string partial;          /* output line still being written (no \n yet) */
    string input;            /* the line being typed */
    string pending;          // unfinished input waiting for more lines, "" if none
    int cursor = 0;          /* length and cursor position in input */
    int scroll = 0;          /* how many lines scrolled back, 0 = bottom */
    string prompt;           /* "user@terminator> " or "> " */
    string cwd;              /* shell's current directory */
    int lastStatus = 0;      /* $? : exit status of the last command */
    bool isRunning = true;   /* running state of the terminal */
    int outFd = -1;          // read end of the running command's pipe, -1 if none
    vector<pid_t> childPids; // every process in the running pipeline
    pid_t pgid = -1;         // their process group (= first child's pid)
    bool busy = false;       // a command is running: don't draw the prompt

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

class CommandStage
{
public:
    vector<string> words;
    string inFile;
    string outFile;
    bool isAppendMode = false;
};

vector<pair<int, int>> tabBounds; // x start / x end of each tab label, filled in redraw
pair<int, int> plusBounds;        // the [ + ] button

bool appRunning = true;

TerminalState &getCurrTab()
{
    return tabs[currentTab];
}

bool tokenize(const string &s, TerminalState &ts, vector<Token> &out); // defined below

// Runs one command (no pipes inside), waits for it, returns what it printed.
string captureOutput(const string &cmd, TerminalState &ts)
{
    vector<Token> toks;
    if (!tokenize(cmd, ts, toks) || toks.empty())
        return "";

    vector<string> words;
    for (Token &t : toks)
        words.push_back(t.text);
    vector<char *> argv;
    for (string &w : words)
        argv.push_back(w.data());
    argv.push_back(nullptr);
    string notFound = "terminator: " + words[0] + ": command not found\n";

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return "";

    pid_t pid = fork();
    if (pid == 0) // child
    {
        int dn = open("/dev/null", O_RDONLY);
        dup2(dn, STDIN_FILENO);
        close(dn);
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        chdir(ts.cwd.c_str());
        execvp(argv[0], argv.data());
        write(STDERR_FILENO, notFound.c_str(), notFound.size());
        _exit(127);
    }

    close(p[1]); // MUST: otherwise read() below never returns 0

    string out;
    char buf[4096];
    ssize_t r;
    while ((r = read(p[0], buf, sizeof(buf))) > 0)
        out.append(buf, r);
    close(p[0]);

    if (pid > 0)
    {
        int st;
        waitpid(pid, &st, 0);
        ts.lastStatus = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }

    while (!out.empty() && out.back() == '\n')
        out.pop_back(); // bash removes trailing newlines
    return out;
}

// i points at '$'. Appends the expansion to cur and advances i.
bool expandDollar(const string &s, size_t &i, TerminalState &ts, string &cur)
{
    size_t n = s.size();
    i++; // skip '$'
    if (i < n && s[i] == '?')
    {
        cur += to_string(ts.lastStatus);
        i++;
    }
    else if (i < n && s[i] == '(')
    {
        int depth = 0;
        size_t j = i;
        for (; j < n; j++)
        {
            if (s[j] == '(')
                depth++;
            else if (s[j] == ')')
            {
                depth--;
                if (depth == 0)
                    break;
            }
        }
        if (j >= n)
            return false; // unclosed $( : multiline prompt, like an unclosed quote

        string inner = s.substr(i + 1, j - i - 1);
        cur += captureOutput(inner, ts);
        i = j + 1; // continue after the ')'
    }
    else if (i < n && s[i] == '{')
    {
        size_t j = s.find('}', i);
        if (j == string::npos)
            return false; // unclosed ${
        const char *v = getenv(s.substr(i + 1, j - i - 1).c_str());
        if (v)
            cur += v;
        i = j + 1;
    }
    else if (i < n && (isalpha(s[i]) || s[i] == '_'))
    {
        size_t j = i;
        while (j < n && (isalnum(s[j]) || s[j] == '_'))
            j++;
        const char *v = getenv(s.substr(i, j - i).c_str());
        if (v)
            cur += v;
        i = j;
    }
    else
        cur += '$'; // lone $ stays literal: echo 5$
    return true;
}

bool tokenize(const string &s, TerminalState &ts, vector<Token> &out)
{
    out.clear();
    string cur;
    bool inWord = false;
    size_t i = 0, n = s.size();

    auto endWord = [&]()
    {
        if (inWord)
            out.push_back({cur, false});
        cur.clear();
        inWord = false;
    };

    while (i < n)
    {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n')
        {
            endWord();
            i++;
        }
        else if (c == '|' || c == '<')
        {
            endWord();
            out.push_back({string(1, c), true});
            i++;
        }
        else if (c == '>')
        {
            endWord();
            if (i + 1 < n && s[i + 1] == '>')
            {
                out.push_back({">>", true});
                i += 2;
            }
            else
            {
                out.push_back({">", true});
                i++;
            }
        }
        else if (c == '\\')
        {
            if (i + 1 >= n)
                return false;     // trailing \ : wants another line
            if (s[i + 1] != '\n') // "\<newline>" = line continuation, adds nothing
            {
                cur += s[i + 1];
                inWord = true;
            }
            i += 2;
        }
        else if (c == '\'')
        {
            size_t j = s.find('\'', i + 1);
            if (j == string::npos)
                return false; // unclosed '
            cur += s.substr(i + 1, j - i - 1);
            inWord = true;
            i = j + 1;
        }
        else if (c == '"')
        {
            inWord = true;
            i++;
            while (true)
            {
                if (i >= n)
                    return false; // unclosed "
                char d = s[i];
                if (d == '"')
                {
                    i++;
                    break;
                }
                if (d == '\\' && i + 1 < n && strchr("\"\\$\n", s[i + 1]))
                {
                    if (s[i + 1] != '\n')
                        cur += s[i + 1];
                    i += 2;
                }
                else if (d == '$')
                {
                    if (!expandDollar(s, i, ts, cur))
                        return false;
                }
                else
                {
                    cur += d;
                    i++;
                }
            }
        }
        else if (c == '$')
        {
            if (!expandDollar(s, i, ts, cur))
                return false;
            if (!cur.empty())
                inWord = true; // unquoted empty $X gives no argument, like bash
        }
        else
        {
            cur += c;
            inWord = true;
            i++;
        }
    }
    endWord();

    if (!out.empty() && out.back().isOp && out.back().text == "|")
        return false; // trailing | : wants another line
    return true;
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

void closeTab(int tabIndex)
{
    TerminalState &tab = tabs[tabIndex];
    if (tab.busy)
    {
        if (tab.pgid > 0)
            kill(-tab.pgid, SIGHUP);
        close(tab.outFd);
        for (pid_t chpid : tab.childPids)
            waitpid(chpid, nullptr, 0);
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
    vector<Token> toks;
    if (!tokenize(str, termState, toks))
    {
        termState.pending = str;
        return;
    }
    if (toks.empty())
        return;

    vector<CommandStage> stages;
    CommandStage currStage;
    for (size_t i = 0; i < toks.size(); i++)
    {
        Token &t = toks[i];

        if (t.isOp && t.text == "|")
        {
            if (currStage.words.empty())
            {
                termState.appendOutput("terminator: syntax error near '|'\n");
                termState.lastStatus = 2;
                return;
            }
            stages.push_back(currStage);
            currStage = CommandStage();
        }
        else if (t.isOp) // < > >>
        {
            if (i + 1 >= toks.size() || toks[i + 1].isOp)
            {
                termState.appendOutput("terminator: syntax error: missing file after " + t.text + "\n");
                termState.lastStatus = 2;
                return;
            }
            if (t.text == "<")
                currStage.inFile = toks[i + 1].text;
            else
            {
                currStage.outFile = toks[i + 1].text;
                currStage.isAppendMode = (t.text == ">>");
            }
            i++; // skip the filename
        }
        else // a normal word
            currStage.words.push_back(t.text);
    }
    if (currStage.words.empty() && !stages.empty())
    {
        termState.appendOutput("terminator: syntax error near '|'\n");
        termState.lastStatus = 2;
        return;
    }
    stages.push_back(currStage);

    if (stages.empty() || stages[0].words.empty())
        return;

    int n = stages.size();

    if (n == 1 && stages[0].words[0] == "exit")
    {
        termState.isRunning = false;
        return;
    }

    if (n == 1 && stages[0].words[0] == "cd")
    {
        termState.lastStatus = builtinCd(stages[0].words, termState);
        return;
    }

    if (n == 1 && stages[0].words[0] == "clear")
    {
        termState.lines.clear();
        termState.partial.clear();
        termState.lastStatus = 0;
        return;
    }

    auto resolve = [&](const string &f)
    { return f[0] == '/' ? f : termState.cwd + "/" + f; };
    vector<vector<char *>> argvs(n);
    vector<string> notFound(n), inPaths(n), outPaths(n), openFail(n);

    for (int i = 0; i < n; i++)
    {
        CommandStage &stage = stages[i];
        for (string &w : stage.words)
            argvs[i].push_back(w.data());
        argvs[i].push_back(nullptr);

        notFound[i] = "terminator: " + stage.words[0] + ": command not found\n";
        if (!stage.inFile.empty())
            inPaths[i] = resolve(stage.inFile);
        if (!stage.outFile.empty())
            outPaths[i] = resolve(stage.outFile);
        openFail[i] = "terminator: cannot open file\n";

        if (!inPaths[i].empty() && access(inPaths[i].c_str(), R_OK) != 0)
        {
            termState.appendOutput("terminator: " + stage.inFile + ": " + strerror(errno) + "\n");
            termState.lastStatus = 1;
            return;
        }
    }

    string badDir = "terminator: cannot enter " + termState.cwd + "\n";

    // Pipes
    int outPipe[2]; // final pipe
    if (pipe2(outPipe, O_CLOEXEC) < 0)
    {
        termState.lastStatus = 1;
        return;
    }
    vector<vector<int>> pipes(n - 1, vector<int>(2));
    for (int i = 0; i < n - 1; i++)
    {
        int *fd = pipes[i].data();

        if (pipe2(fd, O_CLOEXEC) < 0)
        {

            for (int j = 0; j < i; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            close(outPipe[0]);
            close(outPipe[1]);
            termState.lastStatus = 1;
            return;
        }
    }

    // Fork
    vector<pid_t> pids;
    pid_t pgid = 0;
    for (int i = 0; i < n; i++)
    {
        pid_t pid = fork();
        if (pid < 0)
            break;
        if (pid == 0)
        {
            setpgid(0, pgid);

            if (i == 0)
            {
                int devNullFd = open("/dev/null", O_RDONLY);
                dup2(devNullFd, STDIN_FILENO);
                close(devNullFd);
            }
            else
                dup2(pipes[i - 1][0], STDIN_FILENO);

            if (i == n - 1)
                dup2(outPipe[1], STDOUT_FILENO);
            else
                dup2(pipes[i][1], STDOUT_FILENO);
            dup2(outPipe[1], STDERR_FILENO);

            // redirection
            if (!inPaths[i].empty())
            {
                int inFileFd = open(inPaths[i].c_str(), O_RDONLY);
                if (inFileFd < 0)
                {
                    write(STDERR_FILENO, openFail[i].c_str(), openFail[i].size());
                    _exit(1);
                }
                dup2(inFileFd, STDIN_FILENO);
                close(inFileFd);
            }

            if (!outPaths[i].empty())
            {
                auto flags = O_WRONLY | O_CREAT | (stages[i].isAppendMode ? O_APPEND : O_TRUNC);
                int outFileFd = open(outPaths[i].c_str(), flags, 0644);

                if (outFileFd < 0)
                {
                    write(STDERR_FILENO, openFail[i].c_str(), openFail[i].size());
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
            execvp(argvs[i][0], argvs[i].data());
            write(STDERR_FILENO, notFound[i].c_str(), notFound[i].size());
            _exit(127);
        }
        else
        {
            if (pgid == 0)
                pgid = pid;
            setpgid(pid, pgid);
            pids.push_back(pid);
        }
    }

    // parent cleanup
    close(outPipe[1]);
    for (auto &p : pipes)
    {
        close(p[0]);
        close(p[1]);
    }
    if (pids.empty()) // the very first fork failed
    {
        close(outPipe[0]);
        termState.appendOutput("terminator: fork failed\n");
        termState.lastStatus = 1;
        return;
    }
    termState.outFd = outPipe[0];
    termState.pgid = pgid;
    termState.childPids = pids;
    termState.busy = true;
}

void finishCommand(TerminalState &termState)
{
    close(termState.outFd); // close read end
    for (size_t i = 0; i < termState.childPids.size(); i++)
    {
        int status;
        waitpid(termState.childPids[i], &status, 0);
        if (i == termState.childPids.size() - 1)
        {
            if (WIFEXITED(status))
                termState.lastStatus = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                termState.lastStatus = 128 + WTERMSIG(status);
        }
    }

    if (!termState.partial.empty()) // output without a final \n
    {
        termState.lines.push_back(termState.partial);
        termState.partial.clear();
    }
    termState.outFd = -1;
    termState.childPids.clear();
    termState.pgid = -1;
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