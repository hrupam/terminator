#include "gui.h"
#include "tabs.h"
#include "events.h"
#include "executor.h"
#include <climits>
#include <cerrno>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

using namespace std;

int main(void)
{
    if (!initGui())
        return 1;

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
        vector<int> fdTab; // which tab each fd belongs to
        for (size_t i = 0; i < tabs.size(); i++)
        {
            if (tabs[i].outFd != -1)
            {
                fds.push_back({tabs[i].outFd, POLLIN, 0});
                fdTab.push_back(i);
            }
            for (Job &j : tabs[i].jobs) // background (and stopped) jobs
            {
                fds.push_back({j.outFd, POLLIN, 0});
                fdTab.push_back(i);
            }
        }

        poll(fds.data(), fds.size(), -1);

        bool changed = false;
        for (size_t k = 1; k < fds.size(); k++)
        {
            if (!(fds[k].revents & (POLLIN | POLLHUP)))
                continue;
            TerminalState &tab = tabs[fdTab[k - 1]];
            int fd = fds[k].fd;
            char buffer[4096];

            if (fd == tab.outFd) // the foreground command
            {
                ssize_t n = read(fd, buffer, sizeof(buffer));
                if (n > 0)
                    tab.appendOutput(string(buffer, n));
                else if (n == 0 || errno != EINTR)
                    finishCommand(tab);
            }
            else // a job: find it by its fd
            {
                size_t j = 0;
                while (j < tab.jobs.size() && tab.jobs[j].outFd != fd)
                    j++;
                if (j == tab.jobs.size())
                    continue; // already gone
                ssize_t n = read(fd, buffer, sizeof(buffer));
                if (n > 0)
                    tab.appendOutput(string(buffer, n));
                else if (n == 0 || errno != EINTR)
                    finishJob(tab, j);
            }
            changed = true;
        }
        if (changed)
            redraw();
    }

    // Kill all tab processes before closing the terminal
    for (TerminalState &tab : tabs)
        killTabProcesses(tab);

    closeTerminal();

    return 0;
}
