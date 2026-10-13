#!/bin/bash
# Pretends to be Claude Code working: animates the window title the way it
# does ("◐ task" / "◑ task"), then settles on its idle "✳" and rings nothing.
# Used to check the tab throbber and the "done" bell mark.   spin.sh [SECONDS]
end=$((SECONDS + ${1:-5}))
while [ $SECONDS -lt $end ]; do
  printf '\e]0;◐ Refactoring the parser\a'; sleep 0.5
  printf '\e]0;◑ Refactoring the parser\a'; sleep 0.5
done
printf '\e]0;✳ Refactoring the parser\a'
