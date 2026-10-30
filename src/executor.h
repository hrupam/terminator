#pragma once
#include <string>
#include <vector>
#include "terminal_state.h"

// One program in a pipeline, with its own < and > redirections.
class CommandStage
{
public:
    std::vector<std::string> words;
    std::string inFile;
    std::string outFile;
    bool isAppendMode = false;
};

// Parses and starts a command line in this tab (returns at once; output arrives via outFd).
void runCommand(const std::string &str, TerminalState &termState);

// Called when outFd hits EOF: reaps the children and sets $?.
void finishCommand(TerminalState &termState);

void finishJob(TerminalState &ts, size_t k);
