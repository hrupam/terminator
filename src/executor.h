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

// Engine: parses and runs one line. For an external command it waits until the foreground
// job ends or is stopped (Ctrl+Z). Incomplete input is kept in termState.pending.
void runCommand(const std::string &str, TerminalState &termState);

// Engine: reaps finished background jobs and prints [n] Done.
void reportFinishedJobs(TerminalState &ts);

// Engine: kills and reaps every job (tab closing / exit).
void killAllJobs(TerminalState &ts);
