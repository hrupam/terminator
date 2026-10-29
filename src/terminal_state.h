#pragma once
#include <string>
#include <vector>
#include <sys/types.h>

// Everything one tab needs: its screen text, input line, cwd and running command.
class TerminalState
{
public:
    std::vector<std::string> lines; /* finished output lines */
    std::string partial;            /* output line still being written (no \n yet) */
    std::string input;              /* the line being typed */
    std::string pending;            // unfinished input waiting for more lines, "" if none
    int cursor = 0;                 /* length and cursor position in input */
    int scroll = 0;                 /* how many lines scrolled back, 0 = bottom */
    std::string prompt;             /* "user@terminator: path> " */
    std::string cwd;                /* shell's current directory */
    int lastStatus = 0;             /* $? : exit status of the last command */
    bool isRunning = true;          /* running state of the terminal */
    int outFd = -1;                 // read end of the running command's pipe, -1 if none
    std::vector<pid_t> childPids;   // every process in the running pipeline
    pid_t pgid = -1;                // their process group (= first child's pid)
    bool busy = false;              // a command is running: don't draw the prompt

    void appendInput(const std::string &s);
    void appendOutput(const std::string &data);
    std::string getShortCwdPath();
    void setPrompt();
};
