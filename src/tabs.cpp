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
