// SPDX-License-Identifier: GPL-3.0-or-later
#include "model.hpp"
#include "probe.hpp"
#include "platform.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
namespace {
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
    response=std::move(data);request.clear();offset=0;closes=0;clockUs=0;
    responses.clear();requests=0;connected=true;receiveError=false;receiveCost=100;chunk=7;
}
}
namespace botty::platform {
std::uint64_t now() noexcept {return clockUs;}
void sleep(unsigned n) noexcept {if(workerAllowed)std::this_thread::sleep_for(std::chrono::microseconds(n));else clockUs+=n;}
void log(const char*) noexcept {}
int connectLocal() noexcept {if(requests<responses.size())response=responses[requests];++requests;offset=0;return connected?3:-1;}
int send(int,const void* bytes,std::size_t size) noexcept {
    const auto n=std::min(size,chunk);request.append(static_cast<const char*>(bytes),n);return static_cast<int>(n);
}
int receive(int,void* bytes,std::size_t size) noexcept {
    clockUs+=receiveCost;
    if(receiveError)return -1;
    const auto n=std::min({size,chunk,response.size()-offset});
    std::memcpy(bytes,response.data()+offset,n);offset+=n;return static_cast<int>(n);
}
void closeSocket(int) noexcept {++closes;}
bool startWorker(void* (*fn)(void*),void* context,void** handle) noexcept {if(!workerAllowed)return false;*handle=new std::thread([=]{fn(context);});return true;}
void joinWorker(void* handle) noexcept {auto* thread=static_cast<std::thread*>(handle);thread->join();delete thread;}
}
int main() {
    using namespace botty;
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
    reset(wire(legacy));response.pop_back();assert(probeService()==Probe::malformed);assert(closes==1);
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
    Connection details;
    assert(parseConnection(login,details));
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
    Model browse;browse.tab=0;browse.count=12;browse.press(Buttons::down);assert(browse.selected==1);
    browse.press(Buttons::cross);assert(browse.details);browse.press(Buttons::circle);assert(!browse.details&&!browse.quitDialog);
    browse.press(Buttons::right);assert(browse.filter==1&&browse.selected==0);
    browse.press(Buttons::r1);assert(browse.tab==1&&browse.selected==0);
    browse.press(Buttons::l1);assert(browse.tab==0);
    assert(browse.press(Buttons::options)==Model::Action::menu);
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
    flow.press(Buttons::down,catalog,false);flow.press(Buttons::down,catalog,false);flow.press(Buttons::cross,catalog,false);assert(flow.panel==Workflow::Panel::archives);
    flow.press(Buttons::down,catalog,false);flow.press(Buttons::cross,catalog,false);assert(std::string_view(flow.command.archive.data())=="two\".rar");
    flow.append('s');flow.append('e');flow.append('c');flow.erase();assert(std::string_view(flow.command.text.data())=="se");
    flow.selected=48;flow.press(Buttons::cross,catalog,false);assert(flow.panel==Workflow::Panel::confirm&&!flow.confirm);
    flow.press(Buttons::right,catalog,false);assert(!flow.press(Buttons::cross,catalog,true));assert(flow.press(Buttons::cross,catalog,false));
    flow.close();assert(!flow.command.text[0]);
    flow.open(&catalog.jobs[0],1,catalog);flow.press(Buttons::cross,catalog,false);flow.press(Buttons::right,catalog,false);
    catalog.jobCount=0;assert(!flow.press(Buttons::cross,catalog,false));assert(flow.notice[0]); // Removed target cannot redirect to another job.
    flow.add();flow.command.text.fill(0);flow.selected=48;flow.press(Buttons::cross,catalog,false);assert(flow.panel==Workflow::Panel::keyboard&&flow.notice[0]);
    flow.unicodeInput=true;flow.append('e');flow.append('9');assert(flow.finishUnicode());assert(std::string_view(flow.command.text.data())=="é");flow.erase();assert(!flow.command.text[0]);
    flow.unicodeInput=true;for(char c:std::string_view("1f680"))flow.append(c);assert(flow.finishUnicode());assert(std::string_view(flow.command.text.data())=="🚀");flow.erase();assert(!flow.command.text[0]);
    flow.unicodeInput=true;for(char c:std::string_view("d800"))flow.append(c);assert(!flow.finishUnicode());flow.close();
    bool printable[127]={};printable[' ']=true;
    for(unsigned page=0;page<3;++page){assert(Workflow::keys(page).size()==40);for(char c:Workflow::keys(page))printable[static_cast<unsigned char>(c)]=true;}
    for(unsigned c=32;c<127;++c)assert(printable[c]);
    Network worker;assert(!worker.start());assert(worker.state()==Probe::workerError);worker.stop();
    Input input;Model ui;
    assert(input.update(Buttons::cross,true,0)==0); // held on launch
    assert(input.update(0,true,1)==0);
    ui.tab=3;ui.selected=1;
    auto edge=input.update(Buttons::cross,true,2);
    assert(ui.press(edge)==Model::Action::none && ui.quitDialog && !ui.confirmQuit);
    assert(input.update(Buttons::cross,true,900000)==0); // no repeat for action
    ui.press(Buttons::right);
    assert(ui.confirmQuit);
    assert(ui.press(input.update(Buttons::cross,true,1000000))==Model::Action::none);
    input.update(0,true,1000001);
    assert(ui.press(input.update(Buttons::cross,true,1000002))==Model::Action::quit);
    input.update(0,false,1000003);
    assert(input.update(Buttons::cross,true,1000004)==0); // reconnect held
    input.update(0,true,1000005);
    assert(input.update(Buttons::down,true,1000010)==Buttons::down);
    assert(input.update(Buttons::down,true,1100010)==0);
    assert(input.update(Buttons::down,true,1400010)==Buttons::down);
    assert(input.update(Buttons::down,true,1450010)==0);
    assert(input.update(Buttons::down,true,1540010)==Buttons::down);
    ui.quitDialog=true;ui.confirmQuit=true;ui.press(Buttons::circle);assert(!ui.quitDialog);
    ui.press(Buttons::circle);assert(ui.quitDialog&&!ui.confirmQuit);
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
    workerAllowed=true;Network queued;assert(queued.start());Connection snapshot;ActionResult result;
    for(unsigned i=0;i<200;++i){queued.read(snapshot,&catalog,&result);if(snapshot.status==Probe::ready)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(snapshot.status==Probe::ready);command.operation=Operation::pause;
    assert(queued.submit(command));assert(!queued.submit(command));
    for(unsigned i=0;i<500;++i){queued.read(snapshot,&catalog,&result);if(result.revision)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    assert(result.revision==1&&result.status==ActionResult::Status::success);queued.stop();workerAllowed=false;
    const auto post=request.find("POST /api/torrent ");assert(post!=std::string::npos&&request.find("POST /api/torrent ",post+1)==std::string::npos);
    // Start in Explore and follow the browse -> prepare -> collect journey.
    Model searchModel;assert(searchModel.tab==5);
    searchModel.press(Buttons::r1);assert(searchModel.tab==4);
    searchModel.press(Buttons::l1);assert(searchModel.tab==5);
    assert(searchModel.press(Buttons::triangle)==Model::Action::explore);assert(searchModel.exploreSort==1);
    searchModel.press(Buttons::r1);searchModel.press(Buttons::r1);assert(searchModel.tab==0);
    Model library;library.tab=2;library.count=8;
    library.press(Buttons::right);assert(library.selected==1);
    library.press(Buttons::down);assert(library.selected==4);
    library.press(Buttons::down);assert(library.selected==7);
    library.press(Buttons::right);assert(library.selected==7);
    library.press(Buttons::up);assert(library.selected==4);
    library.press(Buttons::left);assert(library.selected==3);
    library.press(Buttons::cross);assert(library.details);
    library.press(Buttons::down);assert(library.detailPage==1&&library.selected==3);
    library.press(Buttons::circle);assert(!library.details&&library.selected==3);
    assert(parseCatalog(R"({"freeBytes":1,"transmissionReady":true,"torrents":[],"jobs":[],"searchSupported":true,"search":{"query":"demo","busy":false,"adding":false,"results":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"Demo","size":100,"seeders":9,"leechers":2,"added":false}]}})",catalog));
    assert(catalog.searchSupported&&catalog.resultCount==1&&catalog.results[0].peers==9);
    flow.search();flow.append('a');flow.selected=48;assert(flow.press(Buttons::cross,catalog,false));
    assert(flow.command.operation==Operation::search);
    flow.grab(catalog.results[0]);assert(!flow.press(Buttons::cross,catalog,false));
    flow.grab(catalog.results[0]);flow.press(Buttons::right,catalog,false);assert(flow.press(Buttons::cross,catalog,false));
    assert(flow.command.operation==Operation::grab);
    assert(parseCatalog(R"({"freeBytes":1,"transmissionReady":true,"torrents":[],"jobs":[],"exploreSupported":true,"explore":{"sort":"completed","busy":false,"results":[{"id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","name":"Demo PS5","size":123,"seeders":4,"completed":77,"published":"2026-10-01T12:00:00Z"}]}})",catalog));
    assert(catalog.exploreSupported&&catalog.exploreCount==1&&catalog.exploreResults[0].completedCount==77);
    flow.grab(catalog.exploreResults[0],true);flow.press(Buttons::right,catalog,false);assert(flow.press(Buttons::cross,catalog,false));assert(flow.command.operation==Operation::exploreGrab);
    command=Command{};command.operation=Operation::explore;std::snprintf(command.text.data(),command.text.size(),"completed");assert(encodeCommand(command,encoded.data(),encoded.size(),encodedSize));
    assert(parseCatalog(actionable,catalog));catalog.torrentRemovalSupported=true;
    flow.open(&catalog.torrents[0],0,catalog);assert(flow.options[3]==Operation::removeTorrent);flow.selected=3;assert(!flow.press(Buttons::cross,catalog,false));assert(flow.panel==Workflow::Panel::confirm&&!flow.confirm);
    flow.press(Buttons::right,catalog,false);assert(flow.press(Buttons::cross,catalog,false));assert(encodeCommand(flow.command,encoded.data(),encoded.size(),encodedSize));assert(std::string_view(encoded.data())==R"({"id":9,"action":"remove-data","confirmed":true})");
    catalog.extracting=true;assert(*unavailable(Operation::removeTorrent,&catalog.torrents[0],catalog));catalog.extracting=false;catalog.torrentRemovalSupported=false;assert(*unavailable(Operation::removeTorrent,&catalog.torrents[0],catalog));
    std::cout<<"Protocol fragmentation, failure cleanup, bounded responses, API versions, focus and input tests passed\n";
}
