#include "gui.h"
#include "tabs.h"
#include "events.h"
#include "executor.h"
#include <climits>
#include <csignal>
#include <strings.h>
#include <cerrno>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

using namespace std;

int main(int argc, char **argv)
{
    // ./terminator                  -> thread mode  (one engine thread per tab)
    // ./terminator useProcess=True  -> process mode (one engine process per tab)
    if (argc > 1 && strcasecmp(argv[1], "useProcess=True") == 0)
        useProcess = true;

    // a write to a pipe whose reader died raises SIGPIPE, which would kill the GUI; children reset it
    signal(SIGPIPE, SIG_IGN);

    if (!initGui())
        return 1;
    XStoreName(disp, win, useProcess ? "Terminator (process mode)" : "Terminator (thread mode)");

    int xfd = ConnectionNumber(disp);
    fcntl(xfd, F_SETFD, FD_CLOEXEC);

    // getting initial working directory
    char cwdbuf[PATH_MAX];
    newTab(getcwd(cwdbuf, sizeof(cwdbuf)) ? cwdbuf : "/");
    if (tabs.empty())
        return 1;

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
        vector<int> fdTab; // which tab each fd belongs to
        for (size_t i = 0; i < tabs.size(); i++)
        {
            if (!tabs[i].isRunning)
                continue;
            fds.push_back({tabs[i].outFd, POLLIN, 0}); // output of the tab's engine and commands
            fdTab.push_back(i);
            fds.push_back({tabs[i].ctlFd, POLLIN, 0}); // control messages of the tab's engine
            fdTab.push_back(i);
        }

        poll(fds.data(), fds.size(), -1);

        bool changed = false;
        for (size_t k = 1; k < fds.size(); k++)
        {
            if (!(fds[k].revents & (POLLIN | POLLHUP)))
                continue;
            TerminalState &tab = tabs[fdTab[k - 1]];
            if (fds[k].fd == tab.outFd)
                handleEngineOutput(tab);
            else
                handleEngineControl(tab);
            changed = true;
        }
        if (changed)
            redraw();
    }

    // stop every tab's engine (and its jobs) before closing the window
    shutdownAllTabs();

    closeTerminal();

    return 0;
}
