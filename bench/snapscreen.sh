#!/usr/bin/env bash
# what bench/snapdiff.sh draws: colors, bold, inverse, underline, wide chars, box drawing
printf '\e[1mbold\e[0m \e[2mdim\e[0m \e[3mitalic\e[0m \e[4munder\e[0m \e[9mstrike\e[0m \e[7minverse\e[0m\n'
for i in 0 1 2 3 4 5 6 7; do printf '\e[4%dm  \e[0m\e[3%dm%d\e[0m ' $i $i $i; done; echo
for i in $(seq 16 51); do printf '\e[48;5;%dm \e[0m' $i; done; echo
printf '\e[38;2;255;100;0mtruecolor\e[0m 日本語 한국어 ✓✗●○…→ αβγ 🙂\n'
printf '┌──────┬──────┐\n│ box  │ draw │\n└──────┴──────┘\n'
printf '\e]8;;https://example.com\e\\link\e]8;;\e\\ and \e[1;31mbold red\e[0m\n'
seq 1 5
