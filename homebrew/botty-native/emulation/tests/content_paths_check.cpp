#include "game_content_paths.h"
#include <cassert>
int main() {
    using Eden::GameContentPaths;
    const std::string root="/data/botty/emulation/switch/";
    assert(Eden::GameFolderPath(root+"roms/Base.nsp",root+"roms").empty());
    assert(Eden::GameFolderPath("/other/Game.nsp",root+"roms").empty());
    assert(Eden::GameFolderPath(root+"roms/Collection/Game/Base.nsp",root+"roms")==root+"roms/Collection/Game");
    assert(Eden::GameFolderPath(root+"roms/.botty-batch-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/Game/Base.nsp",root+"roms")==root+"roms/.botty-batch-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/Game");
    assert(GameContentPaths(root+"roms/Base.nsp",root+"roms",root+"updates")==std::vector<std::string>{root+"updates"});
    assert((GameContentPaths(root+"roms/Game/Base.nsp",root+"roms",root+"updates")==std::vector<std::string>{root+"updates",root+"roms/Game"}));
    assert((GameContentPaths(root+"roms/.botty-batch-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/Base.xci",root+"roms",root+"updates")==std::vector<std::string>{root+"updates",root+"roms/.botty-batch-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}));
}
