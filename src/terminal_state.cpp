#include "terminal_state.h"
#include "string_helper.h"
#include <cstdlib>
#include <unistd.h>
#include <pwd.h>
#include <cerrno>

using namespace std;

void TerminalState::appendInput(const string &s)
{
    input.insert(cursor, s);
    cursor += s.length();
}

// write() may write less than asked; keep going until everything is out.
static void writeAll(int fd, const string &s)
{
    size_t done = 0;
    while (done < s.size())
    {
        ssize_t n = write(fd, s.data() + done, s.size() - done);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return; // the other side is gone
        }
        done += n;
    }
}

void TerminalState::sendCtl(const string &msg)
{
    writeAll(ctlFd, msg + "\n"); // one message per line
}

void TerminalState::appendOutput(const string &data)
{
    if (isEngine) // engine copy: the screen is in the GUI, so send the text through the out pipe
    {
        writeAll(outFd, data);
        return;
    }
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
