#include "engine.h"
#include "executor.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace std;

void engineMain(TerminalState termState)
{
    termState.sendCtl("R " + termState.cwd); // first prompt

    FILE *in = fdopen(termState.inFd, "r");
    char *buf = nullptr;
    size_t cap = 0;

    // getline blocks until the GUI sends a message; it returns -1 when the GUI closes the pipe
    while (termState.isRunning && getline(&buf, &cap, in) > 0)
    {
        string msg = buf;
        if (!msg.empty() && msg.back() == '\n')
            msg.pop_back();

        if (msg.compare(0, 2, "L ") == 0) // "L <line>": a typed line
        {
            string line = msg.substr(2);
            string full = termState.pending.empty() ? line : termState.pending + "\n" + line;
            termState.pending.clear();   // runCommand sets it again if still incomplete
            runCommand(full, termState); // returns when the command has finished or stopped
        }
        else if (msg == "K") // Ctrl+C at the prompt: drop the half-typed command
        {
            termState.pending.clear();
            termState.lastStatus = 130;
        }

        reportFinishedJobs(termState); // [n] Done for background jobs, like bash

        // tell the GUI what to show next
        if (!termState.isRunning)
            termState.sendCtl("X"); // exit
        else if (!termState.pending.empty())
            termState.sendCtl("M"); // more input: "> "
        else
            termState.sendCtl("R " + termState.cwd); // ready: fresh prompt
    }

    free(buf);
    killAllJobs(termState); // no stopped/background process may outlive the tab
    fclose(in);             // closes inFd
    close(termState.outFd);
    close(termState.ctlFd); // the GUI sees EOF on ctl: this engine is finished
}
