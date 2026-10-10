PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
NATIVE_CXX ?= c++
UNRAR := vendor/unrar
COMMON := rar strlist strfn pathfn smallfn global file filefn filcreat archive arcread unicode system crypt crc rawread encname resource match timefn rdwrfn consio options errhnd rarvm secpassword rijndael getbits sha1 sha256 blake2s hash extinfo extract volume list find unpack headers threadpool rs16 cmddata ui largepage filestr scantree dll qopen
NATIVE_OBJ := $(addprefix build/native/,$(addsuffix .o,$(COMMON)))
PS5_OBJ := $(addprefix build/ps5/,$(addsuffix .o,$(COMMON)))
FLAGS := -O2 -std=c++17 -pthread -Ivendor -Isrc -I$(UNRAR) -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
APP_FLAGS := $(FLAGS) -Wall -Wextra -Werror
UPDATER_HEADERS := $(wildcard src/*.hpp src/*.h) vendor/json.hpp vendor/httplib.h $(UNRAR)/sha256.hpp $(UNRAR)/rartypes.hpp
RAR_FLAGS := -DRARDLL -DRAR_SMP -DSILENT -D_UNIX
PS5_CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
.PHONY: native ps5 test test-progress
native: build/botty-native
ps5: build/botty-manager.elf
build/native/%.o: $(UNRAR)/%.cpp $(UNRAR)/global.hpp $(UNRAR)/unpack30.cpp
	@mkdir -p build/native
	$(NATIVE_CXX) $(FLAGS) $(RAR_FLAGS) -MMD -MP -c $< -o $@
# Share host production objects across service and component test executables.
# Generated dependencies include every transitive header (also for unrar).
build/host/%.o: src/%.cpp
	@mkdir -p build/host
	$(NATIVE_CXX) $(APP_FLAGS) -MMD -MP -c $< -o $@
-include $(wildcard build/host/*.d build/native/*.d)
build/native/sha1.o build/ps5/sha1.o: $(UNRAR)/sha1.hpp $(UNRAR)/rartypes.hpp
build/ps5/%.o: $(UNRAR)/%.cpp $(UNRAR)/global.hpp $(UNRAR)/unpack30.cpp
	@mkdir -p build/ps5
	$(PS5_CXX) $(FLAGS) $(RAR_FLAGS) -DPS5_PAYLOAD -c $< -o $@
build/botty-native: build/host/server.o build/host/core.o build/host/storage.o build/host/rar.o $(NATIVE_OBJ) $(UPDATER_HEADERS)
	$(NATIVE_CXX) $(APP_FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl -lz
build/botty-manager.elf: src/server.cpp src/core.cpp src/storage.cpp src/rar.cpp $(PS5_OBJ) $(UPDATER_HEADERS) build/service-updater-embedded.hpp
	$(PS5_CXX) $(APP_FLAGS) -D__PS5__ $(filter %.cpp %.o,$^) -o $@ -lkernel_sys -lcurl -lpsl -lssl -lcrypto -lzstd -lz
build/service-updater.elf: src/service-updater.cpp $(UPDATER_HEADERS) build/ps5/sha256.o
	$(PS5_CXX) $(APP_FLAGS) -D__PS5__ $(filter %.cpp %.o,$^) -o $@ -lkernel_sys -lcurl -lpsl -lssl -lcrypto -lzstd -lz
build/service-updater-embedded.hpp: build/service-updater.elf tools/embed-service-updater.py
	python3 tools/embed-service-updater.py $< $@
build/core-test: tests/core-test.cpp build/host/core.o build/host/storage.o build/host/rar.o $(NATIVE_OBJ) src/unicode.hpp src/storage.hpp src/progress.hpp src/operations.hpp src/search.hpp src/rtorrent.hpp src/compressor.hpp src/compression-library.hpp src/background-storage.hpp src/notification.hpp
	$(NATIVE_CXX) $(FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl -lz
build/progress-test: tests/progress-test.cpp src/progress.hpp
	@mkdir -p build
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $< -o $@
test-progress: build/progress-test
	./build/progress-test
test: build/rest-companion-test build/rest-network-test build/rest-http-test build/core-test build/botty-native build/storage-test build/rest-mode-test build/progress-test build/rtorrent-resume-client build/native-transaction-test build/native-update-test build/native-idle-test build/installation-update-test build/installation-runtime-test build/search-idle-test
	./build/progress-test
	./build/core-test
	./build/rest-mode-test
	./build/rest-companion-test
	./build/rest-network-test
	./build/rest-http-test
	./build/native-transaction-test
	./build/native-update-test
	./build/native-idle-test
	./build/installation-update-test
	./build/installation-runtime-test
	./build/search-idle-test
	python3 tests/integration.py
	python3 tests/rtorrent_resume.py
	python3 tests/search_integration.py
	./build/storage-test
	python3 tests/storage_integration.py
	python3 tests/background_storage_integration.py

build/compressor-test: tests/compressor-test.cpp build/host/core.o build/host/storage.o src/storage.hpp src/compressor.hpp src/progress.hpp src/compression-library.hpp src/background-storage.hpp src/notification.hpp
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $(filter %.cpp %.o,$^) -o $@ -lcurl
.PHONY: test-compressor
test-compressor: build/compressor-test
	./build/compressor-test

build/native-transaction-test: tests/native-transaction-test.cpp $(UPDATER_HEADERS) build/native/sha256.o
	@mkdir -p build
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $(filter %.cpp %.o,$^) -o $@
build/native-update-test: tests/native-update-test.cpp $(UPDATER_HEADERS) build/native/sha256.o
	@mkdir -p build
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $(filter %.cpp %.o,$^) -o $@ -lcurl
build/native-idle-test: tests/native-idle-test.cpp build/host/core.o build/host/storage.o $(UPDATER_HEADERS)
	@mkdir -p build
	$(NATIVE_CXX) $(APP_FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl
.PHONY: test-native-update
test-native-update: build/native-transaction-test build/native-update-test build/native-idle-test
	./build/native-transaction-test
	./build/native-update-test
	./build/native-idle-test

build/installation-update-test: tests/installation-update-test.cpp $(UPDATER_HEADERS) build/native/sha256.o
	@mkdir -p build
	$(NATIVE_CXX) $(APP_FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl -lz
.PHONY: test-installation-update
test-installation-update: build/installation-update-test
	./build/installation-update-test

build/installation-runtime-test: tests/installation-runtime-test.cpp $(UPDATER_HEADERS) build/native/sha256.o
	$(NATIVE_CXX) $(APP_FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl -lz
build/search-idle-test: tests/search-idle-test.cpp src/search.hpp src/core.hpp vendor/json.hpp
	@mkdir -p build
	$(NATIVE_CXX) $(APP_FLAGS) $< -o $@
.PHONY: test-installation-runtime
test-installation-runtime: build/installation-runtime-test build/search-idle-test
	./build/installation-runtime-test
	./build/search-idle-test

build/rtorrent-resume-client: tests/rtorrent-resume-client.cpp build/host/core.o build/host/storage.o build/native/sha1.o src/rtorrent.hpp src/core.hpp src/storage.hpp $(UNRAR)/sha1.hpp $(UNRAR)/rartypes.hpp
	@mkdir -p build
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $(filter %.cpp %.o,$^) -o $@

.PHONY: test-rtorrent-resume
test-rtorrent-resume: build/rtorrent-resume-client
	python3 tests/rtorrent_resume.py

build/compression-library-test: tests/compression-library-test.cpp build/host/core.o build/host/storage.o src/storage.hpp src/compression-library.hpp src/background-storage.hpp src/notification.hpp
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $(filter %.cpp %.o,$^) -o $@
.PHONY: test-compression-library
test-compression-library: build/compression-library-test
	./build/compression-library-test

build/notification-test: tests/notification-test.cpp src/notification.hpp
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror -D__PS5__ $< -o $@
.PHONY: test-notification
test-notification: build/notification-test
	./build/notification-test

build/storage-test: tests/storage-test.cpp build/host/core.o build/host/storage.o src/storage.hpp
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $(filter %.cpp %.o,$^) -o $@
.PHONY: test-storage
test-storage: build/storage-test build/botty-native
	./build/storage-test
	python3 tests/storage_integration.py
	python3 tests/background_storage_integration.py

.PHONY: test-rest-mode
build/rest-mode-test: tests/rest-mode-test.cpp src/rest-mode.hpp
	@mkdir -p build
	$(NATIVE_CXX) $(FLAGS) -Wall -Wextra -Werror $< -o $@
test-rest-mode: build/rest-mode-test
	./build/rest-mode-test

build/rest-network-test: tests/rest-network-test.cpp src/rest-network.hpp
	@mkdir -p build
	$(NATIVE_CXX) $(APP_FLAGS) $< -o $@
.PHONY: test-rest-network
build/rest-http-test: tests/rest-http-test.cpp src/rest-network.hpp vendor/httplib.h
	@mkdir -p build
	$(NATIVE_CXX) $(APP_FLAGS) $< -o $@
test-rest-network: build/rest-network-test build/rest-http-test
	./build/rest-network-test
	./build/rest-http-test

build/rest-companion-test: tests/rest-companion-test.cpp src/rest-companion.hpp src/rest-companion-protocol.h build/host/core.o build/host/storage.o build/host/rar.o $(NATIVE_OBJ)
	$(NATIVE_CXX) $(APP_FLAGS) $(filter %.cpp %.o,$^) -o $@ -lcurl -lz
