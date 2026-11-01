#include "tabs.h"
#include "engine.h"
#include "gui.h"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include "terminal_state.h"

using namespace std;

vector<TerminalState> tabs;
int currentTab = 0;
bool appRunning = true;
bool useProcess = false;

TerminalState &getCurrTab()
{
    return tabs[currentTab];
}

void newTab(const string &cwd)
{
    // 3 pipes per tab. O_CLOEXEC: commands of one tab never inherit another tab's pipes.
    int in[2], out[2], ctl[2];
    if (pipe2(in, O_CLOEXEC) < 0 || pipe2(out, O_CLOEXEC) < 0 || pipe2(ctl, O_CLOEXEC) < 0)
        return;
    fcntl(out[0], F_SETFL, O_NONBLOCK); // the GUI must never block on a tab
    fcntl(ctl[0], F_SETFL, O_NONBLOCK);

    TerminalState engine; // the engine's copy: runs the commands
    engine.isEngine = true;
    engine.cwd = cwd;
    engine.inFd = in[0];
    engine.outFd = out[1];
    engine.ctlFd = ctl[1];

    TerminalState gui; // the GUI's copy: what is drawn
    gui.cwd = cwd;
    gui.setPrompt();
    gui.busy = true; // until the engine's first "R"
    gui.inFd = in[1];
    gui.outFd = out[0];
    gui.ctlFd = ctl[0];

    if (useProcess) // PROCESS MODE: the engine runs in a child process
    {
        pid_t pid = fork();
        if (pid == 0)
        {
            close(ConnectionNumber(disp)); // only the GUI talks to X (never XCloseDisplay here)
            for (TerminalState &t : tabs)  // drop the GUI ends of every other tab
            {
                close(t.inFd);
                close(t.outFd);
                close(t.ctlFd);
            }
            close(in[1]); // and of this tab
            close(out[0]);
            close(ctl[0]);
            engineMain(engine);
            _exit(0);
        }
        gui.enginePid = pid;
        close(in[0]); // the engine ends belong to the child now
        close(out[1]);
        close(ctl[1]);
    }
    else                                     // THREAD MODE: the engine runs in a thread of this process
        thread(engineMain, engine).detach(); // it closes its own ends when it finishes

    tabs.push_back(gui);
    currentTab = tabs.size() - 1;
}

// Stops a tab's engine and waits until it is finished (jobs killed, pipes closed).
static void shutdownTab(TerminalState &t)
{
    if (t.busy && t.pgid > 0)
        kill(-t.pgid, SIGHUP); // the engine may be waiting for this job
    close(t.inFd);             // the engine's getline returns -1: it cleans up and ends

    fcntl(t.ctlFd, F_SETFL, 0); // blocking read until EOF = the engine closed ctl = it is done
    char buf[256];
    while (read(t.ctlFd, buf, sizeof(buf)) > 0)
        ;
    if (t.enginePid > 0)
        waitpid(t.enginePid, nullptr, 0); // process mode: reap the tab process
    close(t.outFd);
    close(t.ctlFd);
}

void closeTab(int tabIndex)
{
    int n = tabs.size();
    if (n == 1)
    {
        appRunning = false; // shutdownAllTabs() runs after the main loop
        return;
    }

    shutdownTab(tabs[tabIndex]);
    tabs.erase(tabs.begin() + tabIndex);

    if (currentTab > tabIndex)
        currentTab--;
    else if (currentTab == tabIndex && tabIndex == n - 1)
        currentTab--;
}

bool closeTabsBasedOnRunning()
{
    bool isClosed = false;
    for (int i = (int)tabs.size() - 1; i >= 0 && appRunning; i--)
    {
        if (!tabs[i].isRunning)
        {
            closeTab(i);
            isClosed = true;
        }
    }
    return isClosed;
}

void shutdownAllTabs()
{
    for (TerminalState &t : tabs)
        shutdownTab(t);
}

void sendToEngine(TerminalState &tab, const string &msg)
{
    string s = msg + "\n";
    if (write(tab.inFd, s.data(), s.size()) < 0)
        return; // engine gone (SIGPIPE is ignored)
}

void handleEngineOutput(TerminalState &tab)
{
    char buf[4096];
    ssize_t n;
    while ((n = read(tab.outFd, buf, sizeof(buf))) > 0) // drain what is there now
        tab.appendOutput(string(buf, n));
}

void handleEngineControl(TerminalState &tab)
{
    char buf[512];
    ssize_t n;
    while ((n = read(tab.ctlFd, buf, sizeof(buf))) > 0)
        tab.ctlBuf.append(buf, n);
    if (n == 0)
        tab.isRunning = false; // EOF: the engine ended (exit, or its process crashed)

    size_t nl;
    while ((nl = tab.ctlBuf.find('\n')) != string::npos)
    {
        string msg = tab.ctlBuf.substr(0, nl);
        tab.ctlBuf.erase(0, nl + 1);
        handleEngineOutput(tab); // output sent before this message is shown first

        char type = msg.empty() ? '?' : msg[0];
        string arg = msg.size() > 2 ? msg.substr(2) : "";

        if (type == 'R' || type == 'M') // R <cwd>: ready for a command, M: wants more input
        {
            if (!tab.partial.empty()) // output without a final \n
            {
                tab.lines.push_back(tab.partial);
                tab.partial.clear();
            }
            tab.busy = false;
            tab.pgid = -1;
            tab.more = (type == 'M');
            if (type == 'R')
            {
                tab.cwd = arg;
                tab.setPrompt();
            }
        }
        else if (type == 'B') // B <pgid>: a foreground job is running
            tab.pgid = atoi(arg.c_str());
        else if (type == 'C') // clear
        {
            tab.lines.clear();
            tab.partial.clear();
        }
        else if (type == 'X') // exit
            tab.isRunning = false;
    }
}
