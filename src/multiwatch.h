#pragma once
#include <string>
#include "terminal_state.h"

// Does this line start with the word multiWatch?
bool isMultiWatch(const std::string &line);

// multiWatch ["cmd1", "cmd2", ...]: runs every command in parallel, again 2 s after it finishes,
// and prints each piece of output in the spec's format, until Ctrl+C.
void runMultiWatch(const std::string &line, TerminalState &termState);
