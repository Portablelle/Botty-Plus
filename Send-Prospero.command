#!/bin/zsh
cd -- "${0:A:h}" || exit 1
python3 send-prospero.py
read "?Press Enter to close."
