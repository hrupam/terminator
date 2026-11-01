#include "executor.h"
#include "lexer.h"
#include "string_helper.h"
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <csignal>
#include <algorithm>

using namespace std;

static int builtinCd(vector<string> &words, TerminalState &termState)
{
    if (words.size() > 2)
    {
        termState.appendOutput("terminator: cd: too many arguments\n");
        return 1;
    }
    string home = getenv("HOME") ? getenv("HOME") : "/";
    string arg = words.size() == 1 ? "~" : words[1];
    string path;
    if (arg == "~") // "cd" or "cd ~"
    {
        path = home;
    }
    else
    {
        if (startsWith(arg, "~/"))
        {
            path = home + arg.substr(1);
        }
        else if (startsWith(arg, "/"))
        {
            path = arg;
        }
        else
            path = termState.cwd + "/" + arg;
    }

    // check if path is valid directory
    error_code ec;
    if (!filesystem::is_directory(path, ec))
    {
        string errMsg = ec ? ec.message() : "Not a directory"; // for - cd /etc/passwd
        termState.appendOutput("terminator: cd: " + arg + ": " + errMsg + "\n");
        return 1;
    }

    if (access(path.c_str(), X_OK) != 0)
    {
        termState.appendOutput("terminator: cd: " + arg + ": Permission denied\n");
        return 1;
    }

    string resolvedPath = filesystem::canonical(path, ec).string();
    if (ec)
    {
        termState.appendOutput("terminator: cd: " + arg + ": " + ec.message() + "\n");
        return 1;
    }
    termState.cwd = resolvedPath;
    termState.setPrompt();

    return 0;
}

// bash's current job (+): the most recently stopped job; if none is stopped, the most recent job.
// skip: an index to ignore (used to find the previous job, marked -).
static int currentJob(const TerminalState &ts, int skip = -1)
{
    for (int i = (int)ts.jobs.size() - 1; i >= 0; i--)
        if (i != skip && ts.jobs[i].stopped)
            return i;
    for (int i = (int)ts.jobs.size() - 1; i >= 0; i--)
        if (i != skip)
            return i;
    return -1;
}

// "fg" -> the current job (last in the list); "fg %2" or "fg 2" -> job 2.
// Returns its index in ts.jobs, or -1.
static int findJob(TerminalState &ts, vector<string> &words)
{
    if (ts.jobs.empty())
        return -1;
    if (words.size() < 2)
        return currentJob(ts);
    string a = words[1];
    if (!a.empty() && a[0] == '%')
        a = a.substr(1);
    int id = atoi(a.c_str());
    for (size_t i = 0; i < ts.jobs.size(); i++)
        if (ts.jobs[i].id == id)
            return i;
    return -1;
}

// raw wait status -> $? value
static int decodeStatus(int st)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return 0;
}

// Engine: wait until the foreground job ends or is stopped (Ctrl+Z).
// WUNTRACED = waitpid also returns when a process is STOPPED, not only when it ends.
// We wait for our own pids only: waitpid(-1) would steal other tabs' children in thread mode.
static void waitForeground(TerminalState &termState, Job job)
{
    vector<pid_t> stopped;
    for (pid_t p : job.pids)
    {
        int st;
        if (waitpid(p, &st, WUNTRACED) < 0)
            continue;
        if (WIFSTOPPED(st))
            stopped.push_back(p); // frozen, still alive
        else if (p == job.lastPid)
            job.status = st;
    }

    if (stopped.empty()) // finished, or killed by Ctrl+C
    {
        termState.lastStatus = decodeStatus(job.status);
        return;
    }

    // stopped by Ctrl+Z: it becomes a job (a job resumed with fg keeps its number)
    job.pids = stopped;
    job.stopped = true;
    if (job.id == 0)
    {
        if (termState.jobs.empty())
            termState.nextJobId = 1; // like bash: numbering restarts when no jobs remain
        job.id = termState.nextJobId++;
    }
    termState.jobs.push_back(job);
    termState.appendOutput("[" + to_string(job.id) + "]+  Stopped                 " + job.cmd + "\n");
    termState.lastStatus = 148; // 128 + SIGTSTP (20), like bash
}

void runCommand(const string &str, TerminalState &termState)
{
    vector<Token> toks;
    if (!tokenize(str, termState, toks))
    {
        termState.pending = str;
        return;
    }
    if (toks.empty())
        return;

    vector<CommandStage> stages;
    CommandStage currStage;
    for (size_t i = 0; i < toks.size(); i++)
    {
        Token &t = toks[i];

        if (t.isOp && t.text == "|")
        {
            if (currStage.words.empty())
            {
                termState.appendOutput("terminator: syntax error near '|'\n");
                termState.lastStatus = 2;
                return;
            }
            stages.push_back(currStage);
            currStage = CommandStage();
        }
        else if (t.isOp) // < > >>
        {
            if (i + 1 >= toks.size() || toks[i + 1].isOp)
            {
                termState.appendOutput("terminator: syntax error: missing file after " + t.text + "\n");
                termState.lastStatus = 2;
                return;
            }
            if (t.text == "<")
                currStage.inFile = toks[i + 1].text;
            else
            {
                currStage.outFile = toks[i + 1].text;
                currStage.isAppendMode = (t.text == ">>");
            }
            i++; // skip the filename
        }
        else // a normal word
            currStage.words.push_back(t.text);
    }
    if (currStage.words.empty() && !stages.empty())
    {
        termState.appendOutput("terminator: syntax error near '|'\n");
        termState.lastStatus = 2;
        return;
    }
    stages.push_back(currStage);

    if (stages.empty() || stages[0].words.empty())
        return;

    int n = stages.size();

    // only one stage and special commands
    if (n == 1)
    {
        if (stages[0].words[0] == "exit")
        {
            termState.isRunning = false;
            return;
        }

        if (stages[0].words[0] == "cd")
        {
            termState.lastStatus = builtinCd(stages[0].words, termState);
            return;
        }

        if (stages[0].words[0] == "clear")
        {
            termState.sendCtl("C"); // the screen lives in the GUI copy
            termState.lastStatus = 0;
            return;
        }

        if (stages[0].words[0] == "jobs")
        {
            int cur = currentJob(termState);
            int prev = currentJob(termState, cur);

            // the list is in recency order; bash prints by job number
            vector<int> order(termState.jobs.size());
            for (size_t i = 0; i < order.size(); i++)
                order[i] = i;
            sort(order.begin(), order.end(), [&](int a, int b)
                 { return termState.jobs[a].id < termState.jobs[b].id; });

            for (int i : order)
            {
                Job &j = termState.jobs[i];
                char mark = (i == cur) ? '+' : (i == prev) ? '-'
                                                           : ' ';
                termState.appendOutput("[" + to_string(j.id) + "]" + mark + "  " +
                                       (j.stopped ? "Stopped" : "Running") +
                                       "                 " + j.cmd + (j.stopped ? "" : " &") + "\n");
            }
            termState.lastStatus = 0;
            return;
        }

        if (stages[0].words[0] == "fg")
        {
            int k = findJob(termState, stages[0].words);
            if (k < 0)
            {
                termState.appendOutput("terminator: fg: no such job\n");
                termState.lastStatus = 1;
                return;
            }
            Job job = termState.jobs[k];
            termState.jobs.erase(termState.jobs.begin() + k);

            termState.appendOutput(job.cmd + "\n");          // bash prints the command it resumes
            termState.sendCtl("B " + to_string(job.pgid)); // GUI: Ctrl+C / Ctrl+Z now go to this group
            job.stopped = false;
            kill(-job.pgid, SIGCONT); // wake the whole group
            waitForeground(termState, job);
            return;
        }

        if (stages[0].words[0] == "bg")
        {
            int k = findJob(termState, stages[0].words);
            if (k < 0)
            {
                termState.appendOutput("terminator: bg: no such job\n");
                termState.lastStatus = 1;
                return;
            }
            Job &job = termState.jobs[k];
            if (!job.stopped)
                termState.appendOutput("terminator: bg: job " + to_string(job.id) + " already in background\n");
            else
            {
                job.stopped = false;
                kill(-job.pgid, SIGCONT);
                termState.appendOutput("[" + to_string(job.id) + "]+ " + job.cmd + " &\n");
            }
            termState.lastStatus = 0;
            return;
        }
    }

    auto resolve = [&](const string &f)
    { return f[0] == '/' ? f : termState.cwd + "/" + f; };
    vector<vector<char *>> argvs(n);
    vector<string> notFound(n), inPaths(n), outPaths(n), openFail(n);

    for (int i = 0; i < n; i++)
    {
        CommandStage &stage = stages[i];
        for (string &w : stage.words)
            argvs[i].push_back(w.data());
        argvs[i].push_back(nullptr);

        notFound[i] = "terminator: " + stage.words[0] + ": command not found\n";
        if (!stage.inFile.empty())
            inPaths[i] = resolve(stage.inFile);
        if (!stage.outFile.empty())
            outPaths[i] = resolve(stage.outFile);
        openFail[i] = "terminator: cannot open file\n";

        if (!inPaths[i].empty() && access(inPaths[i].c_str(), R_OK) != 0)
        {
            termState.appendOutput("terminator: " + stage.inFile + ": " + strerror(errno) + "\n");
            termState.lastStatus = 1;
            return;
        }
    }

    string badDir = "terminator: cannot enter " + termState.cwd + "\n";

    // Pipes between stages (the last stage writes straight into the tab's out pipe)
    vector<vector<int>> pipes(n - 1, vector<int>(2));
    for (int i = 0; i < n - 1; i++)
    {
        int *fd = pipes[i].data();

        if (pipe2(fd, O_CLOEXEC) < 0)
        {

            for (int j = 0; j < i; j++)
            {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            termState.lastStatus = 1;
            return;
        }
    }

    // Fork
    vector<pid_t> pids;
    pid_t pgid = 0;
    for (int i = 0; i < n; i++)
    {
        pid_t pid = fork();
        if (pid < 0)
            break;
        if (pid == 0)
        {
            setpgid(0, pgid);
            // ignored signals stay ignored after exec: reset them, like bash does
            for (int sig : {SIGPIPE, SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU})
                signal(sig, SIG_DFL);

            if (i == 0)
            {
                int devNullFd = open("/dev/null", O_RDONLY);
                dup2(devNullFd, STDIN_FILENO);
                close(devNullFd);
            }
            else
                dup2(pipes[i - 1][0], STDIN_FILENO);

            if (i == n - 1)
                dup2(termState.outFd, STDOUT_FILENO);
            else
                dup2(pipes[i][1], STDOUT_FILENO);
            dup2(termState.outFd, STDERR_FILENO);

            // redirection
            if (!inPaths[i].empty())
            {
                int inFileFd = open(inPaths[i].c_str(), O_RDONLY);
                if (inFileFd < 0)
                {
                    write(STDERR_FILENO, openFail[i].c_str(), openFail[i].size());
                    _exit(1);
                }
                dup2(inFileFd, STDIN_FILENO);
                close(inFileFd);
            }

            if (!outPaths[i].empty())
            {
                auto flags = O_WRONLY | O_CREAT | (stages[i].isAppendMode ? O_APPEND : O_TRUNC);
                int outFileFd = open(outPaths[i].c_str(), flags, 0644);

                if (outFileFd < 0)
                {
                    write(STDERR_FILENO, openFail[i].c_str(), openFail[i].size());
                    _exit(1);
                }
                dup2(outFileFd, STDOUT_FILENO);
                close(outFileFd);
            }

            if (chdir(termState.cwd.c_str()) < 0)
            {
                write(STDERR_FILENO, badDir.c_str(), badDir.size());
                _exit(1);
            }
            execvp(argvs[i][0], argvs[i].data());
            write(STDERR_FILENO, notFound[i].c_str(), notFound[i].size());
            _exit(127);
        }
        else
        {
            if (pgid == 0)
                pgid = pid;
            setpgid(pid, pgid);
            pids.push_back(pid);
        }
    }

    // parent cleanup
    for (auto &p : pipes)
    {
        close(p[0]);
        close(p[1]);
    }
    if (pids.empty()) // the very first fork failed
    {
        termState.appendOutput("terminator: fork failed\n");
        termState.lastStatus = 1;
        return;
    }
    Job fg;
    fg.id = 0;
    fg.pgid = pgid;
    fg.pids = pids;
    fg.lastPid = pids.back();
    fg.status = 0;
    fg.cmd = str;
    fg.stopped = false;
    termState.sendCtl("B " + to_string(pgid)); // GUI: send Ctrl+C / Ctrl+Z to this group
    waitForeground(termState, fg);             // the engine waits; the GUI keeps running
}

// Engine: reap background/stopped jobs that have ended, print [n] Done / Exit / Terminated.
void reportFinishedJobs(TerminalState &ts)
{
    for (size_t k = 0; k < ts.jobs.size();)
    {
        Job &j = ts.jobs[k];
        for (size_t i = 0; i < j.pids.size();)
        {
            int st;
            pid_t r = waitpid(j.pids[i], &st, WNOHANG); // 0 = still running or stopped
            if (r == j.pids[i])
            {
                if (r == j.lastPid)
                    j.status = st;
                j.pids.erase(j.pids.begin() + i);
            }
            else
                i++;
        }
        if (!j.pids.empty())
        {
            k++;
            continue;
        }
        string state;
        if (WIFEXITED(j.status))
            state = WEXITSTATUS(j.status) == 0 ? "Done" : "Exit " + to_string(WEXITSTATUS(j.status));
        else
            state = "Terminated";
        ts.appendOutput("[" + to_string(j.id) + "]   " + state + "                    " + j.cmd + "\n");
        ts.jobs.erase(ts.jobs.begin() + k);
    }
}

// Engine: tab closing or exit: kill and reap every job.
void killAllJobs(TerminalState &ts)
{
    for (Job &j : ts.jobs)
    {
        kill(-j.pgid, SIGHUP);
        kill(-j.pgid, SIGCONT); // a stopped process only reacts to SIGHUP once continued
        for (pid_t p : j.pids)
            waitpid(p, nullptr, 0);
    }
    ts.jobs.clear();
}
