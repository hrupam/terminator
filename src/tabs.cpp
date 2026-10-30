#include "tabs.h"
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>

using namespace std;

vector<TerminalState> tabs;
int currentTab = 0;
bool appRunning = true;

TerminalState &getCurrTab()
{
    return tabs[currentTab];
}

void newTab()
{
    TerminalState newTermState;
    newTermState.cwd = getCurrTab().cwd;
    newTermState.setPrompt();
    tabs.push_back(newTermState);
    currentTab = tabs.size() - 1;
}

void closeTab(int tabIndex)
{
    TerminalState &tab = tabs[tabIndex];
    killTabProcesses(tab);

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

void killTabProcesses(TerminalState &tab)
{
    if (tab.busy)
    {
        if (tab.pgid > 0)
            kill(-tab.pgid, SIGHUP);

        close(tab.outFd);
        for (pid_t chpid : tab.childPids)
            waitpid(chpid, nullptr, 0);
        tab.busy = false;
    }

    for (Job &job : tab.jobs)
    {
        kill(-job.pgid, SIGHUP);
        kill(-job.pgid, SIGCONT);
        close(job.outFd);
        for (pid_t pid : job.pids)
            waitpid(pid, nullptr, 0);
    }
    tab.jobs.clear();
}
