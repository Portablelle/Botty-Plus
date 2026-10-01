PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
NATIVE_CXX ?= c++
UNRAR := vendor/unrar
COMMON := rar strlist strfn pathfn smallfn global file filefn filcreat archive arcread unicode system crypt crc rawread encname resource match timefn rdwrfn consio options errhnd rarvm secpassword rijndael getbits sha1 sha256 blake2s hash extinfo extract volume list find unpack headers threadpool rs16 cmddata ui largepage filestr scantree dll qopen
NATIVE_OBJ := $(addprefix build/native/,$(addsuffix .o,$(COMMON)))
PS5_OBJ := $(addprefix build/ps5/,$(addsuffix .o,$(COMMON)))
FLAGS := -O2 -std=c++17 -pthread -Ivendor -Isrc -I$(UNRAR) -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
RAR_FLAGS := -DRARDLL -DRAR_SMP -DSILENT -D_UNIX
PS5_CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
.PHONY: native ps5 test
native: build/botty-native
ps5: build/botty-manager.elf
build/native/%.o: $(UNRAR)/%.cpp $(UNRAR)/global.hpp $(UNRAR)/unpack30.cpp
	@mkdir -p build/native
	$(NATIVE_CXX) $(FLAGS) $(RAR_FLAGS) -c $< -o $@
build/ps5/%.o: $(UNRAR)/%.cpp $(UNRAR)/global.hpp $(UNRAR)/unpack30.cpp
	@mkdir -p build/ps5
	$(PS5_CXX) $(FLAGS) $(RAR_FLAGS) -DPS5_PAYLOAD -c $< -o $@
build/botty-native: src/server.cpp src/core.cpp src/rar.cpp $(NATIVE_OBJ) src/unicode.hpp src/progress.hpp src/search.hpp
	$(NATIVE_CXX) $(FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl -lz
build/botty-manager.elf: src/server.cpp src/core.cpp src/rar.cpp $(PS5_OBJ) src/unicode.hpp src/progress.hpp src/search.hpp
	$(PS5_CXX) $(FLAGS) -D__PS5__ $(filter %.cpp %.o,$^) -o $@ -lkernel_sys -lcurl -lpsl -lssl -lcrypto -lzstd -lz
build/core-test: tests/core-test.cpp src/core.cpp src/rar.cpp $(NATIVE_OBJ) src/unicode.hpp src/progress.hpp src/search.hpp
	$(NATIVE_CXX) $(FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl -lz
test: build/core-test build/botty-native
	./build/core-test
	python3 tests/integration.py
	python3 tests/search_integration.py
