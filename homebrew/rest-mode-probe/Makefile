PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
CFLAGS = -O2 -std=c11 -Wall -Wextra -Werror
.PHONY: host ps5 ps5-keep-main
host:
	$(CC) $(CFLAGS) probe.c -o rest-probe-host
ps5:
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(CFLAGS) -DBOTTY_PS5 probe.c -lSceSystemService -o rest-probe.elf
ps5-keep-main:
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(CFLAGS) -DBOTTY_PS5 -DREST_PROBE_KEEP_MAIN=1 probe.c -lSceSystemService -o keep-main-probe.elf
