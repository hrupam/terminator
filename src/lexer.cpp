#include "lexer.h"
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

using namespace std;

string captureOutput(const string &cmd, TerminalState &ts)
{
    vector<Token> toks;
    if (!tokenize(cmd, ts, toks) || toks.empty())
        return "";

    vector<string> words;
    for (Token &t : toks)
        words.push_back(t.text);
    vector<char *> argv;
    for (string &w : words)
        argv.push_back(w.data());
    argv.push_back(nullptr);
    string notFound = "terminator: " + words[0] + ": command not found\n";

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return "";

    pid_t pid = fork();
    if (pid == 0) // child
    {
        int dn = open("/dev/null", O_RDONLY);
        dup2(dn, STDIN_FILENO);
        close(dn);
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        chdir(ts.cwd.c_str());
        execvp(argv[0], argv.data());
        write(STDERR_FILENO, notFound.c_str(), notFound.size());
        _exit(127);
    }

    close(p[1]); // MUST: otherwise read() below never returns 0

    string out;
    char buf[4096];
    ssize_t r;
    while ((r = read(p[0], buf, sizeof(buf))) > 0)
        out.append(buf, r);
    close(p[0]);

    if (pid > 0)
    {
        int st;
        waitpid(pid, &st, 0);
        ts.lastStatus = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }

    while (!out.empty() && out.back() == '\n')
        out.pop_back(); // bash removes trailing newlines
    return out;
}

// i points at '$'. Appends the expansion to cur and advances i.
// static: only used inside this file.
static bool expandDollar(const string &s, size_t &i, TerminalState &ts, string &cur)
{
    size_t n = s.size();
    i++; // skip '$'
    if (i < n && s[i] == '?')
    {
        cur += to_string(ts.lastStatus);
        i++;
    }
    else if (i < n && s[i] == '(')
    {
        int depth = 0;
        size_t j = i;
        for (; j < n; j++)
        {
            if (s[j] == '(')
                depth++;
            else if (s[j] == ')')
            {
                depth--;
                if (depth == 0)
                    break;
            }
        }
        if (j >= n)
            return false; // unclosed $( : multiline prompt, like an unclosed quote

        string inner = s.substr(i + 1, j - i - 1);
        cur += captureOutput(inner, ts);
        i = j + 1; // continue after the ')'
    }
    else if (i < n && s[i] == '{')
    {
        size_t j = s.find('}', i);
        if (j == string::npos)
            return false; // unclosed ${
        const char *v = getenv(s.substr(i + 1, j - i - 1).c_str());
        if (v)
            cur += v;
        i = j + 1;
    }
    else if (i < n && (isalpha(s[i]) || s[i] == '_'))
    {
        size_t j = i;
        while (j < n && (isalnum(s[j]) || s[j] == '_'))
            j++;
        const char *v = getenv(s.substr(i, j - i).c_str());
        if (v)
            cur += v;
        i = j;
    }
    else
        cur += '$'; // lone $ stays literal: echo 5$
    return true;
}

bool tokenize(const string &s, TerminalState &ts, vector<Token> &out)
{
    out.clear();
    string cur;
    bool inWord = false;
    size_t i = 0, n = s.size();

    auto endWord = [&]()
    {
        if (inWord)
            out.push_back({cur, false});
        cur.clear();
        inWord = false;
    };

    while (i < n)
    {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n')
        {
            endWord();
            i++;
        }
        else if (c == '|' || c == '<')
        {
            endWord();
            out.push_back({string(1, c), true});
            i++;
        }
        else if (c == '>')
        {
            endWord();
            if (i + 1 < n && s[i + 1] == '>')
            {
                out.push_back({">>", true});
                i += 2;
            }
            else
            {
                out.push_back({">", true});
                i++;
            }
        }
        else if (c == '\\')
        {
            if (i + 1 >= n)
                return false;     // trailing \ : wants another line
            if (s[i + 1] != '\n') // "\<newline>" = line continuation, adds nothing
            {
                cur += s[i + 1];
                inWord = true;
            }
            i += 2;
        }
        else if (c == '\'')
        {
            size_t j = s.find('\'', i + 1);
            if (j == string::npos)
                return false; // unclosed '
            cur += s.substr(i + 1, j - i - 1);
            inWord = true;
            i = j + 1;
        }
        else if (c == '"')
        {
            inWord = true;
            i++;
            while (true)
            {
                if (i >= n)
                    return false; // unclosed "
                char d = s[i];
                if (d == '"')
                {
                    i++;
                    break;
                }
                if (d == '\\' && i + 1 < n && strchr("\"\\$\n", s[i + 1]))
                {
                    if (s[i + 1] != '\n')
                        cur += s[i + 1];
                    i += 2;
                }
                else if (d == '$')
                {
                    if (!expandDollar(s, i, ts, cur))
                        return false;
                }
                else
                {
                    cur += d;
                    i++;
                }
            }
        }
        else if (c == '$')
        {
            if (!expandDollar(s, i, ts, cur))
                return false;
            if (!cur.empty())
                inWord = true; // unquoted empty $X gives no argument, like bash
        }
        else
        {
            cur += c;
            inWord = true;
            i++;
        }
    }
    endWord();

    if (!out.empty() && out.back().isOp && out.back().text == "|")
        return false; // trailing | : wants another line
    return true;
}
