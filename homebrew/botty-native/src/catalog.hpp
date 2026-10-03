// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model.hpp"
#include <array>
namespace botty {
struct Entry {
    std::array<char,96> id{};
    std::array<char,64> storage{},sourceStorage{},compressedStorage{};
    std::array<char,512> name{}, error{}, destination{}, phase{};
    std::array<char,32> status{}, kind{}, titleId{};
    std::array<char,2048> files{};
    unsigned fileCount=0,archiveStart=0,archiveCount=0;
    bool task=false,items=false;
    double elapsed=0;
    std::array<char,512> currentFile{};
    bool compressed=false,originalKept=false,compressionVerified=false;
    std::array<char,32> compressionState{};
    bool dismissed=false,extractable=false,archivesOmitted=false;
    double bytes=0,total=0,download=0,upload=0,progress=0;
    double eta=-1;
    int completedCount=0;
    std::array<char,40> published{};
    int peers=-1,downloadingPeers=-1,uploadingPeers=-1;
    bool etaEstimated=false,complete=false,active=false;
};
struct StorageDevice {std::array<char,64> id{},label{};double freeBytes=0;bool available=false;};
struct Processing {
    std::array<Entry,4> tasks{};unsigned count=0,revision=0;bool stale=false;
};
bool parseProcessing(std::string_view,Processing&) noexcept;
struct Catalog {
    Processing processing;
    std::array<char,512> transferPhase{},transferError{};
    bool transferring=false;
    bool storageSupported=false;
    unsigned storageCount=0;
    std::array<StorageDevice,9> storage{};
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
    bool extracting=false,extractionControls=false,torrentRemovalSupported=false,libraryDeletionSupported=false;
    bool valid=false,stale=false,transmissionStale=false,transmissionReady=false,truncated=false,catalogArtworkSupported=false;
    bool compressionSupported=false,compressionBusy=false,compressedDeletionSupported=false;
    std::array<char,96> compressionJob{};
    std::array<char,32> compressionStatus{};
    std::array<char,512> compressionPhase{},compressionError{};
    double compressionBytes=0,compressionTotal=0,compressedSize=0;
    double freeBytes=0;
    std::array<char,512> library{},error{};
};
bool responseObject(std::string_view,std::array<char,512>& error) noexcept;
bool firstArchive(std::string_view) noexcept;
bool parseCatalog(std::string_view,Catalog&) noexcept;
unsigned entryCount(const Catalog&,unsigned tab,unsigned filter) noexcept;
const Entry* entryAt(const Catalog&,unsigned tab,unsigned filter,unsigned index) noexcept;
struct LibraryCopy { const char* format; const char* storage; bool retained=false; };
unsigned libraryCopies(const Entry&,std::array<LibraryCopy,2>&) noexcept;
void storageLabel(const Catalog&,std::string_view,char*,unsigned) noexcept;
void formatBytes(double,char*,unsigned) noexcept;
void formatETA(const Entry&,char*,unsigned) noexcept;
}
