#!/bin/zsh
cd -- "${0:A:h}" || exit 1
python3 serve-local.py
read "?Press Enter to close."
