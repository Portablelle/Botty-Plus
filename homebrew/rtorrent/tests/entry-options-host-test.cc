// Exercise the generated entry arguments with upstream's real option parser.
#include "option_parser.h"
#include <cassert>
#include <string>

int main(int argc, char** argv) {
    unsigned allocation = 1024, open_limit = 1024, adjustments = 0;
    bool imported = false;
    OptionParser parser;
    parser.insert_flag('n', [](const std::string&) {});
    parser.insert_option_list('o', [&](const std::string& key, const std::string& value) {
        if (key == "system.sockets.files.max_alloc.set") {
            allocation = std::stoul(value);
        } else if (key == "system.sockets.adjust_alloc") {
            assert(value.empty());
            open_limit = allocation;
            ++adjustments;
        } else if (key == "import") {
            // An old config opens SCGI without setting a cache allocation.
            assert(!imported && open_limit == 64 && adjustments == 1);
            imported = true;
            // Even an explicit override in that config must be reapplied before
            // session restoration, which occurs after parse_main_options.
            allocation = open_limit = 512;
        } else {
            return assert(false);
        }
    });
    assert(parser.process(argc, argv) == argc);
    assert(imported && allocation == 64 && open_limit == 64 && adjustments == 2);
}
