.PHONY: ps5 test
CXX := c++
ps5:
	python3 tools/build.py
test:
	@mkdir -p build
	$(CXX) -std=c++20 -Wall -Wextra -Werror -pthread -Isrc tests/model_test.cpp src/model.cpp src/probe.cpp src/catalog.cpp src/actions.cpp -o build/model-test
	./build/model-test
	$(CXX) -std=c++20 -Wall -Wextra -Werror -Isrc tests/keyboard_test.cpp src/keyboard.cpp -o build/keyboard-test
	./build/keyboard-test
	$(CXX) -std=c++20 -Wall -Wextra -Werror -pthread -Isrc tests/native_update_test.cpp src/model.cpp src/probe.cpp src/catalog.cpp src/actions.cpp -o build/native-update-test
	./build/native-update-test
	python3 -m unittest discover -s tests -p 'test_*.py' -v
# macOS host preview of the actual renderer, with simulated platform APIs.
.PHONY: preview
preview:
	@mkdir -p build
	$(CXX) -std=c++20 -Wall -Wextra -Werror -pthread -DBOTTY_HOST_PREVIEW src/*.cpp tests/preview_stubs.cpp -o build/preview
	./build/preview
	sips -s format png build/preview.ppm --out build/preview.png
.PHONY: integration
integration:
	$(CXX) -std=c++20 -Wall -Wextra -Werror -pthread -Isrc tests/action_client.cpp src/model.cpp src/probe.cpp src/catalog.cpp src/actions.cpp -o build/action-client
	python3 tests/actions_integration.py

.PHONY: compression-integration
compression-integration:
	$(MAKE) -C ../botty native
	$(CXX) -std=c++20 -Wall -Wextra -Werror -pthread -Isrc tests/action_client.cpp src/model.cpp src/probe.cpp src/catalog.cpp src/actions.cpp -o build/action-client
	python3 ../botty/tests/compressor_integration.py
