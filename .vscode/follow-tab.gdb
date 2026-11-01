# Process mode: debug the first tab's engine process instead of the GUI.
# The first fork() in Terminator is newTab() creating tab 1's process: follow that child.
set follow-fork-mode child
set detach-on-fork on

# Once inside the tab process, switch back to "parent", so the commands the engine
# forks (ls, sleep, ...) run freely and gdb stays in the engine.
tbreak engineMain
commands
  silent
  set follow-fork-mode parent
  continue
end