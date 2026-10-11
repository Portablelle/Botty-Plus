#include "probe.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

namespace {
std::string request,response,confirmation;
std::size_t offset=0;
bool loseResponse=false;
std::string wire(const std::string& body){return "HTTP/1.1 202 Accepted\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;}
}
namespace botty::platform {
std::uint64_t now() noexcept {return 1;}
void sleep(unsigned) noexcept {}
void log(const char*) noexcept {}
int connectLocal() noexcept {request.clear();response.clear();offset=0;return 1;}
int send(int,const void* bytes,std::size_t length) noexcept {request.append(static_cast<const char*>(bytes),length);return static_cast<int>(length);}
int receive(int,void* bytes,std::size_t length) noexcept {
    if(response.empty()){
        if(request.starts_with("GET /api/bootstrap "))response="HTTP/1.1 200 OK\r\nContent-Length: 59\r\n\r\n{\"apiVersion\":1,\"token\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}";
        else {if(loseResponse)return -1;response=wire(confirmation);}
    }
    const auto size=std::min(length,response.size()-offset);std::memcpy(bytes,response.data()+offset,size);offset+=size;return static_cast<int>(size);
}
void closeSocket(int) noexcept {}
bool startWorker(void* (*)(void*),void*,void**) noexcept {return false;}
void joinWorker(void*) noexcept {}
}
int main(){
    using namespace botty;
    assert(validNativeVersion("01.007.001")&&!validNativeVersion("1.4.2")&&!validNativeVersion("01.004.00x"));
    assert(newerNativeVersion("01.007.001",nativeVersion));
    assert(!newerNativeVersion("01.004.000",nativeVersion));
    assert(!newerNativeVersion(nativeVersion,nativeVersion));
    auto catalog=new Catalog;
    const auto parse=[&](std::string update){
        const bool available=update.find("\"status\":\"available\"")!=std::string::npos;
        update.insert(1,std::string("\"scope\":\"installation\",\"installedServiceVersion\":\"1.5.4\",\"availableServiceVersion\":\"1.5.4\",\"installedWorkerVersion\":\"1.3.1\",\"availableWorkerVersion\":\"1.3.1\",\"installedEngineVersion\":\"0.16.24-botty5\",\"availableEngineVersion\":\"0.16.24-botty5\",\"updateAvailable\":")+(available?"true,":"false,"));
        return parseCatalog("{\"freeBytes\":0,\"transmissionReady\":true,\"torrents\":[],\"jobs\":[],\"nativeUpdate\":"+update+"}",*catalog);
    };
    assert(parse(R"({"supported":true,"status":"available","installedVersion":"01.007.000","availableVersion":"01.007.001","requested":false,"closeRequired":false,"message":"Ready to update"})"));
    assert(nativeUpdateAvailable(catalog->nativeUpdate,false));
    assert(!nativeUpdateAvailable(catalog->nativeUpdate,true));
    assert(std::string_view(nativeUpdateLabel(catalog->nativeUpdate,false))=="Update available");
    assert(validServiceVersion("1.5.4")&&!validServiceVersion("1.5")&&!validServiceVersion("1.5.x")&&!validServiceVersion("1..4"));
    assert(serviceVersionAtLeast("1.10.0","1.9.9")&&!serviceVersionAtLeast("1.5.3","1.5.4"));
    assert(validServiceVersion("1.10000.0")&&validServiceVersion("999999.999999.999999"));
    assert(!validServiceVersion("1000000.0.0")&&!validServiceVersion(std::string(65,'0')));
    assert(parse(R"({"supported":true,"status":"available","installedVersion":"01.007.000","availableVersion":"01.007.000","requested":false,"closeRequired":false})"));
    auto wide=catalog->nativeUpdate;std::snprintf(wide.availableServiceVersion.data(),wide.availableServiceVersion.size(),"1.10000.0");
    assert(nativeUpdateAvailable(wide,false));
    assert(parse(R"({"supported":true,"status":"available","installedVersion":"01.007.000","availableVersion":"01.007.000","requested":false,"closeRequired":false})"));
    assert(nativeUpdateAvailable(catalog->nativeUpdate,false));
    catalog->compressionBusy=true;catalog->extracting=true;
    assert(!*unavailable(Operation::nativeUpdate,nullptr,*catalog));
    assert(parse(R"({"supported":true,"status":"waiting","installedVersion":"01.007.000","availableVersion":"01.007.001","requested":true,"closeRequired":true,"message":"Waiting for compression"})"));
    assert(!nativeUpdateAvailable(catalog->nativeUpdate,false));
    assert(std::string_view(nativeUpdateLabel(catalog->nativeUpdate,false))=="Update queued - Close app");
    assert(parse(R"({"supported":true,"status":"current","installedVersion":"01.007.000","availableVersion":"01.007.000","requested":false,"closeRequired":false})"));
    assert(std::string_view(nativeUpdateLabel(catalog->nativeUpdate,false))=="Installation up to date");
    assert(parse(R"({"supported":true,"status":"complete","installedVersion":"01.007.001","availableVersion":"01.007.001","requested":false,"closeRequired":false})"));
    assert(std::string_view(nativeUpdateLabel(catalog->nativeUpdate,false))=="Reopen updated app");
    assert(parse(R"({"supported":true,"status":"blocked","installedVersion":"01.007.000","availableVersion":"01.007.001","requested":true,"closeRequired":true,"message":"Recheck to retry the scan or use Portal recovery."})"));
    assert(std::string_view(nativeUpdateLabel(catalog->nativeUpdate,false))=="Update needs recovery");
    for(const auto bad:{R"({"supported":true,"status":"current","installedVersion":"","availableVersion":"","requested":false,"closeRequired":false})",R"({"supported":true,"status":"available","installedVersion":"01.007.000","availableVersion":"01.007.001","requested":"false","closeRequired":false})",R"({"supported":true,"status":"unknown","installedVersion":"01.007.000","availableVersion":"01.007.001","requested":false,"closeRequired":false})"}){
        assert(parse(bad)&&!catalog->nativeUpdate.supported);
    }
    assert(parseCatalog(R"({"freeBytes":0,"transmissionReady":true,"torrents":[],"jobs":[]})",*catalog));
    assert(!catalog->nativeUpdate.supported);
    assert(std::string_view(nativeUpdateLabel(catalog->nativeUpdate,true))=="Update status unavailable");
    assert(*unavailable(Operation::checkNativeUpdate,nullptr,*catalog));
    assert(parseCatalog(R"({"freeBytes":0,"transmissionReady":true,"torrents":[],"jobs":[],"nativeUpdate":{"supported":true,"scope":17,"status":"available","installedVersion":"01.007.000","availableVersion":"01.007.001","installedServiceVersion":"1.5.4","availableServiceVersion":"1.5.5","installedWorkerVersion":"1.3.1","availableWorkerVersion":"1.3.1","installedEngineVersion":"0.16.24-botty5","availableEngineVersion":"0.16.24-botty5","updateAvailable":true,"requested":false,"closeRequired":false}})",*catalog)&&!catalog->nativeUpdate.supported);
    // System lists the update button first, then Reconnect and Close Botty+.
    Model model;model.tab=Model::system;
    assert(model.press(Buttons::cross)==Model::Action::update);
    model.press(Buttons::down);assert(model.selected==1);
    model.press(Buttons::down);assert(model.selected==2);
    model.press(Buttons::down);assert(model.selected==2);
    assert(model.press(Buttons::cross)==Model::Action::quit);
    model.press(Buttons::up);model.press(Buttons::up);assert(model.selected==0);
    model.updateDialog=true;model.confirmUpdate=false;
    assert(model.press(Buttons::cross)==Model::Action::none&&!model.updateDialog);
    model.updateDialog=true;model.press(Buttons::right);
    assert(model.press(Buttons::cross)==Model::Action::installUpdate&&!model.updateDialog);
    model.updateDialog=true;assert(model.press(Buttons::circle)==Model::Action::none&&!model.updateDialog);
    Command command;std::array<char,128> encoded{};std::size_t length=0;
    for(const auto op:{Operation::nativeUpdate,Operation::checkNativeUpdate}){
        command.operation=op;assert(encodeCommand(command,encoded.data(),encoded.size(),length));assert(std::string_view(encoded.data())=="{}");
    }
    assert(std::string_view(actionPath(Operation::nativeUpdate))=="/api/native-update");
    command.operation=Operation::nativeUpdate;
    std::snprintf(command.serviceVersion.data(),command.serviceVersion.size(),"1.5.4");
    confirmation=R"({"apiVersion":1,"scope":"installation","status":"queued","version":"01.007.001","serviceVersion":"1.5.4","transaction":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
    assert(performCommand(command).status==ActionResult::Status::success);
    confirmation=R"({"apiVersion":1,"scope":"installation","status":"queued","version":"01.007.000","serviceVersion":"1.5.4","transaction":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
    assert(performCommand(command).status==ActionResult::Status::success);
    confirmation=R"({"apiVersion":1,"scope":"installation","status":"queued","version":"01.007.001","serviceVersion":"1.5.3","transaction":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
    assert(performCommand(command).status==ActionResult::Status::uncertain);
    confirmation=R"({"apiVersion":1,"scope":"installation","status":"queued","version":"01.007.000","serviceVersion":"1.5.5","transaction":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})";
    assert(performCommand(command).status==ActionResult::Status::success);
    for(const auto bad:{"{}",R"({"apiVersion":1,"status":"queued","version":"01.007.000"})",R"({"apiVersion":1,"status":"queued","version":"1.4.2"})",R"({"apiVersion":1,"status":"queued","version":"01.004.000"})",R"({"apiVersion":1,"status":"waiting","version":"01.007.001"})"}){
        confirmation=bad;assert(performCommand(command).status==ActionResult::Status::uncertain);
    }
    for(const auto bad:{R"({"apiVersion":1,"scope":"installation","status":"queued","version":"01.007.001","serviceVersion":"bad","transaction":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})",R"({"apiVersion":1,"scope":"installation","status":"queued","version":"01.007.001","serviceVersion":"1.5.4","transaction":"short"})",R"({"apiVersion":1,"scope":"native","status":"queued","version":"01.007.001","serviceVersion":"1.5.4","transaction":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})"}){
        confirmation=bad;assert(performCommand(command).status==ActionResult::Status::uncertain);
    }
    loseResponse=true;assert(performCommand(command).status==ActionResult::Status::uncertain);
    delete catalog;
}
