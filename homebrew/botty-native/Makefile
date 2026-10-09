.PHONY: host-test-build ps5 test integration preview compression-integration keyboard-overlay-test
CXX := c++
HOST_FLAGS := -std=c++20 -Wall -Wextra -Werror -pthread -Isrc
MODEL_OBJ := $(addprefix build/host/,$(addsuffix .o,model probe catalog actions))
PREVIEW_OBJ := $(addprefix build/preview-host/,$(notdir $(patsubst %.cpp,%.o,$(filter-out src/main.cpp,$(wildcard src/*.cpp)))))

ps5:
	python3 tools/build.py

build/host/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(HOST_FLAGS) -MMD -MP -c $< -o $@
build/preview-host/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(HOST_FLAGS) -DBOTTY_HOST_PREVIEW -MMD -MP -c $< -o $@
build/preview-tests/%.o: tests/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(HOST_FLAGS) -DBOTTY_HOST_PREVIEW -MMD -MP -c $< -o $@
-include $(wildcard build/host/*.d build/preview-host/*.d build/preview-tests/*.d build/*.d)

build/model-test: tests/model_test.cpp $(MODEL_OBJ)
	$(CXX) $(HOST_FLAGS) -MMD -MP $(filter %.cpp %.o,$^) -o $@
build/keyboard-test: tests/keyboard_test.cpp build/host/keyboard.o
	$(CXX) $(HOST_FLAGS) -MMD -MP $(filter %.cpp %.o,$^) -o $@
build/native-update-test: tests/native_update_test.cpp $(MODEL_OBJ)
	$(CXX) $(HOST_FLAGS) -MMD -MP $(filter %.cpp %.o,$^) -o $@
build/action-client: tests/action_client.cpp $(MODEL_OBJ)
	$(CXX) $(HOST_FLAGS) -MMD -MP $(filter %.cpp %.o,$^) -o $@
build/keyboard-overlay-test: build/preview-tests/keyboard_overlay_test.o build/preview-tests/preview_stubs.o $(PREVIEW_OBJ)
	$(CXX) $(HOST_FLAGS) $(filter %.o,$^) -o $@

host-test-build: build/model-test build/keyboard-test build/native-update-test build/keyboard-overlay-test build/action-client

# Prerequisite compilation is bounded by the caller's -j; tests stay sequential.
test: build/model-test build/keyboard-test build/native-update-test build/keyboard-overlay-test
	./build/model-test
	./build/keyboard-test
	./build/native-update-test
	python3 -m unittest discover -s tests -p 'test_*.py' -v
	BOTTY_PREVIEW_MODE=keyboard-overlay ./build/keyboard-overlay-test

integration: build/action-client
	python3 tests/actions_integration.py

compression-integration: build/action-client
	$(MAKE) -C ../botty native
	python3 ../botty/tests/compressor_integration.py

keyboard-overlay-test: build/keyboard-overlay-test
	BOTTY_PREVIEW_MODE=keyboard-overlay ./build/keyboard-overlay-test

# macOS host preview of the actual renderer, with simulated platform APIs.
preview:
	@mkdir -p build
	$(CXX) $(HOST_FLAGS) -DBOTTY_HOST_PREVIEW src/*.cpp tests/preview_stubs.cpp -o build/preview
	./build/preview
	sips -s format png build/preview.ppm --out build/preview.png
