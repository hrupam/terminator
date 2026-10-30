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
            termState.lines.clear();
            termState.partial.clear();
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

            termState.appendOutput(job.cmd + "\n"); // bash prints the command it resumes
            termState.outFd = job.outFd;
            termState.childPids = job.pids;
            termState.pgid = job.pgid;
            termState.cmdText = job.cmd;
            termState.fgJobId = job.id;
            termState.busy = true;
            kill(-job.pgid, SIGCONT); // wake the whole group
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

    // Pipes
    int outPipe[2]; // final pipe
    if (pipe2(outPipe, O_CLOEXEC) < 0)
    {
        termState.lastStatus = 1;
        return;
    }
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
            close(outPipe[0]);
            close(outPipe[1]);
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

            if (i == 0)
            {
                int devNullFd = open("/dev/null", O_RDONLY);
                dup2(devNullFd, STDIN_FILENO);
                close(devNullFd);
            }
            else
                dup2(pipes[i - 1][0], STDIN_FILENO);

            if (i == n - 1)
                dup2(outPipe[1], STDOUT_FILENO);
            else
                dup2(pipes[i][1], STDOUT_FILENO);
            dup2(outPipe[1], STDERR_FILENO);

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
    close(outPipe[1]);
    for (auto &p : pipes)
    {
        close(p[0]);
        close(p[1]);
    }
    if (pids.empty()) // the very first fork failed
    {
        close(outPipe[0]);
        termState.appendOutput("terminator: fork failed\n");
        termState.lastStatus = 1;
        return;
    }
    termState.outFd = outPipe[0];
    termState.pgid = pgid;
    termState.childPids = pids;
    termState.busy = true;
    termState.cmdText = str;
    termState.fgJobId = 0;
}

void finishCommand(TerminalState &termState)
{
    close(termState.outFd); // close read end
    for (size_t i = 0; i < termState.childPids.size(); i++)
    {
        int status;
        waitpid(termState.childPids[i], &status, 0);
        if (i == termState.childPids.size() - 1)
        {
            if (WIFEXITED(status))
                termState.lastStatus = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                termState.lastStatus = 128 + WTERMSIG(status);
        }
    }

    if (!termState.partial.empty()) // output without a final \n
    {
        termState.lines.push_back(termState.partial);
        termState.partial.clear();
    }
    termState.outFd = -1;
    termState.childPids.clear();
    termState.pgid = -1;
    termState.busy = false;
    termState.fgJobId = 0;
}

// A background/stopped job's pipe hit EOF: reap it, print Done/Exit, drop it.
void finishJob(TerminalState &ts, size_t k)
{
    Job job = ts.jobs[k];
    ts.jobs.erase(ts.jobs.begin() + k);

    close(job.outFd);
    int status = 0;
    for (pid_t p : job.pids)
        waitpid(p, &status, 0); // status ends up as the last stage's

    string state;
    if (WIFEXITED(status))
        state = WEXITSTATUS(status) == 0 ? "Done" : "Exit " + to_string(WEXITSTATUS(status));
    else
        state = "Terminated";

    if (!ts.partial.empty())
        ts.appendOutput("\n"); // don't glue onto a half-printed line
    ts.appendOutput("[" + to_string(job.id) + "]   " + state + "                    " + job.cmd + "\n");
}
