#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -pthread -Itests/stubs -Ibuild/include \
  tests/test_title_dir.c build/src/sm_shellcore_bridge.S -o build/test-title-dir
./build/test-title-dir

${CC:-cc} -std=c11 -Wall -Wextra -Werror -Ibuild/include tests/test_botty_storage_policy.c -o build/test-storage-policy
./build/test-storage-policy
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -Itests/stubs -Ibuild/include tests/test_storage_copy.c -o build/test-storage-copy
./build/test-storage-copy
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Ibuild/include tests/test_image_rebase.c -o build/test-image-rebase
./build/test-image-rebase
