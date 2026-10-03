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
build/botty-native: src/server.cpp src/core.cpp src/storage.cpp src/rar.cpp $(NATIVE_OBJ) src/unicode.hpp src/storage.hpp src/progress.hpp src/operations.hpp src/search.hpp src/rtorrent.hpp src/compressor.hpp src/compression-library.hpp src/background-storage.hpp src/notification.hpp
	$(NATIVE_CXX) $(FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl -lz
build/botty-manager.elf: src/server.cpp src/core.cpp src/storage.cpp src/rar.cpp $(PS5_OBJ) src/unicode.hpp src/storage.hpp src/progress.hpp src/operations.hpp src/search.hpp src/rtorrent.hpp src/compressor.hpp src/compression-library.hpp src/background-storage.hpp src/notification.hpp
	$(PS5_CXX) $(FLAGS) -D__PS5__ $(filter %.cpp %.o,$^) -o $@ -lkernel_sys -lcurl -lpsl -lssl -lcrypto -lzstd -lz
build/core-test: tests/core-test.cpp src/core.cpp src/storage.cpp src/rar.cpp $(NATIVE_OBJ) src/unicode.hpp src/storage.hpp src/progress.hpp src/operations.hpp src/search.hpp src/rtorrent.hpp src/compressor.hpp src/compression-library.hpp src/background-storage.hpp src/notification.hpp
	$(NATIVE_CXX) $(FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl -lz
test: build/core-test build/botty-native build/storage-test
	./build/core-test
	python3 tests/integration.py
	python3 tests/search_integration.py
	./build/storage-test
	python3 tests/storage_integration.py
	python3 tests/background_storage_integration.py

build/compressor-test: tests/compressor-test.cpp src/core.cpp src/storage.cpp src/storage.hpp src/compressor.hpp src/compression-library.hpp src/background-storage.hpp src/notification.hpp
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $(filter %.cpp,$^) -o $@ -lcurl
.PHONY: test-compressor
test-compressor: build/compressor-test
	./build/compressor-test

build/compression-library-test: tests/compression-library-test.cpp src/core.cpp src/storage.cpp src/storage.hpp src/compression-library.hpp src/background-storage.hpp src/notification.hpp
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $(filter %.cpp,$^) -o $@
.PHONY: test-compression-library
test-compression-library: build/compression-library-test
	./build/compression-library-test

build/notification-test: tests/notification-test.cpp src/notification.hpp
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror -D__PS5__ $< -o $@
.PHONY: test-notification
test-notification: build/notification-test
	./build/notification-test

build/storage-test: tests/storage-test.cpp src/core.cpp src/storage.cpp src/storage.hpp
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $(filter %.cpp,$^) -o $@
.PHONY: test-storage
test-storage: build/storage-test build/botty-native
	./build/storage-test
	python3 tests/storage_integration.py
	python3 tests/background_storage_integration.py
