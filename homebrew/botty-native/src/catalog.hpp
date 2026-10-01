// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model.hpp"
#include <array>
namespace botty {
struct Entry {
    std::array<char,96> id{};
    std::array<char,512> name{}, error{}, destination{}, phase{};
    std::array<char,32> status{}, kind{};
    std::array<char,2048> files{};
    unsigned fileCount=0,archiveStart=0,archiveCount=0;
    bool dismissed=false,extractable=false,archivesOmitted=false;
    double bytes=0,total=0,download=0,upload=0,progress=0;
    double eta=-1;
    int completedCount=0;
    std::array<char,40> published{};
    int peers=-1,downloadingPeers=-1,uploadingPeers=-1;
    bool etaEstimated=false,complete=false,active=false;
};
struct Catalog {
    std::array<Entry,256> torrents{}, jobs{};
    std::array<std::array<char,4096>,512> archives{};
    std::array<Entry,100> results{};
    unsigned resultCount=0;
    std::array<Entry,100> exploreResults{};
    unsigned exploreCount=0;
    bool exploreSupported=false,exploreBusy=false,exploreAdding=false;
    std::array<char,32> exploreSort{};
    std::array<char,512> exploreError{},exploreNotice{};
    bool searchSupported=false,searchBusy=false,searchAdding=false;
    std::array<char,512> searchQuery{},searchError{},searchNotice{};
    unsigned torrentCount=0,jobCount=0,revision=0,archiveCount=0;
    bool extracting=false,extractionControls=false,torrentRemovalSupported=false;
    bool valid=false,transmissionReady=false,truncated=false,catalogArtworkSupported=false;
    double freeBytes=0;
    std::array<char,512> library{},error{};
};
bool responseObject(std::string_view,std::array<char,512>& error) noexcept;
bool firstArchive(std::string_view) noexcept;
bool parseCatalog(std::string_view,Catalog&) noexcept;
unsigned entryCount(const Catalog&,unsigned tab,unsigned filter) noexcept;
const Entry* entryAt(const Catalog&,unsigned tab,unsigned filter,unsigned index) noexcept;
void formatBytes(double,char*,unsigned) noexcept;
void formatETA(const Entry&,char*,unsigned) noexcept;
}
