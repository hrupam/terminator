#pragma once
#include "terminal_state.h"

// The engine of one tab: reads lines from the GUI and runs them.
// Thread mode runs it in a std::thread, process mode in a fork()ed child. Same code in both.
void engineMain(TerminalState termState);
