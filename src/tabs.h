#pragma once
#include <string>
#include <vector>
#include "terminal_state.h"

extern std::vector<TerminalState> tabs; // the GUI copies
extern int currentTab;
extern bool appRunning;
extern bool useProcess; // false = thread mode (default), true = process mode

TerminalState &getCurrTab();
void newTab(const std::string &cwd); // starts the tab's engine as a thread or a process
void closeTab(int tabIndex);
bool closeTabsBasedOnRunning(); // closes tabs whose engine exited; true if any closed
void shutdownAllTabs();         // at program exit

void sendToEngine(TerminalState &tab, const std::string &msg);
void handleEngineOutput(TerminalState &tab);  // the tab's out pipe is readable
void handleEngineControl(TerminalState &tab); // the tab's ctl pipe is readable
