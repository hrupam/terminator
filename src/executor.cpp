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

    if (n == 1 && stages[0].words[0] == "exit")
    {
        termState.isRunning = false;
        return;
    }

    if (n == 1 && stages[0].words[0] == "cd")
    {
        termState.lastStatus = builtinCd(stages[0].words, termState);
        return;
    }

    if (n == 1 && stages[0].words[0] == "clear")
    {
        termState.lines.clear();
        termState.partial.clear();
        termState.lastStatus = 0;
        return;
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
}
