#pragma once
#include <vector>
#include "terminal_state.h"

extern std::vector<TerminalState> tabs;
extern int currentTab;
extern bool appRunning;

TerminalState &getCurrTab();
void newTab();
void closeTab(int tabIndex);
bool closeTabsBasedOnRunning(); // closes tabs that ran "exit"; true if any closed
void killTabProcesses(TerminalState &tab);
