#include <torrent/data/file.h>
#include <torrent/data/file_manager.h>
#include "data/memory_chunk.h"
#include "data/socket_file.h"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <iostream>

// Use the real file-manager, socket-file and asynchronous close-queue sources.
// Replace only fd_open_file's networking log infrastructure with the same open.
namespace torrent {
int fd_open_file(const std::string& path, int flags, mode_t mode) {
    return ::open(path.c_str(), flags | O_CLOEXEC, mode);
}
}

class TestFile : public torrent::File {
public:
    explicit TestFile(const std::string& path) { set_frozen_path(path); }
};

static int test_multifile(int argc, char** argv) {
    assert(argc == 2);
    rlimit limit{};
    assert(getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur >= 64);
    limit.rlim_cur = 64;
    assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    constexpr int prot = torrent::MemoryChunk::prot_read | torrent::MemoryChunk::prot_write;
    torrent::FileManager manager;
    manager.set_max_open_files(128); // Deliberately above the real process limit.
    std::vector<std::unique_ptr<TestFile>> files;
    for (unsigned i = 0; i < 187; ++i)
        files.push_back(std::make_unique<TestFile>(std::string(argv[1]) + "/part-" + std::to_string(i)));
    TestFile missing(std::string(argv[1]) + "/missing/part");
    assert(!manager.open(&missing, false, prot, O_CREAT) && errno == ENOENT);
    assert(manager.open_files() == 0 && manager.files_closed_counter() == 0);
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < files.size(); ++i) {
            auto* file = files[i].get();
            if (!manager.open(file, true, prot, O_CREAT)) {
                std::cerr << "file open failed: errno=" << errno << " at part " << i << "\n";
                for (auto& opened : files)
                    if (opened->is_open()) manager.close(opened.get());
                return 42;
            }
            file->set_last_touched(pass * files.size() + i + 1);
            assert(lseek(file->file_descriptor(), 0, SEEK_SET) == 0);
            if (pass == 0) {
                assert(write(file->file_descriptor(), &i, sizeof(i)) == sizeof(i));
            } else {
                unsigned value = 0;
                assert(read(file->file_descriptor(), &value, sizeof(value)) == sizeof(value));
                assert(value == i);
            }
        }
    }
    assert(manager.files_closed_counter() > 0 && manager.files_failed_counter() == 1);
    assert(manager.max_open_files() == 128);
    for (auto& file : files)
        if (file->is_open()) manager.close(file.get());
    return 0;
}

static void test_without_victim(const std::string& directory) {
    constexpr int prot = torrent::MemoryChunk::prot_read | torrent::MemoryChunk::prot_write;
    torrent::FileManager manager;
    manager.set_max_open_files(128);
    // No cached files can be reclaimed when unrelated descriptors fill the limit.
    std::vector<int> blockers;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) { assert(errno == EMFILE); break; }
        blockers.push_back(fd);
    }
    TestFile blocked(directory + "/blocked");
    assert(!manager.open(&blocked, false, prot, O_CREAT) && errno == EMFILE);
    assert(manager.files_failed_counter() == 1 && manager.open_files() == 0);
    for (int fd : blockers) assert(fcntl(fd, F_GETFD) != -1);
    for (int fd : blockers) close(fd);
    assert(manager.open_files() == 0);
}

int main(int argc, char** argv) {
    int result = test_multifile(argc, argv);
    if (result) return result;
    test_without_victim(argv[1]);
    std::cout << "187-file write/read under RLIMIT_NOFILE=64 passed\n";
}
