// SPDX-License-Identifier: GPL-3.0-or-later
#include "probe.hpp"
#include "platform.hpp"
#include "json_flat.hpp"
#include <array>
#include <cstdio>
#include <string_view>
namespace botty {
namespace {
struct Socket { int fd; ~Socket(){if(fd>=0)platform::closeSocket(fd);} };
bool equalsIgnoreCase(std::string_view a,std::string_view b) noexcept {
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i) {
        char c=a[i]; if(c>='A'&&c<='Z')c+=32;
        if(c!=b[i])return false;
    }
    return true;
}
template<std::size_t N=1025> struct Response {std::array<char,N> body{};unsigned status=0;std::size_t length=0;
    std::string_view view() const noexcept{return {body.data(),length};}
};
template<std::size_t N> Probe get(std::string_view path,std::string_view token,Response<N>& out,std::uint64_t deadline,std::string_view payload={},bool* sent=nullptr) noexcept {
    const auto started=platform::now();
    const auto unavailable=[&](const char* reason) noexcept {
        const auto endpoint=slice(path,0,path.find('?'));
        char message[160];std::snprintf(message,sizeof(message),"API %.*s: %s after %llu ms",int(endpoint.size()),endpoint.data(),reason,
            static_cast<unsigned long long>((platform::now()-started)/1000));
        platform::log(message);return Probe::unavailable;
    };
    if(platform::now()>deadline)return unavailable("deadline exceeded");
    Socket s{platform::connectLocal()}; if(s.fd<0)return unavailable("connection failed");
    std::array<char,512> requestBytes{};std::size_t requestSize=0;
    const auto append=[&](std::string_view text){for(char c:text){if(requestSize<requestBytes.size())requestBytes[requestSize++]=c;}};
    append(payload.empty()?"GET ":"POST ");append(path);append(" HTTP/1.1\r\nHost: 127.0.0.1:8088\r\nAccept: application/json\r\nConnection: close\r\n");
    if(!token.empty()){append("X-Botty-Token: ");append(token);append("\r\n");}
    if(!payload.empty()){char length[32];std::snprintf(length,sizeof(length),"%zu",payload.size());append("Content-Type: application/json\r\nContent-Length: ");append(length);append("\r\n");}
    append("\r\n");
    const std::string_view request{requestBytes.data(),requestSize};
    for(auto part:{request,payload})for(std::size_t offset=0;offset<part.size();) {
        if(platform::now()>deadline)return unavailable("deadline exceeded");
        const int n=platform::send(s.fd,part.data()+offset,part.size()-offset);
        if(n<=0)return unavailable("send failed");
        if(sent)*sent=true;
        offset+=static_cast<unsigned>(n);
    }
    std::array<char,4096> buffer{};
    std::size_t used=0, bodyStart=0, length=0;
    bool headers=false;
    while(used<buffer.size()) {
        if(platform::now()>deadline)return unavailable("deadline exceeded");
        const int n=platform::receive(s.fd,buffer.data()+used,buffer.size()-used);
        if(n<0)return unavailable("receive timeout or failure");
        if(n==0)return unavailable("response interrupted");
        used+=static_cast<unsigned>(n);
        const std::string_view data{buffer.data(),used};
        if(!headers) {
            const auto end=data.find("\r\n\r\n");
            if(end==std::string_view::npos)continue;
            const auto statusEnd=data.find("\r\n");
            const auto status=slice(data,0,statusEnd);
            if(status.size()<12 || (slice(status,0,9)!="HTTP/1.1 "&&slice(status,0,9)!="HTTP/1.0 "))return Probe::malformed;
            for(char c:slice(status,9,3)){if(c<'0'||c>'9')return Probe::malformed;out.status=out.status*10+static_cast<unsigned>(c-'0');}
            if(payload.empty()){if(out.status==403)return Probe::rejected;if(out.status!=200)return Probe::incompatible;}
            if(status.size()>12 && status[12]!=' ')return Probe::malformed;
            bool hasLength=false;
            auto at=statusEnd+2;
            while(at<end) {
                const auto next=data.find("\r\n",at);
                auto line=slice(data,at,next-at);
                const auto colon=line.find(':');
                if(colon==std::string_view::npos)return Probe::malformed;
                auto key=slice(line,0,colon),value=slice(line,colon+1);
                while(!value.empty()&&(value.front()==' '||value.front()=='\t'))value.remove_prefix(1);
                if(equalsIgnoreCase(key,"transfer-encoding"))return Probe::malformed;
                if(equalsIgnoreCase(key,"content-length")) {
                    if(hasLength||value.empty())return Probe::malformed;
                    hasLength=true;
                    for(char c:value) { if(c<'0'||c>'9')return Probe::malformed; length=length*10+static_cast<unsigned>(c-'0'); if(length>=out.body.size())return Probe::malformed; }
                }
                at=next+2;
            }
            if(!hasLength||!length)return Probe::malformed;
            bodyStart=end+4; headers=true;
        }
        if(headers) {
            std::size_t received=used-bodyStart;
            if(received>length)return Probe::malformed;
            for(std::size_t i=0;i<received;++i)out.body[i]=data[bodyStart+i];
            while(received<length) {
                if(platform::now()>deadline)return unavailable("deadline exceeded");
                const int n=platform::receive(s.fd,out.body.data()+received,length-received);
                if(n<0)return unavailable("receive timeout or failure");
                if(n==0)return unavailable("response interrupted");
                received+=static_cast<unsigned>(n);
            }
            out.length=length;return Probe::ready;
        }
    }
    return Probe::malformed;
}
} // namespace
Probe probeService() noexcept {
    Response response;
    const auto result=get("/health",{},response,platform::now()+5000000);
    return result==Probe::ready?parseHealth(response.view()):result;
}
bool parseConnection(std::string_view body,Connection& out) noexcept {
    FlatJSON json;
    if(!json.parse(body)||json.number("apiVersion")!=1)return false;
    const auto url=json.string("url"),user=json.string("username"),password=json.string("password");
    if(user!="botty"||(password.size()!=6&&password.size()!=32)||url.size()>=out.url.size())return false;
    for(char c:password)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')))return false;
    if(!url.empty()) {
        if(!url.starts_with("http://192.168.")||!url.ends_with(":9091"))return false;
        const auto address=slice(url,7,url.size()-12);unsigned parts=0;std::size_t at=0;
        while(at<address.size()) {
            unsigned n=0,digits=0;while(at<address.size()&&address[at]!='.') {
                const char c=address[at++];if(c<'0'||c>'9'||++digits>3)return false;n=n*10+static_cast<unsigned>(c-'0');
            }
            if(!digits||n>255)return false;++parts;
            if(at<address.size()&&++at==address.size())return false;
        }
        if(parts!=4)return false;
    }
    Connection next;next.status=Probe::ready;
    for(std::size_t i=0;i<url.size();++i)next.url[i]=url[i];
    for(std::size_t i=0;i<user.size();++i)next.username[i]=user[i];
    for(std::size_t i=0;i<password.size();++i)next.password[i]=password[i];
    out=next;return true;
}
Connection probeConnection(Catalog* catalog) noexcept {
    if(catalog)catalog->valid=false;
    Connection out;const auto deadline=platform::now()+5000000;
    Response health;out.status=get("/health",{},health,deadline);
    if(out.status!=Probe::ready)return out;
    out.status=parseHealth(health.view());
    if(out.status!=Probe::ready)return out;
    Response bootstrap;out.status=get("/api/bootstrap",{},bootstrap,platform::now()+5000000);
    if(out.status!=Probe::ready)return out;
    FlatJSON json;
    if(!json.parse(bootstrap.view())||json.number("apiVersion")!=1){out.status=Probe::incompatible;return out;}
    const auto token=json.string("token");
    if(token.size()!=32){out.status=Probe::malformed;return out;}
    for(char c:token)if(!((c>='a'&&c<='f')||(c>='0'&&c<='9'))){out.status=Probe::malformed;return out;}
    Response response;out.status=get("/api/connections",token,response,platform::now()+5000000);
    if(response.status==400)out.status=Probe::transmissionUnavailable;
    if(out.status==Probe::ready&&!parseConnection(response.view(),out))out.status=Probe::malformed;
    if(catalog&&(out.status==Probe::ready||out.status==Probe::transmissionUnavailable)) {
        // Worker-only storage avoids putting a large response on the PS5 thread stack.
        static Response<1048577> state;
        state.status=0;state.length=0;
        const auto result=get("/api/state",token,state,platform::now()+10000000);
        if(result!=Probe::ready)out.status=result;
        else if(!parseCatalog(state.view(),*catalog))out.status=Probe::malformed;
    }
    if(out.status!=Probe::ready){out.url.fill(0);out.username.fill(0);out.password.fill(0);}
    return out;
}
ActionResult performCommand(const Command& command) noexcept {
    ActionResult result;result.status=ActionResult::Status::failed;
    const auto message=[&](const char* text){std::snprintf(result.message.data(),result.message.size(),"%s",text);};
    static std::array<char,131072> encoded;std::size_t length=0;
    if(!encodeCommand(command,encoded.data(),encoded.size(),length)){message("Invalid action or input. Nothing was sent.");encoded.fill(0);return result;}
    Response bootstrap;auto status=get("/api/bootstrap",{},bootstrap,platform::now()+5000000);
    FlatJSON json;
    if(status!=Probe::ready||!json.parse(bootstrap.view())||json.number("apiVersion")!=1){message("Could not authenticate with Botty. Refresh and try again.");encoded.fill(0);return result;}
    auto token=json.string("token");bool tokenValid=token.size()==32;for(char c:token)if(!((c>='a'&&c<='f')||(c>='0'&&c<='9')))tokenValid=false;
    if(!tokenValid){message("Invalid service token. Nothing was sent.");encoded.fill(0);return result;}
    static Response<65537> response;response.status=0;response.length=0;bool sent=false;
    status=get(actionPath(command.operation),token,response,platform::now()+15000000,{encoded.data(),length},&sent);
    encoded.fill(0);
    if(status!=Probe::ready){result.status=sent?ActionResult::Status::uncertain:ActionResult::Status::failed;message(sent?"Response lost. Check the refreshed state before trying again; this request will not be repeated automatically.":"Could not connect to Botty. Nothing was sent.");return result;}
    std::array<char,512> error{};const bool valid=responseObject(response.view(),error);
    if(response.status!=200&&response.status!=202){message(valid&&error[0]?error.data():response.status==403?"Access expired. Refresh and try again.":"Botty rejected this request.");return result;}
    if(!valid){result.status=ActionResult::Status::uncertain;message("Invalid confirmation. Check the refreshed state before trying again.");return result;}
    if(error[0]){message(error.data());return result;}
    result.status=ActionResult::Status::success;
    switch(command.operation){
    case Operation::explore:message("Explore updated.");break;
    case Operation::search:message("Search started. Results will appear in Search.");break;
    case Operation::exploreGrab:case Operation::grab:message("Download requested. Botty will extract and prepare supported content automatically.");break;
    case Operation::pause:message("Torrent paused.");break;
    case Operation::resume:message("Torrent resumed.");break;
    case Operation::verify:message("Verification requested. Extraction waits until verification finishes.");break;
    case Operation::add:message("Torrent added or already present.");break;
    case Operation::extract:message("Extraction started. Open Extracted to follow its progress.");break;
    case Operation::move:message("Moved to the library. ShadowMount may need a scan on the next session.");break;
    case Operation::cancel:message("Cancellation requested. Waiting for a safe stop.");break;
    case Operation::dismiss:message("Removed from Extracted. Partial files from unsuccessful jobs were deleted.");break;
    case Operation::removeLibrary:message("Game files deleted from Library. Torrent and original archives were kept.");break;
    case Operation::removeTorrent:message("Torrent removed and download-file deletion requested. Library games are kept.");break;
    case Operation::remove:message("Extraction deleted. Original downloads and archive volumes were kept.");break;
    default:break;
    }
    return result;
}
bool Network::submit(const Command& command) noexcept {
    if(!thread_||state_.load()!=Probe::ready||busy_.load()||gate_.test_and_set(std::memory_order_acquire))return false;
    // One pending request, never replayed by reconnect or retry.
    if(busy_.exchange(true)){gate_.clear(std::memory_order_release);return false;}
    pending_=command;
    deletion_.store((command.operation==Operation::removeTorrent||command.operation==Operation::removeLibrary)?Deletion::deleting:Deletion::idle);
    queued_.store(true);gate_.clear(std::memory_order_release);return true;
}
void Network::publish(Connection next,const Catalog* catalog) noexcept {
    while(gate_.test_and_set(std::memory_order_acquire))platform::sleep(1000);
    next.revision=connection_.revision+1;connection_=next;
    if((next.status==Probe::unavailable||next.status==Probe::checking)&&catalog_.valid) {
        // A slow refresh must not hide an extraction that is still running.
        // Keep the last snapshot, explicitly stale, until a fresh response.
        catalog_.stale=true;
    } else if(catalog){catalog_=*catalog;catalog_.stale=false;}
    else {catalog_.valid=false;catalog_.stale=false;}
    catalog_.revision=next.revision;
    gate_.clear(std::memory_order_release);
    if(state_.exchange(next.status)!=next.status)platform::log(probeText(next.status));
}
bool Network::start() noexcept {
    stop_.store(false);
    if(!platform::startWorker(worker,this,&thread_)) {Connection failed;failed.status=Probe::workerError;publish(failed);return false;}
    return true;
}
void Network::stop() noexcept {
    stop_.store(true);
    if(thread_) { platform::joinWorker(thread_); thread_=nullptr; }
}
void* Network::worker(void* context) noexcept {
    auto& self=*static_cast<Network*>(context);
    bool checkingDeletion=false;ActionResult deletionResult;
    std::uint64_t deletionDeadline=0;
    while(!self.stop_.load()) {
        if(self.retry_.exchange(false))self.publish(Connection{});
        static Catalog next;
        bool acted=false;ActionResult result;
        if(self.queued_.exchange(false)) {
            static Command command;
            while(self.gate_.test_and_set(std::memory_order_acquire))platform::sleep(1000);
            command=self.pending_;self.pending_=Command{};
            self.gate_.clear(std::memory_order_release);
            result=performCommand(command);acted=true;
            if((command.operation==Operation::removeTorrent||command.operation==Operation::removeLibrary)&&result.status==ActionResult::Status::uncertain){
                // The service may still hold its catalog lock while unlinking.
                // Keep the operation visible until a fresh state arrives. Never
                // replay the destructive POST or infer success from a timeout.
                checkingDeletion=true;deletionResult=result;
                deletionDeadline=platform::now()+120000000;
                self.deletion_.store(Deletion::checking);acted=false;
            }
            command=Command{};
        }
        const auto connection=probeConnection(&next);
        if(checkingDeletion&&((next.valid&&next.transmissionReady)||platform::now()>=deletionDeadline)){
            checkingDeletion=false;result=deletionResult;acted=true;
            std::snprintf(result.message.data(),result.message.size(),"%s",next.valid&&next.transmissionReady?
                "Deletion response lost. The list is now up to date; check the item before trying again.":
                "Deletion is taking longer than expected or Botty is unavailable. Its outcome is not confirmed. Check the refreshed list before trying again.");
        }
        if(next.valid&&!next.transmissionReady&&self.catalog_.valid) {
            // The Botty service can answer while its Transmission RPC times out.
            // Keep only the previous torrent snapshot; extraction jobs stay live.
            next.torrents=self.catalog_.torrents;next.torrentCount=self.catalog_.torrentCount;
            next.archives=self.catalog_.archives;next.archiveCount=self.catalog_.archiveCount;
            next.transmissionStale=true;
        }
        self.publish(connection,&next);
        if(acted){
            while(self.gate_.test_and_set(std::memory_order_acquire))platform::sleep(1000);
            result.revision=self.result_.revision+1;self.result_=result;
            self.deletion_.store(Deletion::idle);self.busy_.store(false);
            self.gate_.clear(std::memory_order_release);
        }
        const unsigned delay=connection.status==Probe::unavailable||connection.status==Probe::transmissionUnavailable||next.transmissionStale?10:50;
        for(unsigned i=0;i<delay&&!self.stop_.load()&&!self.retry_.load()&&!self.queued_.load();++i)platform::sleep(100000);
    }
    return nullptr;
}
void Artwork::start() noexcept {stop_.store(false);(void)platform::startWorker(worker,this,&thread_);}
void Artwork::stop() noexcept {stop_.store(true);if(thread_){platform::joinWorker(thread_);thread_=nullptr;}}
void Artwork::request(const CoverIds& ids) noexcept {
    if(gate_.test_and_set(std::memory_order_acquire))return;
    if(ids!=requested_){requested_=ids;++requestRevision_;}
    gate_.clear(std::memory_order_release);
}
bool Artwork::read(ArtworkPage& out) noexcept {
    if(gate_.test_and_set(std::memory_order_acquire))return false;
    const bool changed=out.revision!=page_.revision;if(changed)out=page_;
    gate_.clear(std::memory_order_release);return changed;
}
void* Artwork::worker(void* context) noexcept {
    auto& self=*static_cast<Artwork*>(context);unsigned handled=0;
    while(!self.stop_.load()){
        CoverIds ids;unsigned requestRevision;
        while(self.gate_.test_and_set(std::memory_order_acquire))platform::sleep(1000);
        ids=self.requested_;requestRevision=self.requestRevision_;self.gate_.clear(std::memory_order_release);
        if(requestRevision==handled){platform::sleep(100000);continue;}handled=requestRevision;
        while(self.gate_.test_and_set(std::memory_order_acquire))platform::sleep(1000);
        self.page_.ids=ids;self.page_.ready.fill(false);++self.page_.revision;self.gate_.clear(std::memory_order_release);
        Response bootstrap;FlatJSON json;
        if(get("/api/bootstrap",{},bootstrap,platform::now()+5000000)!=Probe::ready||!json.parse(bootstrap.view()))continue;
        const auto token=json.string("token");if(token.size()!=32)continue;
        std::array<bool,6> done{};bool stale=false;
        for(unsigned attempt=0;attempt<20&&!self.stop_.load()&&!stale;++attempt){
        for(unsigned i=0;i<ids.size()&&!self.stop_.load();++i){
            if(done[i])continue;
            if(!ids[i][0])continue;
            std::array<char,288> encoded{};unsigned length=0;
            constexpr char hex[]="0123456789ABCDEF";
            for(unsigned char ch:std::string_view(ids[i].data())){
                if((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='-')encoded[length++]=static_cast<char>(ch);
                else {encoded[length++]='%';encoded[length++]=hex[ch>>4];encoded[length++]=hex[ch&15];}
            }
            char path[340];std::snprintf(path,sizeof(path),ids[i][1]==':'?"/api/artwork?id=%s":"/api/explore/artwork?id=%s",encoded.data());
            static Response<160*240*3+1> response;response.status=0;response.length=0;
            const bool ready=get(path,token,response,platform::now()+14000000)==Probe::ready&&response.length==160*240*3;
            while(self.gate_.test_and_set(std::memory_order_acquire))platform::sleep(1000);
            stale=self.requestRevision_!=requestRevision;
            done[i]=ready||response.status==404;
            if(!stale&&ready){for(unsigned b=0;b<response.length;++b)self.page_.pixels[i][b]=static_cast<unsigned char>(response.body[b]);self.page_.ready[i]=true;++self.page_.revision;}
            self.gate_.clear(std::memory_order_release);if(stale)break;
        }
        bool complete=true;for(unsigned i=0;i<6;++i)if(ids[i][0]&&!done[i])complete=false;if(complete)break;
        for(unsigned wait=0;wait<20&&!self.stop_.load();++wait)platform::sleep(100000);
        }
    }
    return nullptr;
}

}
