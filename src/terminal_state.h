#pragma once
#include <string>
#include <vector>
#include <sys/types.h>

// A stopped or background command: a process group this tab's engine still owns.
struct Job
{
    int id;                  // the [1], [2] number
    pid_t pgid;              // its process group
    std::vector<pid_t> pids; // processes in it that are not reaped yet
    pid_t lastPid;           // last stage of the pipeline: its exit status is the job's status
    int status;              // raw wait status of lastPid, once it has ended
    std::string cmd;         // command text, for the jobs list
    bool stopped;            // true = Stopped, false = Running in background
};

// One tab. Every tab has TWO copies of this class:
//   GUI copy    (isEngine = false): screen lines, input line, prompt, busy  -> drawn by the GUI
//   engine copy (isEngine = true):  cwd, $?, jobs, pending                  -> runs the commands
// Thread mode: the engine copy lives in the tab's thread.
// Process mode: the engine copy lives in the tab's fork()ed process.
// The two copies talk only through 3 pipes; inFd/outFd/ctlFd are THIS copy's ends.
class TerminalState
{
public:
    std::vector<std::string> lines; /* finished output lines */
    std::string partial;            /* output line still being written (no \n yet) */
    std::string input;              /* the line being typed */
    std::string pending;            // engine: unfinished multiline input waiting for more lines
    int cursor = 0;                 /* length and cursor position in input */
    int scroll = 0;                 /* how many lines scrolled back, 0 = bottom */
    std::string prompt;             /* "user@host:path$ " */
    std::string cwd;                /* this tab's current directory */
    int lastStatus = 0;             /* $? : exit status of the last command */
    bool isRunning = true;          /* false after "exit" */
    pid_t pgid = -1;                // GUI: process group of the foreground job (for Ctrl+C / Ctrl+Z)
    bool busy = false;              // GUI: a command is running: don't draw the prompt
    bool more = false;              // GUI: the engine wants more input: draw "> " instead of the prompt

    std::vector<Job> jobs; // engine: stopped / background jobs of this tab
    int nextJobId = 1;

    bool isEngine = false; // which copy this is
    int inFd = -1;         // in pipe  (typed lines):      GUI writes,  engine reads
    int outFd = -1;        // out pipe (command output):   engine and its commands write, GUI reads
    int ctlFd = -1;        // ctl pipe (R, M, B, C, X):    engine writes, GUI reads
    std::string ctlBuf;    // GUI: ctl bytes read but not yet a whole message
    pid_t enginePid = -1;  // GUI, process mode: the tab process

    /* HISTORY */
    int histPos = -1;      // GUI: index into history while browsing with Up/Down; -1 = not browsing
    std::string histDraft; // GUI: the line being typed before Up was pressed

    void appendInput(const std::string &s);
    void appendOutput(const std::string &data); // engine copy: sends the text to the GUI
    void sendCtl(const std::string &msg);       // engine copy: one control message
    std::string getShortCwdPath();
    void setPrompt();
};
