// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model.hpp"
#include "catalog.hpp"
#include "actions.hpp"
#include <atomic>
#include <array>
namespace botty {
struct Connection {
    Probe status=Probe::checking;
    std::array<char,64> url{};
    std::array<char,16> username{};
    std::array<char,33> password{};
    unsigned revision=0;
};
Probe probeService() noexcept;
Connection probeConnection(Catalog* catalog=nullptr) noexcept;
bool parseConnection(std::string_view,Connection&) noexcept;
class Network final {
public:
    enum class Deletion { idle, deleting, checking };
private:
    std::atomic<bool> stop_{false}, retry_{false};
    std::atomic<Probe> state_{Probe::checking};
    std::atomic_flag gate_=ATOMIC_FLAG_INIT;
    Connection connection_{};
    Catalog catalog_{};
    Command pending_{};
    ActionResult result_{};
    std::atomic<bool> busy_{false},queued_{false};
    std::atomic<Deletion> deletion_{Deletion::idle};
    void* thread_=nullptr;
    static void* worker(void*) noexcept;
    void publish(Connection,const Catalog* catalog=nullptr) noexcept;
public:
    bool start() noexcept;
    void stop() noexcept;
    bool submit(const Command&) noexcept;
    bool busy() const noexcept {return busy_.load();}
    Deletion deletion() const noexcept {return deletion_.load();}
    void retry() noexcept { retry_.store(true); }
    Probe state() const noexcept { return state_.load(); }
    // Rendering never waits on the worker. Keep the previous snapshot if busy.
    bool read(Connection& out,Catalog* catalog=nullptr,ActionResult* result=nullptr) noexcept {
        if(gate_.test_and_set(std::memory_order_acquire))return false;
        out=connection_;if(result)*result=result_;if(catalog&&catalog->revision!=catalog_.revision)*catalog=catalog_;gate_.clear(std::memory_order_release);return true;
    }
};
using CoverIds=std::array<std::array<char,96>,6>;
struct ArtworkPage {
    CoverIds ids{};
    std::array<std::array<unsigned char,160*240*3>,6> pixels{};
    std::array<bool,6> ready{};
    unsigned revision=0;
};
class Artwork final {
    std::atomic<bool> stop_{false};
    std::atomic_flag gate_=ATOMIC_FLAG_INIT;
    CoverIds requested_{};
    unsigned requestRevision_=0;
    ArtworkPage page_{};
    void* thread_=nullptr;
    static void* worker(void*) noexcept;
public:
    void start() noexcept;
    void stop() noexcept;
    void request(const CoverIds&) noexcept;
    bool read(ArtworkPage&) noexcept;
};

}
