#include "../runtime-at.hpp"
#include <cassert>
#include <cstdlib>

int main() {
    char path[]="/tmp/botty-runtime-at-XXXXXX";
    assert(mkdtemp(path));
    int directory=open(path,O_RDONLY|O_DIRECTORY);
    assert(directory>=0);
    int file=botty_rt_openat(directory,"old",O_WRONLY|O_CREAT|O_EXCL,0600);
    assert(file>=0&&write(file,"data",4)==4);close(file);
    struct stat status{};
    assert(!botty_rt_fstatat(directory,"old",&status,AT_SYMLINK_NOFOLLOW)&&status.st_size==4);
    assert(!botty_rt_renameat(directory,"old",directory,"new"));
    assert(!botty_rt_unlinkat(directory,"new",0));
    assert(botty_rt_fstatat(directory,"new",&status,AT_SYMLINK_NOFOLLOW)==-1&&errno==ENOENT);
    assert(botty_rt_openat(directory,"new",O_RDONLY)==-1&&errno==ENOENT);
    assert(botty_rt_renameat(directory,"new",directory,"other")==-1&&errno==ENOENT);
    assert(botty_rt_unlinkat(directory,"new",0)==-1&&errno==ENOENT);
    assert(botty_rt_at_call(0,0,0,0)==-1&&errno==ENOSYS);
    close(directory);assert(!rmdir(path));
}
