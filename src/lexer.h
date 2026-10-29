#pragma once
#include <string>
#include <vector>
#include "terminal_state.h"

struct Token
{
    std::string text;
    bool isOp = false; // unquoted | < > >>
};

// Splits a command line into tokens (quotes, \, $VAR, $?, $(...)).
// Returns false if the input is incomplete and needs another line.
bool tokenize(const std::string &s, TerminalState &ts, std::vector<Token> &out);

// Runs one command (no pipes inside), waits for it, returns what it printed.
std::string captureOutput(const std::string &cmd, TerminalState &ts);
