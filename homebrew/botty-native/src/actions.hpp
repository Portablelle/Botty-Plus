// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "catalog.hpp"
namespace botty {
enum class Operation { none, pause, resume, verify, add, extract, move, remove, cancel, dismiss };
struct Command {
    Operation operation=Operation::none;
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
struct Workflow {
    enum class Panel { closed, menu, archives, keyboard, confirm } panel=Panel::closed;
    unsigned selected=0,keyPage=0,revision=0,archiveIndex=0;
    bool confirm=false,passwordVisible=false,unicodeInput=false;
    std::array<char,7> codepoint{};
    std::array<char,512> targetName{},notice{};
    std::array<char,96> targetId{};
    unsigned targetTab=0;
    Command command{};
    std::array<Operation,6> options{};
    unsigned optionCount=0;
    void open(const Entry*,unsigned,const Catalog&) noexcept;
    void add() noexcept;
    void close() noexcept;
    const Entry* target(const Catalog&) const noexcept;
    // True emits one confirmed command. Caller queues it, then clears input.
    bool press(unsigned,const Catalog&,bool busy) noexcept;
    static std::string_view keys(unsigned page) noexcept;
    void append(char) noexcept;
    void erase() noexcept;
    bool finishUnicode() noexcept;
};
}
