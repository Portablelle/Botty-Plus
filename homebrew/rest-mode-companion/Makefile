PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5DEBUG ?= build/ps5debug-NG
CC_PS5 := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CRT := $(PS5_PAYLOAD_SDK)/target/lib/crt1.o
FLAGS := -nostartfiles -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-command-line-argument -Wl,--build-id=none
SOURCES := $(PS5DEBUG)/common/source/proc_field_offsets.c $(PS5DEBUG)/common/source/sys_proc_rw.c $(addprefix $(PS5DEBUG)/installer/source/,find_pid.c kern_rw_fast.c proc_elf.c proc_remote.c stubs.c)
.PHONY: experimental check-pin test
check-pin:
	python3 check-inputs.py $(PS5DEBUG)
experimental: check-pin build/botty-rest-injector.elf
build/resident.elf: resident.c ../botty/src/rest-companion-protocol.h
	@mkdir -p build
	$(CC_PS5) $(FLAGS) -Wl,--wrap=__patch_init $< $(CRT) -o $@ -lSceSystemService -lSceLibcInternal -lkernel_sys -lpthread
build/embedded.S: build/resident.elf
	python3 -c 'from pathlib import Path; Path("$@").write_text(".section .rodata.embedded,\"a\",@progbits\n.balign 16\n.global embedded_inner_start\nembedded_inner_start:\n.incbin \"build/resident.elf\"\nembedded_inner_end:\n.balign 8\n.global embedded_inner_size\nembedded_inner_size:\n.quad embedded_inner_end - embedded_inner_start\n")'
build/embedded.o: build/embedded.S build/resident.elf
	$(CC_PS5) -c $< -o $@
build/botty-rest-injector.elf: injector.c ../botty/src/rest-companion-protocol.h build/embedded.o $(SOURCES) | check-pin
	$(CC_PS5) $(FLAGS) -Wno-unused-function -I$(PS5DEBUG)/common/include injector.c $(SOURCES) build/embedded.o $(CRT) -o $@ -lSceLibcInternal -lkernel_sys -lSceNet -lSceNetCtl -lSceSysCore
test:
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Werror protocol-test.c -o build/protocol-test
	./build/protocol-test
