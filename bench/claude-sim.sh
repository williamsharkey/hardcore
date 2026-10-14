#!/bin/bash
# Plays Claude Code's OSC 7501 program-status messages (the format Claude Code
# 2.1 sends once the terminal answers "OSC 7501 ; ?"), to check the sidebar.
#   claude-sim.sh [working|blocked|done|error|clear|demo]
st() { printf '\e]7501;state=%s:app=claude-code%s\e\\' "$1" "$2"; }
b64() { printf '%s' "$1" | base64 | tr -d '\n'; }   # GNU and BSD (macOS) base64 alike
case ${1:-demo} in
  working) st working ":progress=40:msg=$(b64 'Refactoring the VT parser')" ;;
  blocked) st blocked ":kind=permission:msg=$(b64 'approve Bash: rm -rf build')" ;;
  done)    st done ":msg=$(b64 'Fixed 3 failing esctest cases')" ;;
  error)   st error ":msg=$(b64 'API overloaded — wait and retry')" ;;
  clear)   printf '\e]7501;state=clear\e\\' ;;
  demo)
    st working ":msg=$(b64 'Reading hardcore.c')"; sleep 2
    printf '\e]7501;state=working:app=claude-code:id=agent1\e\\'
    st working ":msg=$(b64 'Running make test')"; sleep 2
    st blocked ":kind=permission:msg=$(b64 'approve Bash: git push')"; sleep 3
    printf '\e]7501;state=done:app=claude-code:id=agent1\e\\'
    st done ":msg=$(b64 'Pushed 2 commits to main')" ;;
esac
