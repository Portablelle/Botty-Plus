// SPDX-License-Identifier: GPL-3.0-or-later
#include "model.hpp"
#include "probe.hpp"
#include "platform.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <pthread.h>
#include <vector>
namespace {
thread_local bool progressWorker=false;
thread_local std::string progressRequest,progressResponse;
thread_local std::size_t progressOffset=0;
unsigned workerStarts=0;
std::string response, request;
std::vector<std::string> responses;unsigned requests=0;
std::size_t offset=0, chunk=7;
int closes=0;
bool connected=true, receiveError=false, workerAllowed=false;
std::uint64_t clockUs=0, receiveCost=100;
std::string wire(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}
void reset(std::string data) {
    workerStarts=0;response=std::move(data);request.clear();offset=0;closes=0;clockUs=0;
    responses.clear();requests=0;connected=true;receiveError=false;receiveCost=100;chunk=7;
}
}
namespace botty::platform {
std::uint64_t now() noexcept {if(progressWorker)return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();return clockUs;}
void sleep(unsigned n) noexcept {if(workerAllowed)std::this_thread::sleep_for(std::chrono::microseconds(n));else clockUs+=n;}
void log(const char*) noexcept {}
int connectLocal() noexcept {if(progressWorker){progressRequest.clear();progressResponse.clear();progressOffset=0;return 4;}if(requests<responses.size())response=responses[requests];++requests;offset=0;return connected?3:-1;}
int send(int,const void* bytes,std::size_t size) noexcept {
    if(progressWorker){progressRequest.append(static_cast<const char*>(bytes),size);return int(size);}
    const auto n=std::min(size,chunk);request.append(static_cast<const char*>(bytes),n);return static_cast<int>(n);
}
int receive(int,void* bytes,std::size_t size) noexcept {
    if(progressWorker){
        if(progressResponse.empty())progressResponse=wire(progressRequest.find("GET /api/bootstrap ")==0?R"({"apiVersion":1,"token":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})":R"({"tasks":[{"id":"42","kind":"deletion","name":"Fixture","status":"running","phase":"Deleting files","unit":"items","bytes":2,"total":10,"eta":8,"rate":1}]})");
        const auto n=std::min(size,progressResponse.size()-progressOffset);std::memcpy(bytes,progressResponse.data()+progressOffset,n);progressOffset+=n;return int(n);
    }
    clockUs+=receiveCost;
    if(response=="#long-timeout"){clockUs+=121000000;return -1;}
    if(receiveError||response=="#timeout")return -1;
    const auto n=std::min({size,chunk,response.size()-offset});
    std::memcpy(bytes,response.data()+offset,n);offset+=n;return static_cast<int>(n);
}
void closeSocket(int) noexcept {if(!progressWorker)++closes;}
struct WorkerStart {void* (*fn)(void*);void* context;bool progress;pthread_t thread;};
bool startWorker(void* (*fn)(void*),void* context,void** handle) noexcept {
    if(!workerAllowed)return false;
    auto* worker=new WorkerStart{fn,context,(++workerStarts)==2,{}};
    pthread_attr_t attr;assert(pthread_attr_init(&attr)==0);
    // Reproduce the PS5 budget: the previous Processing buffers overflowed it.
    if(worker->progress)assert(pthread_attr_setstacksize(&attr,64*1024)==0);
    const int rc=pthread_create(&worker->thread,&attr,[](void* arg)->void*{
        auto* w=static_cast<WorkerStart*>(arg);progressWorker=w->progress;return w->fn(w->context);
    },worker);
    pthread_attr_destroy(&attr);if(rc){delete worker;return false;}*handle=worker;return true;
}
void joinWorker(void* handle) noexcept {auto* worker=static_cast<WorkerStart*>(handle);pthread_join(worker->thread,nullptr);delete worker;}
}
int main() {
    using namespace botty;
    // Each Network owns a full catalog. Keep test instances off the main stack;
    // the app uses global storage and the Processing worker still has 64 KiB.
    Processing processing;
    assert(parseProcessing(R"({"tasks":[{"id":"move-a","kind":"transfer","status":"running","bytes":25,"total":100,"rate":5,"eta":15,"phase":"Moving game to selected disk"}]})",processing));
    assert(processing.tasks[0].active&&processing.tasks[0].progress==.25&&processing.tasks[0].eta==15);
    assert(parseProcessing(R"({"tasks":[{"id":"move-a","kind":"transfer","status":"completed","bytes":100,"total":100}]})",processing));
    assert(processing.tasks[0].complete&&!processing.tasks[0].active);

    assert(parseProcessing(R"({"tasks":[{"id":"task-a","kind":"compression","status":"waiting-close","bytes":100,"total":100,"eta":0}]})",processing));
    assert(processing.tasks[0].active&&processing.tasks[0].total==0&&processing.tasks[0].eta==-1);
    assert(!parseProcessing(R"({"tasks":[{"name":"missing ID"}]})",processing));
    static Catalog control;control.processing=processing;control.jobCount=1;control.jobs[0].complete=true;
    assert(entryCount(control,1,0)==2&&entryAt(control,1,0,0)->task);
    Workflow monitored;monitored.open(entryAt(control,1,0,0),1,control);assert(monitored.panel==Workflow::Panel::closed);
    char estimate[160];
    formatDeletionEstimate(250122412221.0,estimate,sizeof(estimate));assert(std::string_view(estimate).find("5-10 min")!=std::string_view::npos);
    formatDeletionEstimate(100000000000.0,estimate,sizeof(estimate));assert(std::string_view(estimate).find("2-4 min")!=std::string_view::npos);
    formatDeletionEstimate(0,estimate,sizeof(estimate));assert(std::string_view(estimate).find("several minutes")!=std::string_view::npos);
    const std::string legacy=R"({"app":"Botty","version":"0.1.0","titleId":"BTTY00001"})";
    assert(parseHealth(legacy)==Probe::legacy);
    assert(parseHealth(R"({"app":"Botty","version":"0.2.0","titleId":"BTTY00001","apiVersion":1})")==Probe::ready);
    assert(parseHealth(R"({"app":"Botty","version":"0.2.0","titleId":"BTTY00001","apiVersion":2})")==Probe::incompatible);
    assert(parseHealth(R"({"app":"Other","version":"0.1.0","titleId":"BTTY00001"})")==Probe::incompatible);
    assert(parseHealth(R"({"app":"Botty","app":"Botty"})")==Probe::malformed);
    assert(parseHealth(legacy+"garbage")==Probe::malformed);
    assert(parseHealth(R"({"app":"Botty",})")==Probe::malformed);
    assert(parseHealth(R"({"note":"\"app\":\"Botty\""})")==Probe::malformed);
    for(std::size_t split:{1u,7u,4096u}) {
        reset(wire(legacy));chunk=split;
        assert(probeService()==Probe::legacy);assert(closes==1);
        assert(request.find("Host: 127.0.0.1:8088\r\n")!=std::string::npos);
        assert(request.find("GET /health ")==0);
    }
    reset("");connected=false;assert(probeService()==Probe::unavailable);assert(closes==0);
    reset(wire(legacy));receiveError=true;assert(probeService()==Probe::unavailable);assert(closes==1);
    reset(wire(legacy));response.pop_back();assert(probeService()==Probe::unavailable);assert(closes==1);
    reset("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");assert(probeService()==Probe::rejected);
    reset("HTTP/1.1 500 Error\r\nContent-Length: 0\r\n\r\n");assert(probeService()==Probe::incompatible);
    reset("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");assert(probeService()==Probe::malformed);
    reset("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n12345");assert(probeService()==Probe::malformed);
    reset("HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n");assert(probeService()==Probe::malformed);
    reset(std::string(4096,'a'));assert(probeService()==Probe::malformed);assert(closes==1);
    reset(wire(legacy));receiveCost=1000000;chunk=1;assert(probeService()==Probe::unavailable);assert(closes==1);
    const std::string health=R"({"app":"Botty","version":"0.1.1","titleId":"BTTY00001","apiVersion":1})";
    const std::string boot=R"({"apiVersion":1,"token":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
    const std::string login=R"({"apiVersion":1,"url":"http://192.168.1.50:9091","username":"botty","password":"B7mQ2x"})";
    const std::string rtorrentLogin=R"({"apiVersion":1,"url":"http://192.168.1.50:8088","username":"botty","password":"B7mQ2x"})";
    Connection details;
    assert(parseConnection(login,details));
    assert(parseConnection(rtorrentLogin,details));
    assert(std::string_view(details.url.data())=="http://192.168.1.50:8088");
    assert(!parseConnection(R"({"apiVersion":1,"url":"http://192.168.1.50:5001","username":"botty","password":"B7mQ2x"})",details));
    assert(!parseConnection(R"({"apiVersion":1,"url":"http://192.168.1.50:8088/extra","username":"botty","password":"B7mQ2x"})",details));

    assert(std::string_view(details.password.data())=="B7mQ2x");
    assert(!parseConnection(R"({"apiVersion":1,"url":"http://127.0.0.1:9091","username":"botty","password":"B7mQ2x"})",details));
    assert(!parseConnection(R"({"apiVersion":1,"url":"http://192.168.999.1:9091","username":"botty","password":"B7mQ2x"})",details));
    assert(!parseConnection(R"({"apiVersion":1,"url":"http://192.168.1.1:9091","username":"botty","password":"bad!pw"})",details));
    reset("");responses={wire(health),wire(boot),wire(login)};
    const auto authenticated=probeConnection();
    assert(authenticated.status==Probe::ready&&std::string_view(authenticated.password.data())=="B7mQ2x");
    assert(requests==3&&closes==3);
    assert(request.find("GET /api/connections ")!=std::string::npos);
    assert(request.find("X-Botty-Token: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n")!=std::string::npos);
    reset("");responses={wire(health),wire(boot),"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"};
    const auto rejected=probeConnection();assert(rejected.status==Probe::rejected&&rejected.password[0]==0&&rejected.url[0]==0);
    reset("");responses={wire(health),wire(boot),"HTTP/1.1 400 Error\r\nContent-Length: 0\r\n\r\n"};
    assert(probeConnection().status==Probe::transmissionUnavailable);
    reset(wire(legacy));assert(probeConnection().status==Probe::legacy);assert(requests==1);
    reset(wire(health+std::string(1,'\0')+"garbage"));assert(probeService()==Probe::malformed);
    static Catalog catalog;
    const std::string state=R"({"freeBytes":1099511627776,"library":"/data/library","transmissionReady":true,"torrents":[{"id":9,"name":"A \"quoted\" name \u00e9 \ud83d\ude80","status":4,"peersConnected":42,"peersSendingToUs":8,"peersGettingFromUs":3,"percentDone":0.5,"leftUntilDone":1073741824,"totalSize":2147483648,"rateDownload":1048576,"rateUpload":0,"files":[{"name":"folder/file.rar"}]},{"id":10,"name":"Done","status":0,"percentDone":1,"leftUntilDone":0,"totalSize":100}],"jobs":[{"id":"j1","name":"Ready","status":"ready","content":{"kind":"unsupported","reason":"No recognized content"}},{"id":"j2","name":"Moved","status":"moved","destination":"/data/library/a"},{"id":"j3","name":"Failed","status":"failed","error":"CRC error"}]})";
    reset("");chunk=4096;responses={wire(health),wire(boot),wire(rtorrentLogin),wire(state)};
    assert(probeConnection(&catalog).status==Probe::ready&&catalog.valid&&requests==4&&closes==4);
    assert(catalog.torrentCount==2&&catalog.jobCount==3);
    assert(request.find("GET /api/state ")!=std::string::npos);
    assert(parseCatalog(state,catalog));
    assert(catalog.valid&&catalog.torrentCount==2&&catalog.jobCount==3);
    assert(catalog.freeBytes==1099511627776.0);
    assert(std::string_view(catalog.torrents[0].name.data())=="A \"quoted\" name é 🚀");
    assert(catalog.torrents[0].bytes==1073741824&&catalog.torrents[0].eta==1024&&catalog.torrents[0].etaEstimated);
    assert(catalog.torrents[0].fileCount==1);
    assert(catalog.torrents[0].peers==42&&catalog.torrents[0].downloadingPeers==8&&catalog.torrents[0].uploadingPeers==3);
    assert(catalog.torrents[1].peers==-1&&catalog.torrents[1].downloadingPeers==-1);
    assert(std::string_view(catalog.torrents[0].files.data())=="folder/file.rar\n");
    assert(entryCount(catalog,0,1)==1&&entryCount(catalog,0,2)==1);
    assert(entryCount(catalog,1,0)==2&&entryCount(catalog,2,0)==2);
    assert(std::string_view(entryAt(catalog,2,0,1)->id.data())=="j2");
    {
        static Catalog ordered;
        assert(parseCatalog(R"({"freeBytes":0,"transmissionReady":true,"torrents":[{"id":1,"name":"Seeding","status":6,"leftUntilDone":0},{"id":2,"name":"Paused","status":0,"leftUntilDone":100},{"id":3,"name":"Downloading","status":4,"leftUntilDone":100,"rateDownload":10},{"id":4,"name":"Verifying","status":2,"leftUntilDone":100},{"id":5,"name":"Waiting for peers","status":4,"leftUntilDone":100,"rateDownload":0},{"id":6,"name":"Queued","status":3,"leftUntilDone":100},{"id":7,"name":"Completed","status":0,"leftUntilDone":0},{"id":8,"name":"Finishing","status":4,"leftUntilDone":0}],"jobs":[]})",ordered));
        const unsigned all[]={3,5,1,2,4,6,7,8},active[]={3,5,1,4,6,8},completed[]={1,7,8};
        const auto check=[&](unsigned filter,const auto& ids){
            assert(entryCount(ordered,0,filter)==std::size(ids));
            for(unsigned i=0;i<std::size(ids);++i)assert(std::string_view(entryAt(ordered,0,filter,i)->id.data())==std::to_string(ids[i]));
            assert(entryAt(ordered,0,filter,std::size(ids))==nullptr);
        };
        check(0,all);check(1,active);check(2,completed);
        assert(ordered.torrents[2].downloading&&ordered.torrents[4].downloading&&!ordered.torrents[7].downloading);
        ordered.torrents[2].downloading=false;ordered.torrents[2].active=false;
        assert(std::string_view(entryAt(ordered,0,0,0)->id.data())=="5");
        assert(std::string_view(entryAt(ordered,0,1,0)->id.data())=="5");
    }
    {
        // Activity groups downloads and jobs by state; it never pairs rows by name.
        static Catalog a;assert(parseCatalog(R"({"freeBytes":1,"transmissionReady":true,"torrents":[{"id":1,"name":"Down","status":4,"leftUntilDone":5,"totalSize":10,"rateDownload":1},{"id":2,"name":"Paused","status":0,"leftUntilDone":5,"totalSize":10},{"id":3,"name":"Seed","status":6,"leftUntilDone":0,"totalSize":10},{"id":4,"name":"Broken","status":0,"leftUntilDone":5,"totalSize":10,"errorString":"Tracker error"}],"jobs":[{"id":"j1","name":"Down","status":"extracting","bytes":1,"total":2},{"id":"j2","name":"Ready","status":"ready","content":{"kind":"folder"}},{"id":"j3","name":"Failed","status":"failed","error":"CRC error"},{"id":"j4","name":"Moved","status":"moved","content":{"kind":"folder"}},{"id":"j5","name":"Hidden","status":"ready","dismissed":true,"content":{"kind":"folder"}}]})",a));
        assert(activityCount(a,0)==7&&activityCount(a,1)==2&&activityCount(a,2)==2&&activityCount(a,3)==2);
        const char* order[]={"1","j1","4","j3","2","3","j2"};
        for(unsigned i=0;i<7;++i)assert(std::string_view(activityAt(a,0,i).entry->id.data())==order[i]);
        assert(activityAt(a,0,0).torrent&&!activityAt(a,0,1).torrent&&!activityAt(a,0,7).entry);
        assert(activityState(a.torrents[2],true)==ActivityState::done&&activityState(a.jobs[1],false)==ActivityState::done);
        // A Processing task replaces its job row; waiting for Botty+ to close needs the user.
        a.processing.count=1;a.processing.tasks[0]=Entry{};a.processing.tasks[0].task=true;a.processing.tasks[0].active=true;a.processing.tasks[0].id=a.jobs[0].id;
        std::snprintf(a.processing.tasks[0].status.data(),a.processing.tasks[0].status.size(),"waiting-close");
        assert(activityCount(a,0)==7&&activityCount(a,2)==3&&activityAt(a,2,1).entry==&a.processing.tasks[0]);
        // Library keeps dismissed rows and filters by disk or compression.
        assert(libraryCount(a,0)==3&&libraryCount(a,1)==3&&libraryCount(a,2)==0&&libraryCount(a,3)==0);
        std::snprintf(a.jobs[3].storage.data(),a.jobs[3].storage.size(),"external-x");
        assert(libraryCount(a,2)==1&&std::string_view(libraryAt(a,2,0)->id.data())=="j4"&&libraryCount(a,1)==2&&!libraryAt(a,2,1));
        // A restored original keeps its compressed image, so Compressed still lists it.
        std::snprintf(a.jobs[3].kind.data(),a.jobs[3].kind.size(),"folder");std::snprintf(a.jobs[3].compressionState.data(),a.jobs[3].compressionState.size(),"restored");
        assert(!a.jobs[3].compressed&&libraryCount(a,3)==1&&std::string_view(libraryAt(a,3,0)->id.data())=="j4");
        // Queued downloads wait; only transferring or verifying work counts as running.
        std::snprintf(a.torrents[0].status.data(),a.torrents[0].status.size(),"Queued");
        assert(activityState(a.torrents[0],true)==ActivityState::waiting&&activityCount(a,1)==0);
    }
    char formatted[64];formatETA(catalog.torrents[0],formatted,sizeof(formatted));assert(std::string_view(formatted)=="ETA ~17 min");
    formatETA(catalog.torrents[1],formatted,sizeof(formatted));assert(std::string_view(formatted)=="Completed");
    auto paused=catalog.torrents[0];paused.eta=-1;formatETA(paused,formatted,sizeof(formatted));assert(std::string_view(formatted)=="ETA unavailable");
    paused.eta=90061;paused.etaEstimated=false;formatETA(paused,formatted,sizeof(formatted));assert(std::string_view(formatted)=="ETA 1 d 1 h");
    formatBytes(1073741824,formatted,sizeof(formatted));assert(std::string_view(formatted)=="1.0 GiB");
    assert(!parseCatalog(state+"x",catalog)&&!catalog.valid);
    assert(!parseCatalog(R"({"torrents":[],"jobs":[],"transmissionReady":"true","freeBytes":0})",catalog));
    assert(!parseCatalog(R"({"torrents":[{"name":"\uZZZZ"}],"jobs":[]})",catalog));
    std::string huge=R"({"freeBytes":0,"library":"/data/library","transmissionReady":false,"torrents":[],"jobs":[)";
    for(unsigned i=0;i<300;++i){if(i)huge+=',';huge+=R"({"id":")"+std::to_string(i)+R"(","name":"Job","status":"ready"})";}
    huge+="]}";assert(parseCatalog(huge,catalog)&&catalog.jobCount==256&&catalog.truncated&&!catalog.transmissionReady);
    reset("");chunk=4096;responses={wire(health),wire(boot),wire(login),wire(huge)};
    assert(probeConnection(&catalog).status==Probe::ready&&catalog.valid&&requests==4&&closes==4);
    reset("");chunk=4096;responses={wire(health),wire(boot),"HTTP/1.1 400 Error\r\nContent-Length: 0\r\n\r\n",wire(huge)};
    assert(probeConnection(&catalog).status==Probe::transmissionUnavailable&&catalog.valid);
    reset("");responses={wire(health),wire(boot),wire(login),"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"};
    assert(probeConnection(&catalog).status==Probe::rejected&&!catalog.valid);
    Model browse;browse.tab=Model::activity;browse.count=12;browse.press(Buttons::down);assert(browse.selected==1);
    browse.press(Buttons::cross);assert(browse.details);browse.press(Buttons::circle);assert(!browse.details);
    browse.press(Buttons::right);assert(browse.filter==1&&browse.selected==0);
    browse.press(Buttons::left);browse.press(Buttons::left);assert(browse.filter==3);
    browse.press(Buttons::r1);assert(browse.tab==Model::library&&browse.selected==0&&browse.filter==0);
    browse.press(Buttons::l1);assert(browse.tab==Model::activity);
    assert(browse.press(Buttons::options)==Model::Action::menu);
    assert(browse.press(Buttons::square)==Model::Action::add);
    // Circle never opens a quit dialog: Close Botty+ lives in System.
    for(unsigned tab=0;tab<Model::tabCount;++tab){Model root;root.tab=tab;root.count=3;assert(root.press(Buttons::circle)==Model::Action::none&&root.tab==tab&&!root.details);}
    {
        Model tabs;assert(tabs.tab==Model::discover);tabs.press(Buttons::l1);assert(tabs.tab==Model::system);
        tabs.press(Buttons::r1);tabs.press(Buttons::r1);tabs.press(Buttons::r1);assert(tabs.tab==Model::library);
        // Details buttons move left/right; files page up/down; Cross runs the focused button.
        tabs.count=4;tabs.press(Buttons::cross);assert(tabs.details&&tabs.detailButton==0);tabs.buttonCount=3;
        tabs.press(Buttons::right);tabs.press(Buttons::right);tabs.press(Buttons::right);assert(tabs.detailButton==2);
        tabs.press(Buttons::down);assert(tabs.detailPage==1);assert(tabs.press(Buttons::cross)==Model::Action::run);
        assert(tabs.press(Buttons::options)==Model::Action::menu);tabs.press(Buttons::circle);assert(!tabs.details);
    }
    // Native commands encode exact paths/passwords and never auto-replay a POST.
    Command command;command.operation=Operation::extract;std::snprintf(command.id.data(),command.id.size(),"9");
    std::snprintf(command.archive.data(),command.archive.size(),"folder/a\"b\\c.rar");
    std::snprintf(command.text.data(),command.text.size(),"a\"b\\c\n");
    std::array<char,131072> encoded{};std::size_t encodedSize=0;
    assert(encodeCommand(command,encoded.data(),encoded.size(),encodedSize));
    assert(std::string_view(encoded.data())==R"({"id":9,"archive":"folder/a\"b\\c.rar","password":"a\"b\\c\u000a"})");
    assert(!encodeCommand(command,encoded.data(),5,encodedSize));
    reset("");responses={wire(boot),"HTTP/1.1 202 Accepted\r\nContent-Length: 18\r\n\r\n{\"status\":\"ready\"}"};
    assert(performCommand(command).status==ActionResult::Status::success&&requests==2);
    assert(request.find("POST /api/extract ")!=std::string::npos&&request.find("Content-Type: application/json")!=std::string::npos);
    assert(request.find("X-Botty-Token: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")!=std::string::npos);
    reset("");auto failed=wire(R"({"error":"Missing volume \"part2.rar\""})");failed.replace(9,3,"400");responses={wire(boot),failed};
    auto error=performCommand(command);assert(error.status==ActionResult::Status::failed&&std::string_view(error.message.data())=="Missing volume \"part2.rar\"");
    reset("");responses={wire(boot),""};assert(performCommand(command).status==ActionResult::Status::uncertain&&requests==2);
    reset("");responses={wire(boot),wire("not JSON")};assert(performCommand(command).status==ActionResult::Status::uncertain&&requests==2);
    reset("");failed=wire(R"({"error":"Access rejected"})");failed.replace(9,3,"403");responses={wire(boot),failed};assert(performCommand(command).status==ActionResult::Status::failed&&requests==2);
    reset("");connected=false;assert(performCommand(command).status==ActionResult::Status::failed&&requests==1);
    assert(firstArchive("a.rar")&&firstArchive("a.part01.rar")&&firstArchive("a.PaRt1.rar"));
    assert(!firstArchive("a.part2.rar")&&!firstArchive("a.r00")&&!firstArchive("a.RAR"));
    assert(parseCatalog(R"({"freeBytes":0,"library":"/data/homebrew","transmissionReady":true,"torrents":[],"jobs":[{"id":"eta","name":"Extracting","status":"extracting","bytes":1000000000,"total":2000000000,"extractionRate":10000000,"eta":100}]})",catalog));
    assert(catalog.jobs[0].active&&catalog.jobs[0].bytes==1000000000&&catalog.jobs[0].download==10000000&&catalog.jobs[0].eta==100&&catalog.jobs[0].etaEstimated);
    assert(parseCatalog(R"({"freeBytes":0,"library":"/data/homebrew","transmissionReady":true,"torrents":[],"jobs":[{"id":"old","name":"Legacy","status":"extracting","bytes":100,"total":200}]})",catalog));
    assert(catalog.jobs[0].eta==-1&&catalog.jobs[0].download==0);
    catalog.jobs[0].dismissed=true;assert(entryCount(catalog,1,0)==0);
    std::snprintf(catalog.jobs[0].status.data(),catalog.jobs[0].status.size(),"ready");assert(entryCount(catalog,2,0)==1);
    Command cancel;cancel.operation=Operation::cancel;std::snprintf(cancel.id.data(),cancel.id.size(),"job-id");char cancelEncoded[256];std::size_t cancelEncodedSize=0;
    assert(encodeCommand(cancel,cancelEncoded,sizeof(cancelEncoded),cancelEncodedSize));assert(std::string_view(cancelEncoded)==R"({"id":"job-id"})");assert(std::string_view(actionPath(cancel.operation))=="/api/cancel-extraction");
    cancel.operation=Operation::dismiss;assert(encodeCommand(cancel,cancelEncoded,sizeof(cancelEncoded),cancelEncodedSize));assert(std::string_view(actionPath(cancel.operation))=="/api/dismiss-extraction");
    const std::string actionable=R"({"freeBytes":0,"library":"/data/homebrew","transmissionReady":true,"extracting":false,"torrents":[{"id":9,"name":"Archive","status":0,"error":0,"leftUntilDone":0,"files":[{"name":"one.part1.rar"},{"name":"one.part2.rar"},{"name":"two\".rar"},{"name":"with\nnewline.rar"}]}],"jobs":[{"id":"j1","name":"Ready","status":"ready","content":{"kind":"folder","destination":"demo"}},{"id":"j2","name":"Unsupported","status":"ready","content":{"kind":"unsupported"}},{"id":"j3","name":"Moved","status":"moved"}]})";
    assert(parseCatalog(actionable,catalog));assert(catalog.torrents[0].extractable&&catalog.torrents[0].archiveCount==3);
    assert(std::string_view(catalog.archives[2].data())=="with\nnewline.rar");
    assert(!*unavailable(Operation::extract,&catalog.torrents[0],catalog));
    assert(!*unavailable(Operation::move,&catalog.jobs[0],catalog));
    assert(*unavailable(Operation::move,&catalog.jobs[1],catalog));assert(*unavailable(Operation::remove,&catalog.jobs[2],catalog));
    catalog.extracting=true;assert(*unavailable(Operation::extract,&catalog.torrents[0],catalog));assert(*unavailable(Operation::move,&catalog.jobs[0],catalog));catalog.extracting=false;
    Workflow flow;flow.open(&catalog.torrents[0],0,catalog);assert(flow.options[0]==Operation::resume);
    // One Extract sheet: archive, optional password, and the button focused first.
    flow.press(Buttons::down,catalog,false);flow.press(Buttons::down,catalog,false);flow.press(Buttons::cross,catalog,false);
    assert(flow.panel==Workflow::Panel::sheet&&flow.rowCount==2&&flow.focus==2&&flow.hasRow(Workflow::Row::password));
    flow.press(Buttons::up,catalog,false);flow.press(Buttons::up,catalog,false);flow.press(Buttons::right,catalog,false);assert(flow.archiveIndex==1);
    flow.press(Buttons::left,catalog,false);flow.press(Buttons::left,catalog,false);assert(flow.archiveIndex==0);flow.press(Buttons::right,catalog,false);
    flow.press(Buttons::square,catalog,false);assert(flow.panel==Workflow::Panel::keyboard&&flow.command.operation==Operation::extract);
    flow.append('x');flow.press(Buttons::circle,catalog,false);assert(flow.panel==Workflow::Panel::sheet&&!flow.command.text[0]); // Cancel keeps the old password.
    flow.press(Buttons::square,catalog,false);
    flow.append('s');flow.append('e');flow.append('c');flow.erase();assert(std::string_view(flow.command.text.data())=="se");
    flow.selected=48;flow.press(Buttons::cross,catalog,false);assert(flow.panel==Workflow::Panel::sheet&&flow.focus==flow.rowCount);
    assert(!flow.press(Buttons::cross,catalog,true)&&flow.notice[0]&&flow.panel==Workflow::Panel::sheet);
    assert(flow.press(Buttons::cross,catalog,false));
    assert(std::string_view(flow.command.archive.data())=="two\".rar"&&std::string_view(flow.command.text.data())=="se");
    flow.close();assert(!flow.command.text[0]);
    flow.open(&catalog.jobs[0],1,catalog);flow.press(Buttons::cross,catalog,false);flow.press(Buttons::right,catalog,false);
    catalog.jobCount=0;assert(!flow.press(Buttons::cross,catalog,false));assert(flow.notice[0]); // Removed target cannot redirect to another job.
    flow.add();flow.command.text.fill(0);flow.selected=48;flow.press(Buttons::cross,catalog,false);assert(flow.panel==Workflow::Panel::keyboard&&flow.notice[0]);
    flow.unicodeInput=true;flow.append('e');flow.append('9');assert(flow.finishUnicode());assert(std::string_view(flow.command.text.data())=="é");flow.erase();assert(!flow.command.text[0]);
    flow.unicodeInput=true;for(char c:std::string_view("1f680"))flow.append(c);assert(flow.finishUnicode());assert(std::string_view(flow.command.text.data())=="🚀");flow.erase();assert(!flow.command.text[0]);
    flow.unicodeInput=true;for(char c:std::string_view("d800"))flow.append(c);assert(!flow.finishUnicode());flow.close();
    flow.add();assert(!flow.sourceCount&&!flow.targetName[0]); // No size or name carried over from another sheet.
    assert(!flow.acceptText("magnet:?xt=urn:btih:",catalog,false));assert(flow.panel==Workflow::Panel::keyboard);
    assert(!flow.acceptText("magnet:?xt=urn:btih:abcdef&dn=Game",catalog,false));
    assert(flow.panel==Workflow::Panel::sheet&&flow.hasRow(Workflow::Row::magnet)&&flow.focus==flow.rowCount);
    flow.press(Buttons::up,catalog,false);flow.press(Buttons::cross,catalog,false);assert(flow.panel==Workflow::Panel::keyboard); // Edit the link again.
    flow.selected=48;flow.press(Buttons::cross,catalog,false);assert(flow.panel==Workflow::Panel::sheet&&flow.rowCount==1);
    assert(flow.press(Buttons::cross,catalog,false)&&flow.command.operation==Operation::add);
    flow.close();flow.search();catalog.searchSupported=true;catalog.stale=false;catalog.valid=true;
    assert(!flow.acceptText("",catalog,false)&&flow.panel==Workflow::Panel::keyboard);
    assert(!flow.acceptText(std::string(201,'x'),catalog,false)&&flow.command.text[0]==0);
    flow.append('a');flow.unicodeInput=true;flow.append('e');
    assert(!flow.acceptText("busy query",catalog,true)&&flow.panel==Workflow::Panel::keyboard);
    assert(std::string_view(flow.command.text.data())=="a"&&flow.unicodeInput&&std::string_view(flow.codepoint.data())=="e");
    assert(flow.acceptText("Pokémon",catalog,false)&&flow.panel==Workflow::Panel::closed);
    assert(std::string_view(flow.command.text.data())=="Pokémon");
    flow.close();flow.panel=Workflow::Panel::keyboard;flow.command.operation=Operation::extract;
    assert(!flow.acceptText("",catalog,false)&&flow.panel==Workflow::Panel::sheet); // Empty passwords are allowed.
    flow.close();
    for(const auto op:{Operation::search,Operation::extract,Operation::add}){
        flow.close();flow.panel=Workflow::Panel::keyboard;flow.command.operation=op;
        const unsigned limit=Workflow::textLimit(op);
        assert(limit==(op==Operation::search?200U:op==Operation::extract?1024U:16384U));
        const std::string original=op==Operation::add?"magnet:?xt=urn:btih:original":"original";
        std::copy(original.begin(),original.end(),flow.command.text.begin());
        assert(!flow.acceptText(std::string(limit+1,'x'),catalog,false));
        assert(std::string_view(flow.command.text.data())==original&&flow.panel==Workflow::Panel::keyboard);
        assert(!flow.acceptText(std::string_view("a\0b",3),catalog,false));
        assert(std::string_view(flow.command.text.data())==original&&flow.panel==Workflow::Panel::keyboard);
        std::string boundary=op==Operation::add?"magnet:?xt=urn:btih:":"";boundary.resize(limit,'x');
        const bool emitted=flow.acceptText(boundary,catalog,false);
        assert(emitted==(op==Operation::search)&&std::string_view(flow.command.text.data())==boundary);
        assert(flow.panel==(op==Operation::search?Workflow::Panel::closed:Workflow::Panel::sheet));
    }
    flow.close();
    bool printable[127]={};printable[' ']=true;
    for(unsigned page=0;page<3;++page){assert(Workflow::keys(page).size()==40);for(char c:Workflow::keys(page))printable[static_cast<unsigned char>(c)]=true;}
    for(unsigned c=32;c<127;++c)assert(printable[c]);
    auto workerOwner=std::make_unique<Network>();auto& worker=*workerOwner;assert(!worker.start());assert(worker.state()==Probe::workerError);worker.stop();
    Input input;Model ui;
    assert(input.update(Buttons::cross,true,0)==0); // held on launch
    assert(input.update(0,true,1)==0);
    ui.tab=Model::system;ui.selected=0;
    auto edge=input.update(Buttons::cross,true,2);
    assert(ui.press(edge)==Model::Action::update);
    assert(input.update(Buttons::cross,true,900000)==0); // no repeat for action
    // The update confirmation focuses Cancel; one held press cannot also confirm it.
    ui.updateDialog=true;ui.confirmUpdate=false;assert(ui.press(Buttons::cross)==Model::Action::none&&!ui.updateDialog);
    ui.updateDialog=true;ui.press(Buttons::right);
    assert(ui.confirmUpdate);
    assert(ui.press(input.update(Buttons::cross,true,1000000))==Model::Action::none);
    input.update(0,true,1000001);
    assert(ui.press(input.update(Buttons::cross,true,1000002))==Model::Action::installUpdate);
    ui.press(Buttons::down);ui.press(Buttons::down);ui.press(Buttons::down);assert(ui.selected==2&&ui.press(Buttons::cross)==Model::Action::quit);
    ui.selected=1;assert(ui.press(Buttons::cross)==Model::Action::retry);
    input.update(0,false,1000003);
    assert(input.update(Buttons::cross,true,1000004)==0); // reconnect held
    input.update(0,true,1000005);
    assert(input.update(Buttons::down,true,1000010)==Buttons::down);
    assert(input.update(Buttons::down,true,1100010)==0);
    assert(input.update(Buttons::down,true,1400010)==Buttons::down);
    assert(input.update(Buttons::down,true,1450010)==0);
    assert(input.update(Buttons::down,true,1540010)==Buttons::down);
    ui.updateDialog=true;ui.confirmUpdate=true;ui.press(Buttons::circle);assert(!ui.updateDialog);
    assert(ui.press(Buttons::circle)==Model::Action::none&&ui.tab==Model::system);
    // The controller can deliver press AND release since the last rendered frame.
    InputEvents buffered;buffered.ingest(0,true,1,0);
    buffered.ingest(Buttons::options,true,2,10);buffered.ingest(0,true,3,10);
    buffered.ingest(Buttons::down,true,4,10);buffered.ingest(0,true,5,10);
    buffered.ingest(Buttons::cross,true,6,10);buffered.ingest(0,true,7,10);
    assert(buffered.next(3000000)==Buttons::options); // A slow frame must not erase this.
    assert(buffered.next(3000000)==Buttons::down);
    assert(buffered.next(3000000)==Buttons::cross);assert(buffered.next(3000000)==0);
    buffered.ingest(Buttons::cross,true,6,3000000);assert(buffered.next(3000000)==0); // Duplicate history.
    buffered.ingest(Buttons::cross,true,8,3000010);buffered.ingest(0,false,9,3000010);assert(buffered.next(3000010)==0);
    buffered.ingest(Buttons::cross,true,10,3000020);assert(buffered.next(3000020)==0); // Held on reconnect.
    buffered.ingest(0,true,11,3000030);buffered.ingest(Buttons::cross,true,12,3000040);assert(buffered.next(3000040)==Buttons::cross);
    buffered.reset();buffered.ingest(0,true,1,0);
    for(unsigned i=0;i<130;++i){buffered.ingest(Buttons::cross,true,2+i*2,0);buffered.ingest(0,true,3+i*2,0);}
    assert(buffered.count<=1); // Overflow discards stale selections instead of replaying a partial queue.
    flow.add();flow.selected=40;flow.press(Buttons::right,catalog,false);assert(flow.selected==42);
    flow.press(Buttons::right,catalog,false);assert(flow.selected==44);
    flow.press(Buttons::right,catalog,false);assert(flow.selected==46);
    flow.press(Buttons::right,catalog,false);assert(flow.selected==48);
    flow.press(Buttons::up,catalog,false);assert(flow.selected==39);
    flow.press(Buttons::down,catalog,false);assert(flow.selected==48);
    reset("");responses={wire(health),wire(boot),wire(login),wire(actionable),wire(boot),wire("{}"),wire(health),wire(boot),wire(login),wire(actionable)};
    workerAllowed=true;auto queuedOwner=std::make_unique<Network>();auto& queued=*queuedOwner;assert(queued.start());Connection snapshot;ActionResult result;
    for(unsigned i=0;i<200;++i){queued.read(snapshot,&catalog,&result);if(snapshot.status==Probe::ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    Processing live;for(unsigned i=0;i<200;++i){queued.readProcessing(live);if(live.count)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(live.count==1&&live.tasks[0].items&&live.tasks[0].eta==8);
    assert(snapshot.status==Probe::ready);command.operation=Operation::pause;
    assert(queued.submit(command));assert(!queued.submit(command));
    bool snapshotBusy=true;
    for(unsigned i=0;i<500;++i){
        if(queued.read(snapshot,&catalog,&result,&snapshotBusy))assert(snapshotBusy||result.revision);
        if(result.revision)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(!snapshotBusy);
    assert(result.revision==1&&result.status==ActionResult::Status::success);queued.stop();workerAllowed=false;
    const auto post=request.find("POST /api/torrent ");assert(post!=std::string::npos&&request.find("POST /api/torrent ",post+1)==std::string::npos);
    // A deletion outlives its HTTP response: retain progress and reject duplicate
    // submissions until the catalog is fresh again, without replaying the POST.
    for(unsigned scenario:{0u,1u,2u,3u}){
        const bool lost=scenario!=0;
        reset("");chunk=4096;
        responses={wire(health),wire(boot),wire(login),wire(actionable),wire(boot),lost?"#timeout":wire("{}")};
        if(lost)responses.push_back(scenario==2?"#long-timeout":"#timeout");
        if(scenario==1){
            for(const auto& value:{wire(health),wire(boot),std::string("HTTP/1.1 400 Error\r\nContent-Length: 0\r\n\r\n"),wire(huge)})responses.push_back(value);
        }
        const auto finalState=scenario==3?std::string(R"({"freeBytes":1,"transmissionReady":true,"torrents":[],"jobs":[]})"):actionable;
        for(const auto& value:{health,boot,login,finalState})responses.push_back(wire(value));
        workerAllowed=true;auto deletingOwner=std::make_unique<Network>();auto& deleting=*deletingOwner;assert(deleting.start());catalog.revision=0;result=ActionResult{};
        for(unsigned i=0;i<200;++i){deleting.read(snapshot,&catalog);if(snapshot.status==Probe::ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        command=Command{};command.operation=scenario==3?Operation::removeLibrary:Operation::removeTorrent;std::snprintf(command.id.data(),command.id.size(),"%s",scenario==3?"j3":"9");
        assert(deleting.submit(command));assert(deleting.busy());assert(!deleting.submit(command));
        if(lost){
            for(unsigned i=0;i<200;++i){deleting.read(snapshot,&catalog,&result);if(catalog.stale)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
            assert(catalog.stale&&deleting.busy()&&deleting.deletion()==Network::Deletion::checking&&result.revision==0);
            assert(!deleting.submit(command));
        }
        for(unsigned i=0;i<2000;++i){deleting.read(snapshot,&catalog,&result);if(result.revision)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        assert(result.revision==1&&result.status==(lost&&scenario!=3?ActionResult::Status::uncertain:ActionResult::Status::success));
        assert(catalog.valid&&!catalog.stale&&!deleting.busy()&&deleting.deletion()==Network::Deletion::idle);
        deleting.stop();workerAllowed=false;
        const auto endpoint=scenario==3?"POST /api/delete-library-game ":"POST /api/torrent ";
        const auto deletionPost=request.find(endpoint);
        assert(deletionPost!=std::string::npos&&request.find(endpoint,deletionPost+1)==std::string::npos);
    }
    reset("");responses={wire(health),wire(boot),wire(login),wire(actionable),"#timeout",wire(health),wire(boot),wire(login),wire(actionable)};
    workerAllowed=true;auto recoveringOwner=std::make_unique<Network>();auto& recovering=*recoveringOwner;assert(recovering.start());
    for(unsigned i=0;i<200;++i){recovering.read(snapshot,&catalog);if(snapshot.status==Probe::ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(snapshot.status==Probe::ready&&catalog.valid&&!catalog.stale);
    const auto savedJobs=catalog.jobCount;recovering.retry();
    for(unsigned i=0;i<200;++i){recovering.read(snapshot,&catalog);if(snapshot.status==Probe::unavailable)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(snapshot.status==Probe::unavailable&&catalog.valid&&catalog.stale&&catalog.jobCount==savedJobs);
    assert(!recovering.submit(command));assert(*unavailable(Operation::pause,&catalog.torrents[0],catalog));
    // No Triangle press: the next automatic poll restores live data.
    for(unsigned i=0;i<800;++i){recovering.read(snapshot,&catalog);if(snapshot.status==Probe::ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(snapshot.status==Probe::ready&&catalog.valid&&!catalog.stale);recovering.stop();workerAllowed=false;
    reset("");chunk=4096;responses={wire(health),wire(boot),wire(login),wire(actionable),wire(health),wire(boot),"HTTP/1.1 400 Error\r\nContent-Length: 0\r\n\r\n",wire(huge),wire(health),wire(boot),wire(login),wire(actionable)};
    workerAllowed=true;auto transmissionRetryOwner=std::make_unique<Network>();auto& transmissionRetry=*transmissionRetryOwner;assert(transmissionRetry.start());catalog.revision=0;
    for(unsigned i=0;i<200;++i){transmissionRetry.read(snapshot,&catalog);if(snapshot.status==Probe::ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    const auto savedTorrents=catalog.torrentCount;assert(savedTorrents>0);transmissionRetry.retry();
    for(unsigned i=0;i<200;++i){transmissionRetry.read(snapshot,&catalog);if(snapshot.status==Probe::transmissionUnavailable)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(snapshot.status==Probe::transmissionUnavailable&&catalog.valid&&!catalog.stale&&catalog.transmissionStale);
    assert(catalog.torrentCount==savedTorrents&&catalog.jobCount==256&&!catalog.transmissionReady);
    assert(!transmissionRetry.submit(command));
    for(unsigned i=0;i<800;++i){transmissionRetry.read(snapshot,&catalog);if(snapshot.status==Probe::ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(snapshot.status==Probe::ready&&catalog.transmissionReady&&!catalog.transmissionStale);transmissionRetry.stop();workerAllowed=false;
    reset("");chunk=4096;responses={wire(health),wire(boot),"HTTP/1.1 400 Error\r\nContent-Length: 0\r\n\r\n",wire(huge),wire(boot),wire("{}"),wire(health),wire(boot),wire(login),wire(actionable)};
    workerAllowed=true;auto updateCheckOwner=std::make_unique<Network>();auto& updateCheck=*updateCheckOwner;assert(updateCheck.start());result=ActionResult{};
    for(unsigned i=0;i<200;++i){updateCheck.read(snapshot,&catalog);if(snapshot.status==Probe::transmissionUnavailable)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(snapshot.status==Probe::transmissionUnavailable);
    Command readonlyUpdate;readonlyUpdate.operation=Operation::checkNativeUpdate;assert(updateCheck.submit(readonlyUpdate));
    for(unsigned i=0;i<800;++i){updateCheck.read(snapshot,&catalog,&result);if(result.revision)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(result.revision&&result.status==ActionResult::Status::success);assert(request.find("POST /api/native-update/check ")!=std::string::npos);updateCheck.stop();workerAllowed=false;
    // Start in Discover: Explore and Search share one tab.
    Model searchModel;assert(searchModel.tab==Model::discover);
    searchModel.press(Buttons::r1);assert(searchModel.tab==Model::activity);
    searchModel.press(Buttons::l1);assert(searchModel.tab==Model::discover);
    assert(searchModel.press(Buttons::triangle)==Model::Action::explore);assert(searchModel.exploreSort==1);
    assert(searchModel.press(Buttons::square)==Model::Action::search);
    assert(searchModel.press(Buttons::cross)==Model::Action::none); // Nothing to get yet.
    searchModel.count=3;searchModel.press(Buttons::right);searchModel.press(Buttons::right);searchModel.press(Buttons::right);assert(searchModel.selected==2);
    assert(searchModel.press(Buttons::cross)==Model::Action::get&&searchModel.press(Buttons::options)==Model::Action::compare);
    searchModel.searchResults=true;searchModel.selected=0;searchModel.press(Buttons::down);assert(searchModel.selected==1);
    assert(searchModel.press(Buttons::triangle)==Model::Action::none&&searchModel.exploreSort==1);
    assert(searchModel.press(Buttons::square)==Model::Action::search&&searchModel.press(Buttons::cross)==Model::Action::get);
    searchModel.press(Buttons::circle);assert(!searchModel.searchResults&&searchModel.tab==Model::discover);
    searchModel.press(Buttons::r1);searchModel.press(Buttons::r1);assert(searchModel.tab==Model::library);
    Model library;library.tab=Model::library;library.count=16;
    library.press(Buttons::right);assert(library.selected==1);
    library.press(Buttons::down);assert(library.selected==8);
    library.press(Buttons::down);assert(library.selected==15);
    library.press(Buttons::right);assert(library.selected==15);
    library.press(Buttons::up);assert(library.selected==8);
    library.press(Buttons::left);assert(library.selected==7);
    library.press(Buttons::triangle);assert(library.filter==1&&library.selected==0);library.selected=7;
    library.press(Buttons::cross);assert(library.details);
    library.press(Buttons::down);assert(library.detailPage==1&&library.selected==7);
    library.press(Buttons::circle);assert(!library.details&&library.selected==7);
    assert(parseCatalog(R"({"freeBytes":1,"transmissionReady":true,"torrents":[],"jobs":[],"searchSupported":true,"search":{"query":"demo","busy":false,"adding":false,"results":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"Demo","size":100,"seeders":9,"leechers":2,"added":false}]}})",catalog));
    assert(catalog.searchSupported&&catalog.resultCount==1&&catalog.results[0].peers==9);
    flow.search();flow.append('a');flow.selected=48;assert(flow.press(Buttons::cross,catalog,false));
    assert(flow.command.operation==Operation::search);
    // A search result needs one press on the focused Get game button; busy waits.
    flow.grab(catalog.results[0],catalog);assert(flow.panel==Workflow::Panel::sheet&&flow.focus==flow.rowCount&&flow.rowCount==1);
    assert(!flow.press(Buttons::cross,catalog,true)&&flow.notice[0]&&flow.panel==Workflow::Panel::sheet);
    assert(flow.press(Buttons::cross,catalog,false));
    assert(flow.command.operation==Operation::grab&&std::string_view(flow.command.id.data())=="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    assert(parseCatalog(R"({"freeBytes":1,"transmissionReady":true,"torrents":[],"jobs":[],"exploreSupported":true,"explore":{"sort":"completed","busy":false,"results":[{"id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","name":"Demo PS5","size":123,"seeders":4,"completed":77,"published":"2026-10-01T12:00:00Z"}]}})",catalog));
    assert(catalog.exploreSupported&&catalog.exploreCount==1&&catalog.exploreResults[0].completedCount==77);
    flow.chooseSources(catalog.exploreResults[0],catalog);assert(flow.press(Buttons::cross,catalog,false));assert(flow.command.operation==Operation::exploreGrab);
    assert(parseCatalog(R"({"freeBytes":1,"transmissionReady":true,"torrents":[],"jobs":[],"exploreSupported":true,"explore":{"sort":"seeders","results":[{"id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","name":"Demo PS5","sources":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","tracker":"Tracker One","name":"Demo PS5","size":100,"seeders":20,"leechers":2,"completed":12},{"id":"cccccccccccccccccccccccccccccccc","tracker":"Tracker Two","name":"Demo PS5 Deluxe","size":200,"seeders":3,"completed":50}]}]}})",catalog));
    assert(catalog.exploreResults[0].sourceCount==2&&catalog.sourceCount==2&&catalog.sources[1].grabs==50&&catalog.sources[1].size==200);
    // The best-seeded source is preselected; Compare focuses the source row.
    flow.chooseSources(catalog.exploreResults[0],catalog);assert(flow.panel==Workflow::Panel::sheet&&flow.focus==flow.rowCount);
    assert(flow.sourceIndex==0&&std::string_view(flow.command.id.data())=="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    flow.chooseSources(catalog.exploreResults[0],catalog,true);assert(flow.focus==0);
    assert(!flow.press(Buttons::right,catalog,false)&&flow.sourceIndex==1&&std::string_view(flow.targetName.data())=="Demo PS5 Deluxe");
    flow.press(Buttons::right,catalog,false);assert(flow.sourceIndex==1); // No wrap past the last source.
    assert(!flow.press(Buttons::cross,catalog,false)&&flow.panel==Workflow::Panel::sheet&&flow.focus==flow.rowCount); // Cross on a row returns to the button.
    assert(flow.press(Buttons::cross,catalog,false)&&std::string_view(flow.command.id.data())=="cccccccccccccccccccccccccccccccc");
    catalog.storageSupported=true;flow.chooseSources(catalog.exploreResults[0],catalog);assert(flow.hasRow(Workflow::Row::storage)&&flow.hasRow(Workflow::Row::mode));
    catalog.exploreBusy=true;assert(!flow.press(Buttons::cross,catalog,false)&&flow.panel==Workflow::Panel::sheet&&std::string_view(flow.notice.data()).find("Browse sources")!=std::string_view::npos);catalog.exploreBusy=false;
    assert(std::string_view(Model::exploreSorts[0])=="newest"&&std::string_view(Model::exploreSorts[1])=="seeders"&&std::string_view(Model::exploreSorts[2])=="completed");
    assert(std::string_view(Model::exploreLabels[0])=="Newest");
    assert(std::string_view(Model::exploreLabels[1])=="Most seeded"&&std::string_view(Model::exploreLabels[2])=="Most grabbed");
    flow.chooseSources(catalog.exploreResults[0],catalog,true);flow.press(Buttons::right,catalog,true);assert(flow.sourceIndex==1); // Browsing works while busy.
    flow.press(Buttons::down,catalog,true);flow.press(Buttons::down,catalog,true);flow.press(Buttons::down,catalog,true);
    assert(!flow.press(Buttons::cross,catalog,true)&&flow.panel==Workflow::Panel::sheet&&flow.notice[0]);
    // No connected disk: the sheet stays open and explains why.
    assert(!flow.press(Buttons::cross,catalog,false)&&flow.panel==Workflow::Panel::sheet&&std::string_view(flow.notice.data()).find("disk")!=std::string_view::npos);
    flow.press(Buttons::circle,catalog,false);assert(flow.panel==Workflow::Panel::closed);catalog.storageSupported=false;
    command=Command{};command.operation=Operation::explore;std::snprintf(command.text.data(),command.text.size(),"completed");assert(encodeCommand(command,encoded.data(),encoded.size(),encodedSize));
    assert(parseCatalog(actionable,catalog));catalog.torrentRemovalSupported=true;
    flow.open(&catalog.torrents[0],0,catalog);assert(flow.options[3]==Operation::removeTorrent);flow.selected=3;assert(!flow.press(Buttons::cross,catalog,false));
    assert(flow.panel==Workflow::Panel::menu&&flow.confirming&&!flow.confirm);
    assert(!flow.press(Buttons::cross,catalog,false)&&!flow.confirming&&flow.panel==Workflow::Panel::menu); // Cancel is focused first.
    flow.press(Buttons::cross,catalog,false);flow.press(Buttons::up,catalog,false);assert(flow.selected==3&&flow.confirming);
    flow.press(Buttons::circle,catalog,false);assert(!flow.confirming&&flow.panel==Workflow::Panel::menu); // Circle backs out of the confirmation only.
    flow.press(Buttons::cross,catalog,false);flow.press(Buttons::right,catalog,false);assert(!flow.press(Buttons::cross,catalog,true)&&flow.confirming);
    assert(flow.press(Buttons::cross,catalog,false));assert(encodeCommand(flow.command,encoded.data(),encoded.size(),encodedSize));assert(std::string_view(encoded.data())==R"({"id":9,"action":"remove-data","confirmed":true})");
    catalog.extracting=true;assert(*unavailable(Operation::removeTorrent,&catalog.torrents[0],catalog));catalog.extracting=false;catalog.torrentRemovalSupported=false;assert(*unavailable(Operation::removeTorrent,&catalog.torrents[0],catalog));
    std::cout<<"Protocol fragmentation, failure cleanup, bounded responses, API versions, focus and input tests passed\n";
    {
        static Catalog lib;assert(parseCatalog(R"({"freeBytes":1,"libraryDeletionSupported":true,"transmissionReady":true,"torrents":[],"jobs":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"Test game","status":"moved","dismissed":true,"content":{"kind":"folder"}}]})",lib));
        assert(lib.libraryDeletionSupported&&entryCount(lib,1,0)==0&&entryCount(lib,2,0)==1);
        Workflow menu;menu.open(&lib.jobs[0],2,lib);
        assert(menu.optionCount==1&&menu.options[0]==Operation::removeLibrary);
        assert(!*unavailable(Operation::removeLibrary,&lib.jobs[0],lib));
        assert(!menu.press(Buttons::cross,lib,false));assert(menu.panel==Workflow::Panel::menu&&menu.confirming&&!menu.confirm);
        menu.press(Buttons::right,lib,false);assert(menu.press(Buttons::cross,lib,false));
        char body[256];std::size_t length=0;assert(encodeCommand(menu.command,body,sizeof(body),length));
        assert(std::string_view(body).find("\"confirmed\":true")!=std::string_view::npos);
        assert(std::string_view(actionPath(menu.command.operation))=="/api/delete-library-game");
        lib.libraryDeletionSupported=false;assert(*unavailable(Operation::removeLibrary,&lib.jobs[0],lib));
    }

    {
        static Catalog beta;assert(parseCatalog(R"({"freeBytes":1,"transmissionReady":true,"compression":{"supported":true,"busy":false,"status":"idle"},"torrents":[],"jobs":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"LEGO","status":"moved","content":{"kind":"folder","titleId":"PPSA23732"}}]})",beta));
        Workflow menu;menu.open(&beta.jobs[0],2,beta);assert(menu.options[0]==Operation::compress);
        assert(!*unavailable(Operation::compress,&beta.jobs[0],beta));
        // Compression is not destructive: its sheet starts on the Compress button.
        assert(!menu.press(Buttons::cross,beta,false));assert(menu.panel==Workflow::Panel::sheet&&!menu.rowCount&&!menu.confirming);
        assert(menu.press(Buttons::cross,beta,false));
        char body[256];std::size_t length=0;assert(encodeCommand(menu.command,body,sizeof(body),length));
        assert(std::string_view(body)==R"({"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","confirmed":true})");
        assert(std::string_view(actionPath(menu.command.operation))=="/api/compress-game");
        beta.compressionBusy=true;assert(*unavailable(Operation::compress,&beta.jobs[0],beta));
        assert(*unavailable(Operation::removeLibrary,&beta.jobs[0],beta));
        std::snprintf(beta.compressionStatus.data(),beta.compressionStatus.size(),"running");beta.compressionJob=beta.jobs[0].id;menu.open(&beta.jobs[0],2,beta);assert(menu.options[0]==Operation::cancelCompression);
        beta.compressionBusy=false;std::snprintf(beta.jobs[0].titleId.data(),beta.jobs[0].titleId.size(),"PPSA31246");
        assert(!*unavailable(Operation::compress,&beta.jobs[0],beta));
    }

    {
        static Catalog release;assert(parseCatalog(R"({"freeBytes":1,"transmissionReady":true,"compression":{"supported":true,"busy":false},"torrents":[],"jobs":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"Compressed game","status":"moved","content":{"kind":"compressed","titleId":"PPSA12345"},"compression":{"status":"ready","verified":false,"originalKept":true,"sourceStorage":"internal","storage":"external-test"}}]})",release));
        auto& e=release.jobs[0];assert(e.compressed&&e.originalKept&&!e.compressionVerified);
        std::array<LibraryCopy,2> copies{};
        assert(libraryCopies(e,copies)==2);
        assert(std::string_view(copies[0].format)=="Compressed"&&std::string_view(copies[0].storage)=="external-test");
        assert(std::string_view(copies[1].format)=="Uncompressed"&&std::string_view(copies[1].storage)=="internal");
        auto pending=e;pending.compressionVerified=false;std::snprintf(pending.kind.data(),pending.kind.size(),"folder");
        std::snprintf(pending.compressionState.data(),pending.compressionState.size(),"running");
        assert(libraryCopies(pending,copies)==1&&std::string_view(copies[0].format)=="Uncompressed");
        std::snprintf(pending.compressionState.data(),pending.compressionState.size(),"restored");
        assert(libraryCopies(pending,copies)==2&&copies[1].retained);
        e.originalKept=false;assert(libraryCopies(e,copies)==1);e.originalKept=true;
        char label[100];storageLabel(release,"external-missing",label,sizeof(label));assert(std::string_view(label)=="Offline: external SSD");
        Workflow w;w.open(&e,2,release);assert(w.options[0]==Operation::removeOriginal&&w.options[1]==Operation::restoreOriginal);
        assert(!*unavailable(Operation::removeOriginal,&e,release));assert(*unavailable(Operation::compress,&e,release));
        e.compressionVerified=false;w.open(&e,2,release);assert(w.options[0]==Operation::removeOriginal);
        assert(!*unavailable(Operation::removeOriginal,&e,release));
        e.compressionVerified=true;
        Command cmd;cmd.operation=Operation::removeOriginal;cmd.id=e.id;char body[512];size_t n=0;assert(encodeCommand(cmd,body,sizeof(body),n));assert(std::string_view(body).find("\"confirmed\":true")!=std::string_view::npos);
        assert(std::string_view(actionPath(cmd.operation))=="/api/delete-uncompressed");
        e.originalKept=false;assert(*unavailable(Operation::removeOriginal,&e,release));assert(*unavailable(Operation::restoreOriginal,&e,release));
        w.open(&e,2,release);assert(w.optionCount==1&&w.options[0]==Operation::removeLibrary);
        assert(*unavailable(Operation::removeLibrary,&e,release)); // Old service must not pretend to support deletion.
        release.compressedDeletionSupported=true;release.transmissionReady=false;
        assert(!*unavailable(Operation::removeLibrary,&e,release)); // No torrent or archive required.
        assert(!w.press(Buttons::cross,release,false)&&w.panel==Workflow::Panel::menu&&w.confirming&&!w.confirm);
        w.press(Buttons::right,release,false);assert(w.press(Buttons::cross,release,false));
        assert(w.command.operation==Operation::removeLibrary);
        release.compressionBusy=true;assert(*unavailable(Operation::removeLibrary,&e,release));
        release.compressionBusy=false;std::snprintf(e.compressionState.data(),e.compressionState.size(),"uncertain");assert(*unavailable(Operation::removeLibrary,&e,release));
    }

    {
        static Catalog c;assert(parseCatalog(R"({"freeBytes":123,"transmissionReady":true,"searchSupported":true,"torrents":[],"jobs":[],"storageSupported":true,"storage":[{"id":"internal","label":"Internal SSD","available":true,"freeBytes":123},{"id":"external-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","label":"External SSD","available":true,"freeBytes":999}]})",c));
        assert(c.storageCount==2&&c.storage[1].freeBytes==999);
        Entry game;std::snprintf(game.id.data(),game.id.size(),"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        // One Get game sheet: source, install-on and after-download rows, button focused.
        Workflow w;w.grab(game,c);assert(w.panel==Workflow::Panel::sheet&&w.rowCount==3&&w.focus==3);
        assert(w.command.automatic&&std::string_view(w.command.storage.data())=="internal");
        w.press(Buttons::up,c,false);w.press(Buttons::right,c,false);assert(!w.command.automatic);
        w.press(Buttons::up,c,false);w.press(Buttons::right,c,false);assert(w.command.storage==c.storage[1].id);
        w.press(Buttons::right,c,false);assert(w.command.storage==c.storage[1].id); // Last disk: no wrap.
        w.press(Buttons::down,c,false);w.press(Buttons::down,c,false);assert(w.focus==3);
        c.storage[1].available=false;assert(!w.press(Buttons::cross,c,false)&&w.notice[0]);c.storage[1].available=true;assert(w.press(Buttons::cross,c,false));
        char body[512];size_t n=0;assert(encodeCommand(w.command,body,sizeof(body),n));assert(std::string_view(body).find("\"automatic\":false")!=std::string_view::npos);assert(std::string_view(body).find(c.storage[1].id.data())!=std::string_view::npos);
        // The last disk and mode are remembered, and dropped if that disk disconnects.
        w.grab(game,c);assert(w.command.storage==c.storage[1].id&&!w.command.automatic);
        c.storage[1].available=false;w.grab(game,c);assert(std::string_view(w.command.storage.data())=="internal");c.storage[1].available=true;
        // Moving to another disk never offers the current one.
        static Catalog disks;disks=c;disks.jobCount=1;auto& moved=disks.jobs[0];moved=Entry{};std::snprintf(moved.id.data(),moved.id.size(),"job");std::snprintf(moved.name.data(),moved.name.size(),"Game");
        std::snprintf(moved.status.data(),moved.status.size(),"moved");std::snprintf(moved.storage.data(),moved.storage.size(),"internal");std::snprintf(moved.kind.data(),moved.kind.size(),"folder");
        disks.valid=true;disks.transmissionReady=true;
        Workflow move;move.open(&moved,2,disks);assert(!move.choose(Operation::transfer,disks,false)&&move.panel==Workflow::Panel::sheet);
        assert(move.command.storage==disks.storage[1].id);move.press(Buttons::up,disks,false);move.press(Buttons::left,disks,false);assert(move.command.storage==disks.storage[1].id);
        move.press(Buttons::down,disks,false);assert(move.press(Buttons::cross,disks,false)&&move.command.operation==Operation::transfer);
        // Safe actions run at once; destructive ones always confirm first.
        assert(Workflow::immediate(Operation::pause)&&Workflow::immediate(Operation::verify)&&!Workflow::immediate(Operation::extract));
        for(auto op:{Operation::removeTorrent,Operation::removeLibrary,Operation::removeOriginal,Operation::restoreOriginal,Operation::remove,Operation::dismiss,Operation::cancel,Operation::cancelCompression})assert(Workflow::destructive(op)&&!Workflow::immediate(op));
        Command cmd;cmd.operation=Operation::transfer;cmd.torrent=true;std::snprintf(cmd.id.data(),cmd.id.size(),"42");cmd.storage=c.storage[1].id;assert(encodeCommand(cmd,body,sizeof(body),n));assert(std::string_view(body).starts_with("{\"id\":42"));assert(std::string_view(body).find("\"kind\":\"torrent\"")!=std::string_view::npos);
        cmd.torrent=false;cmd.id=game.id;assert(encodeCommand(cmd,body,sizeof(body),n));assert(std::string_view(body).find("\"kind\":\"job\"")!=std::string_view::npos);
    }

}
