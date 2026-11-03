# Terminator

Terminator is a terminal emulator and shell written from scratch in C++ on top of Xlib, for CS69201 Computing Lab (IIT Kharagpur).
It draws its own X11 window with tabs, runs commands with `fork`/`execvp`, and supports pipes, redirection, job control and `multiWatch`, with each tab running either as a thread or as a separate process.

Author: Rupam Hari (26CS60R04)

## Dependencies

A Linux system with an X server (any desktop session) and:

```
sudo apt install build-essential libx11-dev
```

No other libraries are used. Terminator does not use a pseudo-terminal (no `forkpty`) and never starts `bash` or any other shell.

## Build and run

```
make                          # builds ./terminator
./terminator                  # thread mode: one engine thread per tab (default)
./terminator useProcess=True  # process mode: one engine process per tab
make test                     # unit tests for the completion trie (no X11 needed)
make clean
```

The window title shows the mode: "Terminator (thread mode)" or "Terminator (process mode)".

## Features

| Spec task | Status | Notes |
| --- | --- | --- |
| 1. X11 GUI and tabs | Done | Own window, per-tab text buffer, scrolling, tab bar with `[+]` and an `x` per tab |
| 2. Built-in and external commands | Done | `fork` + `execvp`; built-ins `cd`, `exit`, `clear`, `jobs`, `fg`, `bg`, `multiWatch`; `$?` |
| 3. Multiline input | Done | Quotes, `\`, `$VAR`, `${VAR}`, `$?`, `$(...)`; unclosed quote, trailing `\` or `\|` continue on a `> ` line |
| 4. Redirection | Done | `<`, `>`, `>>`, also without spaces (`ls>out.txt`) |
| 5. Pipes | Done | Pipelines of any length, combined with redirection; `*`, `?`, `[...]` wildcards |
| 6. multiWatch | Done | Parallel, one pipe per command, `poll()`, re-run every 2 s, exact output format, Ctrl+C stops all |
| 7. Hotkeys | Done | All keys in the spec's table; configurable in `~/.terminatorrc` |
| 8. Signals and job control | Done | Ctrl+C, Ctrl+Z, `cmd &`, `jobs`, `fg [%n]`, `bg [%n]` |
| Two modes | Done | Thread mode and process mode behave the same |
| 9. Searchable history (bonus) | Partial | Up/Down history shared by all tabs; no history file, `history` built-in or Ctrl+R |
| 10. File-name auto-completion (bonus) | Done | Tab completes; longest common prefix; numbered list, pick with a digit |
| 11. Bash scripts (bonus) | Not done | `bash x.sh`, `sh x.sh`, `./x.sh` and `source x.sh` are refused, so no shell is ever started |

## Keys

| Key | Action |
| --- | --- |
| ← / → | Move within the line |
| Ctrl+A / Ctrl+E | Start / end of the line |
| ↑ / ↓ | Previous / next command (history shared by all tabs) |
| Ctrl+U / Ctrl+K | Delete from the cursor to the start / end of the line |
| Ctrl+W | Delete the previous word |
| Ctrl+L | Clear the screen |
| Ctrl+D | Close the tab (the last tab closes Terminator) |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy / paste through the X11 `CLIPBOARD` (works with other applications) |
| Ctrl+C | Interrupt the running command, or cancel the line being typed |
| Ctrl+Z | Stop the running command; it becomes a job |
| Tab | Complete a file name |
| Ctrl+Shift+T | New tab (opens in the current tab's directory) |
| Ctrl+→ / Ctrl+← | Next / previous tab |
| Shift+PageUp / Shift+PageDown, mouse wheel | Scroll |

Tabs can also be switched, opened and closed with the mouse: click a tab, its `x`, or `[+]`.

Copy takes the line being typed; if it is empty, it takes the last output line.

## Configuring keys

Every key in the table can be rebound in `~/.terminatorrc`, one `action = key` per line. The file is read at start-up, so a change needs no recompiling and survives a restart.
An unknown action, an invalid key, a printable key without Ctrl or Alt, or a key already used by another action is reported in the first tab, and that action keeps its default.

```
cp terminatorrc.example ~/.terminatorrc
```

`terminatorrc.example` lists every action name with its default key.

## Try it

```
ls -l | grep cpp | sort -r > out.txt
cat out.txt | wc -l
ls *.txt | wc -l
echo "a    b" '$HOME' "$HOME"
echo "multi                      (press Enter, Terminator shows "> ")
line"
echo $(date)
false; echo $?               (two commands: false, then echo $?)
sleep 100                    (then Ctrl+Z, then: jobs, bg, fg, Ctrl+C)
sleep 30 &
jobs
multiWatch ["date +%T", "ls | wc -l"]      (Ctrl+C to stop)
cat ab<Tab>
```

`;` is not supported, so run `false` and `echo $?` as two separate commands.

## Project layout

```
src/
  main.cpp            start-up, mode selection, the poll() event loop
  gui.cpp / gui.h     X11 window, colours, font, drawing the tab bar and text
  events.cpp          key and mouse events, hotkey actions, completion, copy/paste
  keys.cpp / keys.h   hotkey table, ~/.terminatorrc
  tabs.cpp / tabs.h   creating tabs (thread or process), the 3 pipes, engine messages
  terminal_state.*    the per-tab state (one GUI copy and one engine copy per tab)
  engine.cpp          the engine loop of a tab: read a line, run it, report back
  executor.cpp        built-ins, redirection, pipelines, wildcards, job control
  lexer.cpp           tokenizer: quotes, escapes, $VAR, $?, $(...)
  multiwatch.cpp      multiWatch
  autocomplete.*      trie used by Tab completion
tests/trie_test.cpp   unit tests for the trie (make test)
terminatorrc.example  sample key configuration
DESIGN.md             design document
.vscode/              build tasks and gdb launch configurations for both modes
```

## Known limitations

- **No pseudo-terminal**, as required by the course. Programs see a pipe, not a terminal: full-screen or interactive programs (`vim`, `top`, `less`, a Python REPL) do not work, commands read their input from `/dev/null`, and programs that buffer their output when it is not a terminal print it in chunks.
- ANSI escape sequences (colours, cursor movement) are not interpreted, and long lines are cut at the window edge instead of wrapping.
- No `;`, `&&`, `||`, variable assignment (`NAME=value`) or functions. `$VAR` reads the environment Terminator was started with.
- `$(...)` runs a single command (no pipes or redirection inside), and its output is not split into words.
- History is kept in memory only and is lost on exit; there is no `history` built-in or Ctrl+R search.
- Scripts are not interpreted (bonus Task 11). Any `.sh` file, `bash`, `sh` and `source` are refused instead of being handed to a shell. A script without the `.sh` extension that starts with `#!/bin/bash` would still be run by `execvp`.
- A finished background job is reported (`[1] Done`) after the next command, as in bash.
- Ctrl+Z is ignored while `multiWatch` runs; use Ctrl+C.
- In `~/.terminatorrc`, conflicts are checked line by line, so swapping two keys is reported as a conflict; bind one of them to a free key instead.
- A Tab completion list can be picked from with 1 to 9; with more matches, type more letters first.
- The clipboard text belongs to Terminator, so it is gone after Terminator exits (unless a desktop clipboard manager keeps it).

## Debugging

`.vscode/launch.json` has gdb configurations for thread mode, process mode (GUI), process mode (tab 1's engine, using `.vscode/follow-tab.gdb`) and attaching to a running tab process.
`.vscode/tasks.json` runs both modes, `strace` and `valgrind --track-fds=yes` for each mode, and shows the threads, the process tree and the GUI's pipes.
