#include "terminal_state.h"
#include "string_helper.h"
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>
#include <iostream>

using namespace std;

void TerminalState::appendInput(const string &s)
{
    input.insert(cursor, s);
    cursor += s.length();
}

void TerminalState::appendOutput(const string &data)
{
    for (char c : data)
    {
        if (c == '\n')
        {
            lines.push_back(partial);
            partial.clear();
        }
        else if (c == '\t')
            do
                partial += ' ';
            while (partial.size() % 8);
        else if ((unsigned char)c >= 32)
            partial += c;
    }
}

string TerminalState::getShortCwdPath()
{
    string cwdPath = this->cwd;
    string homePath = getenv("HOME");
    string pathText = cwdPath;
    if (startsWith(cwdPath, homePath))
        pathText = "~" + cwdPath.substr(homePath.length());
    return pathText;
}

void TerminalState::setPrompt()
{
    // setting up actual linux prompt
    auto *pw = getpwuid(getuid());
    string user = pw ? pw->pw_name : (getenv("USER") ? getenv("USER") : "user");

    // host name, e.g. "rupam"
    char host[256] = "localhost";
    gethostname(host, sizeof(host) - 1);

    string symbol = getuid() == 0 ? "#" : "$";

    string promptText = user + "@" + host + ":" + getShortCwdPath() + symbol + " ";
    this->prompt = promptText;
}
