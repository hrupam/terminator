#include "multiwatch.h"
#include "executor.h"
#include <cerrno>
#include <csignal>
#include <ctime>
#include <vector>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

using namespace std;

static const string DASHES(52, '-'); // the spec's separator: exactly 52 dashes

bool isMultiWatch(const string &line)
{
    size_t i = line.find_first_not_of(" \t");
    return i != string::npos && line.compare(i, 10, "multiWatch") == 0;
}

// ["date +%T", "ls | wc -l"]  ->  {date +%T, ls | wc -l}.  Empty result = syntax error.
static vector<string> parseCommandList(const string &line)
{
    vector<string> cmds;
    size_t open = line.find('['), close = line.rfind(']');
    if (open == string::npos || close == string::npos || close < open)
        return cmds;
    size_t i = open + 1;
    while (i < close)
    {
        size_t q1 = line.find('"', i);
        if (q1 == string::npos || q1 > close)
            break;
        size_t q2 = line.find('"', q1 + 1);
        if (q2 == string::npos || q2 > close)
            return {}; // unclosed quote
        cmds.push_back(line.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return cmds;
}

// One command being watched.
struct Watch
{
    string cmd;
    pid_t pid = -1;    // the runner process, -1 = not running
    int fd = -1;       // read end of its output pipe
    time_t nextRun = 0; // when to start it again
};

// Starts one run of w.cmd: a child process that runs it with the normal runCommand,
// its stdout/stderr going into a pipe that only this command uses.
static void startRun(Watch &w, TerminalState &termState)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return;
    pid_t pid = fork();
    if (pid == 0) // runner: run the command line, exit with its status
    {
        TerminalState sub = termState;
        sub.outFd = p[1];        // its output goes into this command's pipe
        sub.ctlFd = -1;          // nothing to tell the GUI
        sub.newGroups = false;   // stay in the watcher's process group, so Ctrl+C reaches it
        sub.jobs.clear();
        runCommand(w.cmd, sub);
        _exit(sub.lastStatus);
    }
    close(p[1]); // watcher only reads
    if (pid < 0)
    {
        close(p[0]);
        return;
    }
    w.pid = pid;
    w.fd = p[0];
}

// The watcher: runs forever, until Ctrl+C kills its whole process group.
static void watchLoop(vector<Watch> &watches, TerminalState &termState)
{
    while (true)
    {
        time_t now = time(nullptr);
        for (Watch &w : watches) // start every command that is due
            if (w.pid == -1 && now >= w.nextRun)
                startRun(w, termState);

        // wait for output from any running command, or until the next restart is due
        vector<pollfd> fds;
        vector<Watch *> owner;
        int timeoutMs = -1;
        for (Watch &w : watches)
        {
            if (w.fd != -1)
            {
                fds.push_back({w.fd, POLLIN, 0});
                owner.push_back(&w);
            }
            else
            {
                int ms = (int)(w.nextRun - now) * 1000;
                if (ms < 0)
                    ms = 0;
                if (timeoutMs == -1 || ms < timeoutMs)
                    timeoutMs = ms;
            }
        }
        if (poll(fds.data(), fds.size(), timeoutMs) < 0 && errno != EINTR)
            return;

        for (size_t k = 0; k < fds.size(); k++)
        {
            if (!(fds[k].revents & (POLLIN | POLLHUP)))
                continue;
            Watch &w = *owner[k];
            char buf[4096];
            ssize_t n = read(w.fd, buf, sizeof(buf));
            if (n > 0) // a piece of output: print it in the spec's format
            {
                string out(buf, n);
                if (out.back() != '\n')
                    out += '\n';
                termState.appendOutput("\"" + w.cmd + "\", " + to_string(time(nullptr)) + ":\n" +
                                       DASHES + "\n" + out + DASHES + "\n");
            }
            else if (n == 0 || errno != EINTR) // this run has finished
            {
                close(w.fd);
                waitpid(w.pid, nullptr, 0);
                w.fd = -1;
                w.pid = -1;
                w.nextRun = time(nullptr) + 2; // like watch: again in 2 seconds
            }
        }
    }
}

void runMultiWatch(const string &line, TerminalState &termState)
{
    vector<string> cmds = parseCommandList(line);
    if (cmds.empty())
    {
        termState.appendOutput("terminator: multiWatch: usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n");
        termState.lastStatus = 2;
        return;
    }

    // One watcher process does all the work, in its own process group. Every command it starts
    // stays in that group, so the GUI's Ctrl+C (kill(-pgid, SIGINT)) stops all of them at once.
    pid_t pid = fork();
    if (pid == 0)
    {
        setpgid(0, 0);
        for (int sig : {SIGPIPE, SIGINT, SIGQUIT, SIGTSTP})
            signal(sig, SIG_DFL);
        vector<Watch> watches(cmds.size());
        for (size_t i = 0; i < cmds.size(); i++)
            watches[i].cmd = cmds[i];
        watchLoop(watches, termState);
        _exit(1);
    }
    if (pid < 0)
    {
        termState.appendOutput("terminator: multiWatch: fork failed\n");
        termState.lastStatus = 1;
        return;
    }
    setpgid(pid, pid);

    // the engine treats the watcher like any foreground command: Ctrl+C ends it, $? = 130
    termState.sendCtl("B " + to_string(pid));
    int st = 0;
    while (true)
    {
        if (waitpid(pid, &st, WUNTRACED) < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (WIFSTOPPED(st)) // Ctrl+Z is not supported for multiWatch: keep it running
        {
            kill(-pid, SIGCONT);
            continue;
        }
        break; // ended (Ctrl+C)
    }
    termState.lastStatus = WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
}
