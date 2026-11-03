#pragma once
#include <string>
#include <vector>
#include "terminal_state.h"

struct Token
{
    std::string text;
    bool isOp = false;   // unquoted | < > >> &
    bool quoted = false; // the word had quotes or a \ escape: no wildcard expansion
};

// Splits a command line into tokens (quotes, \, $VAR, $?, $(...)).
// Returns false if the input is incomplete and needs another line.
bool tokenize(const std::string &s, TerminalState &ts, std::vector<Token> &out);

// Runs one command (no pipes inside), waits for it, returns what it printed.
// True for commands that would hand work to a shell (bash, sh, source, ./x.sh).
// Course rule: Terminator never runs a shell, so these are refused.
bool isShellInvocation(const std::string &cmd);

std::string captureOutput(const std::string &cmd, TerminalState &ts);
