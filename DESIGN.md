# Terminator: design document

This document explains how Terminator is built: the X11 GUI and its event loop, thread mode and process mode, process and job management, the pipes between parts, signal handling, the data structures for history and completion, and the main design decisions.

## 1. Overview

Terminator has two halves:

- **The GUI** (one thread, `main.cpp`, `gui.cpp`, `events.cpp`, `tabs.cpp`). It owns the X11 window, all tabs, the text on screen, the line being typed, history and completion. It never runs a command.
- **One engine per tab** (`engine.cpp`, `executor.cpp`, `lexer.cpp`, `multiwatch.cpp`). It receives a finished command line, parses it, forks and execs the programs, waits for them and keeps the tab's shell state: current directory, `$?` and jobs.

The two halves talk **only through three pipes per tab**. Because of this, the engine code is the same whether it runs as a thread (thread mode) or as a forked process (process mode); only `newTab()` differs.

```
                       GUI thread (one per Terminator)
   +------------------------------------------------------------------+
   |  X11 window, tab bar, text buffers, input line, history, trie    |
   |  poll(): X connection fd + every tab's out and ctl pipes         |
   +------------------------------------------------------------------+
        | in (L/K)     ^ out (raw text)  ^ ctl (R/M/B/C/X)
        v              |                 |
   +-----------------------------+   +-----------------------------+
   | tab 1 engine                |   | tab 2 engine                |  ...
   | thread or process           |   | thread or process           |
   | cwd, $?, jobs, pending line |   | cwd, $?, jobs, pending line |
   +-----------------------------+   +-----------------------------+
        | fork + execvp                     | fork + execvp
        v                                   v
   ls | grep | sort  (own process group)    sleep 100 (own process group)
   stdout/stderr -> the tab's out pipe
```

## 2. The X11 GUI and the event loop

`initGui()` opens the display, creates one window, loads the first available font of `12x24`, `10x20`, `9x15` and `fixed`, allocates the colours (background `gray12`, text `gray85`, active tab `SteelBlue`, inactive `gray40`), and interns the atoms `WM_DELETE_WINDOW`, `CLIPBOARD`, `UTF8_STRING`, `TARGETS` and `TERMINATOR_PASTE`. The window title shows the mode.

`main()` runs a single `poll()` loop:

```
fds = { ConnectionNumber(display) }
for each tab: fds += { tab.outFd, tab.ctlFd }
poll(fds, -1)
while XPending(display): XNextEvent -> handleEvent
for each tab with data on outFd: handleEngineOutput  (append text, redraw)
for each tab with data on ctlFd: handleEngineControl (parse messages)
close tabs whose engine has exited
```

The GUI never blocks on anything other than `poll()`: the GUI ends of `out` and `ctl` are `O_NONBLOCK`, and output is read in chunks until `EAGAIN`. A long or endless command (`yes`, `ping`) therefore never freezes the window, other tabs or the hotkeys.

Drawing (`redraw()`) is done from the GUI's copy of the tab's state: the tab bar (`n: path`, `*` while busy, `x` to close, `[+]` for a new tab), then the visible part of `lines` given the scroll offset, then the prompt (or `> ` while a multiline command is being typed) with the input and the cursor. Text that arrives without a newline is kept in `partial` until the newline comes.

Event handling (`events.cpp`):

- `KeyPress`: if a completion list is shown, a digit picks from it. Otherwise `findAction()` looks the key up in the binding table and `handleAction()` runs the action. Anything else edits the input (typing, BackSpace, ←/→), and Return sends the line to the engine.
- `ButtonPress`: clicks on the tab bar (switch, `x`, `[+]`) and the wheel (buttons 4/5).
- `ConfigureNotify` and `Expose`: redraw at the new size.
- `SelectionRequest` and `SelectionNotify`: the clipboard (section 9).
- `ClientMessage` with `WM_DELETE_WINDOW`: quit.

## 3. Per-tab state

`TerminalState` (terminal_state.h) is one struct, but each tab has **two copies** of it, and each copy uses only its own half of the fields:

| GUI copy (`isEngine = false`) | Engine copy (`isEngine = true`) |
| --- | --- |
| `lines`, `partial`, `scroll` | `cwd` (the real current directory of the tab) |
| `input`, `cursor`, `prompt` | `lastStatus` (`$?`) |
| `busy`, `more`, `pgid` of the foreground job | `jobs`, `nextJobId` |
| `histPos`, `histDraft` | `pending` (unfinished multiline input) |
| `choices`, `wordStart` (completion list) | `newGroups` (false inside multiWatch) |
| `inFd` write end, `outFd`/`ctlFd` read ends | `inFd` read end, `outFd`/`ctlFd` write ends |

`appendOutput()` is the one function both halves call to print. In the GUI copy it appends to `lines`; in the engine copy it writes to the `out` pipe. Built-ins and error messages in the engine therefore look the same as program output.

No field is shared between the two copies, so nothing needs a lock.

## 4. Communication: the three pipes

`newTab()` creates three pipes with `pipe2(O_CLOEXEC)`:

| Pipe | Direction | Contents |
| --- | --- | --- |
| `in` | GUI → engine | `L <line>`: a line the user entered; `K`: Ctrl+C was pressed at the prompt or at `> ` |
| `out` | engine and its children → GUI | Raw output: stdout and stderr of every program, plus built-in messages |
| `ctl` | engine → GUI | `R <cwd>`: ready for the next command; `M`: more input needed (show `> `); `B <pgid>`: a foreground job is running; `C`: clear the screen; `X`: the engine has exited |

Messages on `in` and `ctl` are single text lines. The engine reads `in` with `getline()` on an `fdopen()`ed stream; the GUI keeps a buffer (`ctlBuf`) for `ctl` and handles each complete line.

A typical command:

```
user types "ls | wc -l" + Return
GUI  --in-->  "L ls | wc -l"
engine: parse, fork ls and wc (own process group)
engine --ctl-->  "B 4242"            GUI: busy, Ctrl+C/Ctrl+Z go to -4242
wc    --out-->  "12\n"               GUI: append, redraw
engine: waitpid both, $? = 0
engine --ctl-->  "R /home/rupam"     GUI: not busy, new prompt for this cwd
```

Separating `out` from `ctl` means program output can never be mistaken for a control message, whatever it contains.

`O_CLOEXEC` on every pipe matters: without it, every program would inherit the ends of every tab's pipes, and a tab's `out` pipe would never reach EOF while a program from another tab still held it open.

## 5. Thread mode and process mode

The mode is chosen once at start-up: `./terminator` is thread mode and `./terminator useProcess=True` is process mode. Both call the same `engineMain(TerminalState)`.

**Thread mode.** `newTab()` starts `std::thread(engineMain, engineCopy)` and detaches it. All tabs are threads of one process, so they share the process's real current directory, which is why `cd` never calls `chdir()` (section 6).

**Process mode.** `newTab()` calls `fork()`. The child closes the X connection fd and the GUI ends of every tab's pipes (its own and those of the tabs that already exist), runs `engineMain()` and ends with `_exit(0)`. The parent closes the engine ends. A crash in one tab's engine, such as a `SIGSEGV`, cannot touch the GUI or the other tabs; this was tested with `kill -SEGV` on a tab process.

**How shared state is synchronized.** There is no shared memory and no lock in either mode, because nothing is shared:

- Text, the input line, history and completion live only in the GUI, which is a single thread.
- `cwd`, `$?` and jobs live only in the tab's engine.
- Everything that crosses between them is a message on a pipe, and a pipe is already a synchronized FIFO queue.

History is shared across tabs because it is a single vector in the GUI; engines never see it. The same design therefore works for threads and for processes, and process mode needed no shared memory.

**Closing a tab** (`shutdownTab()`):

1. If a foreground job is running, send `SIGHUP` to its process group.
2. Close the GUI's write end of `in`. The engine's `getline()` returns EOF.
3. The engine sends `SIGHUP` then `SIGCONT` to every job (`killAllJobs()`), closes its fds and returns.
4. The GUI makes `ctl` blocking and reads it until EOF, so it knows the engine has finished.
5. In process mode, `waitpid()` the engine process so no zombie is left.
6. Close the GUI's ends of `out` and `ctl`.

`exit` or Ctrl+D runs the same path. Closing the last tab, or the window, closes Terminator.

## 6. Running commands

`runCommand()` in executor.cpp:

1. If the line is a `multiWatch` command, hand it to `runMultiWatch()`.
2. Tokenize it (section 7). A trailing `&` makes it a background job; an `&` anywhere else is a syntax error.
3. Split the tokens on `|` into stages. Each stage has its words and its `<`, `>` and `>>` targets. Unquoted words containing `*`, `?` or `[` are expanded with `glob()` relative to the tab's `cwd`; a pattern with no match stays as typed, as in bash.
4. A single stage that is a built-in runs inside the engine: `exit`, `cd`, `clear`, `jobs`, `fg`, `bg`.
5. A stage that would start a shell is refused (section 11).
6. Otherwise fork one child per stage (section 8).

**`cd` without `chdir()`.** In thread mode all tabs share one process-wide current directory, so `chdir()` in one tab would move every tab. Instead, `cd` resolves the path against the tab's `cwd` string, checks that it is a directory (`std::filesystem::is_directory`) that can be entered (`access(X_OK)`), and stores the result. Each child calls `chdir(cwd)` after `fork()` and before `execvp()`, and relative paths in redirections, globs and completion are resolved against `cwd`. Process mode uses the same code.

**Exit status.** `$?` is the status of the last stage: its exit code, or `128 + signal` if a signal killed it (130 for Ctrl+C). A command that `execvp()` cannot find gives 127 and a "command not found" message.

## 7. Lexer and multiline input

`tokenize()` in lexer.cpp walks the line character by character and returns `Token { text, isOp, quoted }`:

- Spaces separate words; `|`, `<`, `>`, `>>` and `&` are operators even without spaces (`ls>out.txt`).
- `'...'` is literal. `"..."` keeps spaces but expands `$`. `\` escapes the next character.
- `$NAME`, `${NAME}` and `$?` are replaced inside and outside double quotes.
- `$(cmd)` is run by `captureOutput()`: fork, a pipe for stdout, read to EOF, wait, and strip the trailing newlines.
- `quoted` is set if any part of the word was quoted or escaped, so `"*.txt"` and `\*` are not globbed and a quoted `"&"` is not an operator.

If the line ends inside a quote, after a trailing `\` or `|`, or inside an unclosed `$(`, the lexer reports it as incomplete. The engine keeps the text in `pending`, sends `M`, and the GUI shows `> `. The next line is joined to `pending` with a newline and parsed again; a `\` followed by that newline is a line continuation and adds nothing. Ctrl+C at `> ` sends `K`, which clears `pending`.

## 8. Process management, pipes and redirection

For a pipeline of N stages the engine creates N − 1 pipes and forks N children. Each child:

1. Joins the job's process group: `setpgid(0, pgid)`, where the first child's pid becomes the pgid. The parent makes the same `setpgid()` call, so the group exists whichever of the two runs first.
2. Resets `SIGINT`, `SIGQUIT`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU` and `SIGPIPE` to `SIG_DFL`.
3. Wires its standard fds with `dup2()`:
   - stdin: the previous pipe's read end, or `/dev/null` for the first stage (there is no terminal to read from);
   - stdout: the next pipe's write end, or the tab's `out` pipe for the last stage;
   - stderr: the tab's `out` pipe.
4. Applies its redirections, which override the pipe wiring: `<` opens with `O_RDONLY`, `>` with `O_WRONLY|O_CREAT|O_TRUNC`, `>>` with `O_WRONLY|O_CREAT|O_APPEND`, all mode 0644 and relative to `cwd`. If the open fails, the child prints the error and exits with 1.
5. Closes every pipe fd, calls `chdir(cwd)` and `execvp()`.

The parent closes each pipe end as soon as both of its neighbours are forked, so a reader always gets EOF when its writer exits.

## 9. Signals and job control

There is no terminal, so there is no terminal driver to turn Ctrl+C into `SIGINT`. Terminator sends the signals itself:

- When a foreground job starts, the engine sends `B <pgid>`. The GUI stores the pgid.
- Ctrl+C in the GUI calls `kill(-pgid, SIGINT)`; Ctrl+Z calls `kill(-pgid, SIGTSTP)`. Every process of the pipeline gets the signal, and Terminator itself does not, because the job is in its own process group.
- With nothing running, Ctrl+C clears the input line (and sends `K` at a `> ` prompt).

`waitForeground()` calls `waitpid(pid, &status, WUNTRACED)` for each process of the job:

- If a process is stopped, the whole job becomes a `Job { id, pgid, pids, command, state = Stopped }` in the engine's `jobs` vector and `[n]+ Stopped  command` is printed.
- If all processes have exited, `$?` is set from the last stage.

Built-ins:

- `jobs` lists the jobs with `+` on the current job and `-` on the previous one. The current job follows bash's rule: the most recently stopped job, or else the most recently started one.
- `fg [%n]` sends `kill(-pgid, SIGCONT)`, sends `B pgid` to the GUI and waits for the job as a foreground job again.
- `bg [%n]` sends `SIGCONT` and marks the job Running.
- `cmd &` starts the job in its own process group, prints `[n] pid` and returns to the prompt at once.

Before each prompt, `reportFinishedJobs()` calls `waitpid(-pgid, WNOHANG)` for each job and prints `[n]  Done  command` (or the signal name) for the ones that finished, as bash does.

**Other signals.** The GUI ignores `SIGPIPE`, so writing to the pipe of an engine that has died returns `EPIPE` instead of killing Terminator; children reset it to the default so that `yes | head` ends normally. Closing a tab sends `SIGHUP` followed by `SIGCONT` to every job, so stopped jobs also receive the hang-up and exit. Since process-group signals are used, no signal handlers are installed anywhere, and no code needs to be async-signal-safe.

## 10. multiWatch

`multiWatch ["cmd1", "cmd2", ...]` is parsed by `parseCommandList()`. The engine forks one **watcher** process in its own process group and waits for it as a normal foreground job, so Ctrl+C reaches the watcher and all the commands it runs.

The watcher runs each command through the same `runCommand()` with `newGroups = false`, so the commands stay in the watcher's group. Each run gets its own pipe for stdout and stderr. The watcher `poll()`s all the read ends, with a timeout, and collects each command's output until EOF. When a run finishes, the watcher prints its block and starts the same command again two seconds later:

```
"date +%T", 1730637562:
----------------------------------------------------
12:39:22
----------------------------------------------------
```

The timestamp is the Unix time at which that run's output was printed. Commands run in parallel and never wait for each other. Ctrl+C kills the whole group; the engine reports status 130 and no process is left behind.

## 11. No shell is ever started

Following the course rule, Terminator never hands a command to `bash` or another shell: there is no `system()`, no `popen()` and no `sh -c` anywhere. `isShellInvocation()` refuses `bash`, `sh`, `dash`, `zsh`, `ksh`, `source`, `.` and any program ending in `.sh`, both as a pipeline stage and inside `$(...)`, with "running shell scripts is not supported (no shell is ever started)" and `$?` = 1.

## 12. History

```
vector<string> history;        // in the GUI, one for all tabs
TerminalState::histPos;        // per tab, -1 = not browsing
TerminalState::histDraft;      // per tab, the line being typed before browsing
```

On Return, a non-empty line is appended unless it equals the previous entry. ↑ saves the current input as `histDraft` the first time, moves `histPos` back and shows that entry; ↓ moves forward and, past the newest entry, restores `histDraft`. Each tab has its own position, but all tabs read the same vector, so a command typed in one tab can be recalled in another. Appending is O(1) amortized, and moving through history is O(1). History is not saved to a file (see the limitations in the README).

## 13. Auto-completion

When Tab is pressed, `completeWord()` takes the word before the cursor and splits it into a directory part and a prefix (`src/ma` → `src/` and `ma`). It reads that directory (relative to the tab's `cwd`) with `readdir()` and inserts every name into a **trie**, with a trailing `/` on directories. Hidden files are included only if the prefix starts with `.`.

```
struct TrieNode { map<char, TrieNode*> mpp; bool flag; };
```

- `insert(word)`: O(L log σ) for a word of length L, where σ is the alphabet size, because children are kept in a `std::map`.
- `autocomplete(prefix)`: walk down the prefix, then keep going while the node has exactly one child and is not the end of a word. This yields the longest common prefix of all matches in O(P + C), where C is the length of the common prefix.
- `getWords(prefix)`: a depth-first walk below the prefix node, returning all matches in sorted order (the map keeps the children sorted).

Behaviour:

- One match: replace the word with it.
- Several matches: extend the word to their longest common prefix, then list the matches numbered `1.` to `9.` in the output. The next keystroke, if it is a digit, picks that match; any other key dismisses the list.
- No match: nothing changes.

The trie is rebuilt on every Tab press. A directory listing is small, rebuilding costs microseconds, and the result is never stale. `make test` runs 22 unit tests on the trie (tests/trie_test.cpp), which needs no X11.

## 14. Hotkeys and configuration

```
enum Action { ACT_LINE_START, ..., ACT_PASTE, ACT_COUNT };
struct Binding { string name; unsigned mods; KeySym sym; };
Binding bindings[ACT_COUNT];
```

`findAction(XKeyEvent&)` masks the event state to Ctrl, Shift and Alt, lowercases the KeySym with `XConvertCase()` and returns the matching action, or none. `handleAction()` is a single `switch`. Since the table is the only place where keys appear, rebinding needs no code change.

`loadKeyConfig()` reads `~/.terminatorrc` at start-up, one `action = key` per line, where a key is something like `Ctrl+Shift+L` or `Alt+b`. `parseKey()` reads the modifiers and passes the rest to `XStringToKeysym()`. A bad line is reported in the first tab and leaves the default in place. A line is bad if it has an unknown action, an unknown key, a printable key without Ctrl or Alt (it would stop that character from being typed), or a key that another action already uses.

## 15. Copy and paste

Copy (Ctrl+Shift+C) stores the input line, or the last output line if the input is empty, in `clipboardText`, and takes ownership of the `CLIPBOARD` selection with `XSetSelectionOwner()`. When any application asks for it (`SelectionRequest`), `answerSelectionRequest()` replies with the list of targets (`TARGETS`) or the text as `UTF8_STRING`/`STRING`, written with `XChangeProperty()` and confirmed with a `SelectionNotify` event.

Paste (Ctrl+Shift+V) calls `XConvertSelection(CLIPBOARD, UTF8_STRING)`. When the `SelectionNotify` arrives, the text is read with `XGetWindowProperty()` and inserted at the cursor, with newlines turned into spaces so that pasting cannot run a command by accident.

This is the standard X11 protocol, so copying to and from other applications works. Everything happens in the GUI's event loop and never blocks.

## 16. Other design decisions

- **One `poll()` loop and no GUI threads.** Xlib is not thread-safe by default. Keeping every X call on one thread avoids `XInitThreads()` and locking, and makes the event order predictable.
- **No PTY, by rule.** Output arrives through a pipe, so programs see a pipe rather than a terminal. Full-screen programs do not work, and some programs buffer their output. This is a deliberate consequence of the course rules.
- **The engine is single-threaded and synchronous.** Within one tab, commands run one after another, exactly like an interactive shell. Tabs run in parallel because each has its own engine.
- **Only plain text crosses the pipes.** A one-letter command plus an argument per line is easy to read with `strace` and easy to debug, and the same text works between threads or processes.
- **Errors never take down the GUI.** Engine errors are printed into the tab, a dead engine is detected by EOF on `ctl` and the tab is closed, and `SIGPIPE` is ignored.

## 17. Testing

- `make test` runs the trie unit tests.
- Both modes were tested by driving the window with synthetic X key events. The tests covered pipes, redirection, globbing, `$()`, multiline input, `$?`, Ctrl+C (status 130, no leftover processes), Ctrl+Z, `jobs`/`fg`/`bg`, `&`, multiWatch, completion, history, copy/paste, the key-configuration file, closing busy tabs, the refusal of shell scripts, and a tab process killed with `SIGSEGV` (in process mode the GUI and other tabs keep running).
- `valgrind --track-fds=yes` in thread mode reported no memory errors and no leaked fds.
