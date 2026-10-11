// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "catalog.hpp"
namespace botty {
void formatDeletionEstimate(double bytes,char* out,unsigned size) noexcept;
enum class Operation { none, pause, resume, verify, add, extract, move, remove, cancel, dismiss, search, grab, explore, exploreGrab, removeTorrent, removeLibrary, compress, cancelCompression, removeOriginal, restoreOriginal, transfer, nativeUpdate, checkNativeUpdate };
struct Command {
    Operation operation=Operation::none;
    bool refresh=false,automatic=true,torrent=false;
    std::array<char,64> storage{};
    std::array<char,65> serviceVersion{};
    std::array<char,96> id{};
    std::array<char,4096> archive{};
    std::array<char,16385> text{}; // magnet or archive password; never logged
};
struct ActionResult {
    enum class Status { idle, success, failed, uncertain } status=Status::idle;
    std::array<char,512> message{};
    unsigned revision=0;
};
const char* operationLabel(Operation) noexcept;
const char* actionPath(Operation) noexcept;
bool encodeCommand(const Command&,char*,std::size_t,std::size_t&) noexcept;
ActionResult performCommand(const Command&) noexcept;
const char* unavailable(Operation,const Entry*,const Catalog&) noexcept;
// Quick actions for one entry (0 download, 1 Activity job, 2 Library game),
// destructive choices last. Followed tasks have none.
unsigned quickActions(const Entry*,unsigned tab,const Catalog&,std::array<Operation,8>&) noexcept;
struct Workflow {
    // menu: Quick actions with inline destructive confirmation. sheet: one settings
    // sheet for Get game, Extract and disk moves. keyboard: text entry.
    enum class Panel { closed, menu, sheet, keyboard } panel=Panel::closed;
    enum class Row { source, magnet, storage, mode, archive, password };
    unsigned selected=0,keyPage=0,revision=0,archiveIndex=0;
    // Sheet focus covers its rows, then the primary button, which starts focused.
    unsigned focus=0,rowCount=0,sourceIndex=0;
    std::array<Row,4> rows{};
    bool confirming=false,confirm=false,passwordVisible=false,unicodeInput=false;
    std::array<char,7> codepoint{};
    std::array<char,512> targetName{},notice{};
    std::array<char,96> targetId{};
    // Target source: 0 download, 1 Activity job, 2 Library game.
    unsigned targetTab=0,sourceCount=0;
    std::array<DownloadSource,32> sources{};
    // The game a Get game sheet opened for; catalog refreshes cannot change it.
    std::array<char,512> gameName{};
    std::array<char,96> gameId{};
    Command command{};
    std::array<Operation,8> options{};
    unsigned optionCount=0;
    // The last disk and download mode are remembered for this launch.
    std::array<char,64> lastStorage{};
    bool lastAutomatic=true;
    void open(const Entry*,unsigned,const Catalog&) noexcept;
    // Starts one Quick action. True emits a safe action that runs immediately.
    bool choose(Operation,const Catalog&,bool busy) noexcept;
    void chooseSources(const Entry&,const Catalog&,bool compare=false) noexcept;
    void add() noexcept;
    void search(std::string_view previous={}) noexcept;
    void grab(const Entry&,const Catalog&) noexcept;
    void close() noexcept;
    const Entry* target(const Catalog&) const noexcept;
    bool hasRow(Row) const noexcept;
    // True emits one confirmed command. Caller queues it, then clears input.
    bool press(unsigned,const Catalog&,bool busy) noexcept;
    bool acceptText(std::string_view,const Catalog&,bool busy) noexcept;
    bool finishInput(const Catalog&,bool busy) noexcept;
    static unsigned textLimit(Operation) noexcept;
    static std::string_view keys(unsigned page) noexcept;
    static bool immediate(Operation) noexcept;
    static bool destructive(Operation) noexcept;
    void append(char) noexcept;
    void erase() noexcept;
    bool finishUnicode() noexcept;
private:
    std::array<char,1025> savedPassword{};
    void openSheet(const Catalog&) noexcept;
    void change(Row,bool forward,const Catalog&) noexcept;
    void editPassword() noexcept;
    bool submit(const Catalog&,bool busy) noexcept;
};
}
