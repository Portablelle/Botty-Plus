// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#include <cstdio>
namespace botty {
namespace {
// Bounded recursive JSON reader. Views borrow the response only during parsing.
struct JSON {
    std::string_view s; std::size_t p=0;
    void ws(){while(p<s.size()&&(s[p]==' '||s[p]=='\n'||s[p]=='\r'||s[p]=='\t'))++p;}
    bool take(char c){ws();if(p==s.size()||s[p]!=c)return false;++p;return true;}
    bool string(){if(!take('"'))return false;while(p<s.size()){
        unsigned char c=s[p++];if(c=='"')return true;if(c<32)return false;
        if(c=='\\'){if(p==s.size())return false;char e=s[p++];
            if(e=='u'){for(unsigned i=0;i<4;++i){if(p==s.size())return false;char h=s[p++];if(!((h>='0'&&h<='9')||(h>='a'&&h<='f')||(h>='A'&&h<='F')))return false;}}
            else if(std::string_view("\"\\/bfnrt").find(e)==std::string_view::npos)return false;
        }
    }return false;}
    bool value(unsigned depth=0){if(depth>32)return false;ws();if(p==s.size())return false;
        if(s[p]=='"')return string();
        if(s[p]=='{'||s[p]=='['){bool object=s[p++]=='{';char end=object?'}':']';if(take(end))return true;
            do{if(object&&(!string()||!take(':')))return false;if(!value(depth+1))return false;}while(take(','));return take(end);}
        for(auto word:{std::string_view("true"),std::string_view("false"),std::string_view("null")})if(slice(s,p,word.size())==word){p+=word.size();return true;}
        if(s[p]=='-')++p;if(p==s.size())return false;
        if(s[p]=='0')++p;else{auto start=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;if(p==start)return false;}
        if(p<s.size()&&s[p]=='.'){++p;auto start=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;if(start==p)return false;}
        if(p<s.size()&&(s[p]=='e'||s[p]=='E')){++p;if(p<s.size()&&(s[p]=='+'||s[p]=='-'))++p;auto start=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;if(start==p)return false;}
        return true;
    }
};
std::string_view field(std::string_view object,std::string_view key){JSON j{object};if(!j.take('{'))return {};if(j.take('}'))return {};
    do{j.ws();auto k=j.p;if(!j.string())return {};auto name=slice(object,k+1,j.p-k-2);if(!j.take(':'))return {};j.ws();auto start=j.p;if(!j.value())return {};if(name==key)return slice(object,start,j.p-start);}while(j.take(','));return {};}
unsigned hex(char c){return c<='9'?c-'0':(c|32)-'a'+10;}
template<std::size_t N> bool decode(std::string_view s,std::array<char,N>& out,bool exact=false){out.fill(0);if(s.size()<2||s.front()!='"')return false;unsigned at=0;bool fits=true;
    auto put=[&](unsigned c){if(c==0)fits=false;if(at+1<N)out[at++]=static_cast<char>(c);else fits=false;};
    for(std::size_t i=1;i+1<s.size();++i){unsigned char c=s[i];if(c!='\\'){put(c);continue;}c=s[++i];
        if(c=='u'){unsigned cp=0;for(unsigned n=0;n<4;++n)cp=cp*16+hex(s[++i]);
            if(cp>=0xd800&&cp<=0xdbff&&i+6<s.size()&&s[i+1]=='\\'&&s[i+2]=='u'){i+=2;unsigned low=0;for(unsigned n=0;n<4;++n)low=low*16+hex(s[++i]);cp=low>=0xdc00&&low<=0xdfff?0x10000+((cp-0xd800)<<10)+low-0xdc00:0xfffd;}
            if(cp>=0xd800&&cp<=0xdfff)cp=0xfffd;
            if(cp<128)put(cp<32&&!exact?' ':cp);else if(cp<2048){put(0xc0|(cp>>6));put(0x80|(cp&63));}else if(cp<65536){put(0xe0|(cp>>12));put(0x80|((cp>>6)&63));put(0x80|(cp&63));}else{put(0xf0|(cp>>18));put(0x80|((cp>>12)&63));put(0x80|((cp>>6)&63));put(0x80|(cp&63));}
        }else {unsigned d=c=='n'?'\n':c=='r'?'\r':c=='t'?'\t':c=='b'?'\b':c=='f'?'\f':c;put(d<32&&!exact?' ':d);}
    }
    if(!fits&&!exact){out[N-4]='.';out[N-3]='.';out[N-2]='.';}
    return fits;
}
double number(std::string_view s){double n=0,scale=1;bool fraction=false,negative=false;std::size_t p=0;if(!s.empty()&&s[0]=='-'){negative=true;++p;}
    for(;p<s.size();++p){char c=s[p];if(c=='.'){fraction=true;continue;}if(c<'0'||c>'9')break;if(fraction){scale*=.1;n+=(c-'0')*scale;}else n=n*10+c-'0';}
    if(p<s.size()&&(s[p]=='e'||s[p]=='E')){++p;bool neg=p<s.size()&&s[p]=='-';if(p<s.size()&&(s[p]=='-'||s[p]=='+'))++p;unsigned e=0;for(;p<s.size();++p){e=e*10+s[p]-'0';if(e>308)return 0;}while(e--)n*=neg?.1:10;}
    if(n>1e30)return 0;return negative?-n:n;
}
bool visible(const Entry& e,unsigned tab,unsigned filter){if(tab==1&&(e.dismissed||std::string_view(e.status.data())=="moved"))return false;if(tab==2)return std::string_view(e.status.data())=="ready"||std::string_view(e.status.data())=="moved"||std::string_view(e.status.data())=="moving"||std::string_view(e.status.data())=="move-error";return tab!=0||filter==0||(filter==1?e.active:e.complete);}
}
bool responseObject(std::string_view body,std::array<char,512>& error) noexcept {
 JSON j{body};j.ws();if(j.p==body.size()||body[j.p]!='{'||!j.value())return false;j.ws();if(j.p!=body.size())return false;decode(field(body,"error"),error);return true;
}
bool firstArchive(std::string_view name) noexcept {
 if(!name.ends_with(".rar"))return false;
 auto part=std::string_view::npos;for(std::size_t i=0;i+5<=name.size();++i)if(name[i]=='.'&&(name[i+1]|32)=='p'&&(name[i+2]|32)=='a'&&(name[i+3]|32)=='r'&&(name[i+4]|32)=='t')part=i;
 if(part==std::string_view::npos)return true;
 auto digits=slice(name,part+5,name.size()-part-9);if(digits.empty())return true;
 unsigned n=0;for(char c:digits){if(c<'0'||c>'9')return true;if(n>1)return false;n=n*10+c-'0';}return n==1;
}
bool parseCatalog(std::string_view body,Catalog& out) noexcept {
    out.valid=false;out.stale=false;out.transmissionStale=false;JSON root{body};if(!root.value())return false;root.ws();if(root.p!=body.size())return false;
    auto torrents=field(body,"torrents"),jobs=field(body,"jobs"),ready=field(body,"transmissionReady");
    if(torrents.empty()||torrents.front()!='['||jobs.empty()||jobs.front()!='['||(ready!="true"&&ready!="false")||field(body,"freeBytes").empty())return false;
    out.torrentCount=out.jobCount=out.archiveCount=0;out.extracting=field(body,"extracting")=="true";out.extractionControls=field(body,"extractionControls")=="true";out.truncated=false;out.transmissionReady=ready=="true";
    {
    out.libraryDeletionSupported=field(body,"libraryDeletionSupported")=="true";
    out.torrentRemovalSupported=field(body,"torrentRemovalSupported")=="true";
    out.catalogArtworkSupported=field(body,"catalogArtworkSupported")=="true";
    out.searchSupported=field(body,"searchSupported")=="true";out.resultCount=0;
    const auto search=field(body,"search");out.searchBusy=field(search,"busy")=="true";out.searchAdding=field(search,"adding")=="true";
    decode(field(search,"query"),out.searchQuery);decode(field(search,"error"),out.searchError);decode(field(search,"notice"),out.searchNotice);
    auto results=field(search,"results");if(!results.empty()) {JSON items{results};if(!items.take('['))return false;if(!items.take(']')){do{items.ws();auto start=items.p;if(!items.value())return false;auto row=slice(results,start,items.p-start);if(out.resultCount==out.results.size())continue;auto& e=out.results[out.resultCount++];e=Entry{};if(!decode(field(row,"id"),e.id,true))return false;decode(field(row,"name"),e.name);e.total=number(field(row,"size"));e.peers=static_cast<int>(number(field(row,"seeders")));e.downloadingPeers=static_cast<int>(number(field(row,"leechers")));e.complete=field(row,"added")=="true";}while(items.take(','));if(!items.take(']'))return false;}}
    }
    {
    out.exploreSupported=field(body,"exploreSupported")=="true";out.exploreCount=0;
    const auto search=field(body,"explore");out.exploreBusy=field(search,"busy")=="true";out.exploreAdding=field(search,"adding")=="true";
    decode(field(search,"sort"),out.exploreSort);decode(field(search,"error"),out.exploreError);decode(field(search,"notice"),out.exploreNotice);
    auto results=field(search,"results");if(!results.empty()) {JSON items{results};if(!items.take('['))return false;if(!items.take(']')){do{items.ws();auto start=items.p;if(!items.value())return false;auto row=slice(results,start,items.p-start);if(out.exploreCount==out.exploreResults.size())continue;auto& e=out.exploreResults[out.exploreCount++];e=Entry{};if(!decode(field(row,"id"),e.id,true))return false;decode(field(row,"name"),e.name);e.total=number(field(row,"size"));e.peers=static_cast<int>(number(field(row,"seeders")));e.downloadingPeers=static_cast<int>(number(field(row,"leechers")));e.completedCount=static_cast<int>(number(field(row,"completed")));decode(field(row,"published"),e.published);e.complete=field(row,"added")=="true";}while(items.take(','));if(!items.take(']'))return false;}}
    }
    out.freeBytes=number(field(body,"freeBytes"));decode(field(body,"library"),out.library);decode(field(body,"error"),out.error);
    for(unsigned type=0;type<2;++type){auto array=type?jobs:torrents;JSON j{array};j.take('[');if(j.take(']'))continue;
        do{j.ws();auto start=j.p;if(!j.value())return false;auto obj=slice(array,start,j.p-start);if(obj.empty()||obj.front()!='{')return false;
            auto& count=type?out.jobCount:out.torrentCount;auto& entries=type?out.jobs:out.torrents;
            if(count==entries.size()){out.truncated=true;continue;}auto& e=entries[count++];e=Entry{};
            auto id=field(obj,"id");if(id.empty()||field(obj,"name").empty())return false;
            if(type)decode(id,e.id);else std::snprintf(e.id.data(),e.id.size(),"%.0f",number(id));
            decode(field(obj,"name"),e.name);
            if(type){e.dismissed=field(obj,"dismissed")=="true";decode(field(obj,"status"),e.status);decode(field(obj,"phase"),e.phase);decode(field(obj,"error"),e.error);
                auto content=field(obj,"content");decode(field(content,"kind"),e.kind);if(!e.error[0])decode(field(content,"reason"),e.error);
                decode(field(obj,"destination"),e.destination);if(!e.destination[0])decode(field(content,"destination"),e.destination);
                e.bytes=number(field(obj,"bytes"));e.total=number(field(obj,"total"));e.progress=e.total>0?e.bytes/e.total:0;
                e.active=std::string_view(e.status.data())=="extracting";
                if(e.active){e.download=number(field(obj,"extractionRate"));const auto eta=field(obj,"eta");if(!eta.empty())e.eta=number(eta);e.etaEstimated=true;}
            }else{const int status=static_cast<int>(number(field(obj,"status")));e.complete=number(field(obj,"leftUntilDone"))==0;e.active=status!=0;
                const char* label=status==0?"Paused":status==1||status==2?"Verifying":e.complete?"Completed":status==3?"Queued":"Downloading";
                std::snprintf(e.status.data(),e.status.size(),"%s",label);decode(field(obj,"errorString"),e.error);e.total=number(field(obj,"totalSize"));e.download=number(field(obj,"rateDownload"));e.upload=number(field(obj,"rateUpload"));e.progress=number(field(obj,"percentDone"));
                const auto count=[&](std::string_view key){const auto value=field(obj,key);const double n=value.empty()?-1:number(value);return n>=0&&n<=2147483647?static_cast<int>(n):-1;};
                e.peers=count("peersConnected");e.downloadingPeers=count("peersSendingToUs");e.uploadingPeers=count("peersGettingFromUs");
                const auto wanted=field(obj,"sizeWhenDone");if(!wanted.empty())e.total=number(wanted);
                double remaining=number(field(obj,"leftUntilDone"));if(remaining<0)remaining=0;if(remaining>e.total)remaining=e.total;
                e.bytes=e.total-remaining;
                const auto eta=field(obj,"eta");
                if(!eta.empty())e.eta=number(eta);
                else if(status==4&&e.download>0){e.eta=remaining/e.download;e.etaEstimated=true;}
                if(status!=4)e.eta=-1;
                e.extractable=e.complete&&status!=1&&status!=2&&number(field(obj,"error"))==0&&!e.error[0];
                e.archiveStart=out.archiveCount;
                auto files=field(obj,"files");JSON f{files};unsigned used=0;
                if(f.take('[')&&!f.take(']'))do {f.ws();auto begin=f.p;if(!f.value())return false;++e.fileCount;
                    std::array<char,512> name{};decode(field(slice(files,begin,f.p-begin),"name"),name);
                    std::array<char,4096> archive{};
                    const bool exact=decode(field(slice(files,begin,f.p-begin),"name"),archive,true);
                    if(!exact)e.archivesOmitted=true;
                    if(firstArchive(archive.data())){if(exact&&out.archiveCount<out.archives.size()){out.archives[out.archiveCount++]=archive;++e.archiveCount;}else e.archivesOmitted=true;}
                    const auto length=std::string_view(name.data()).size();
                    if(used+length+2<e.files.size()){for(char ch:std::string_view(name.data()))e.files[used++]=ch;e.files[used++]='\n';}
                    else if(used+5<e.files.size()){for(char ch:std::string_view("...\n"))e.files[used++]=ch;}
                }while(f.take(','));
            }
            if(type&&(std::string_view(e.status.data())=="ready"||std::string_view(e.status.data())=="moved"))e.progress=1;
            if(e.progress<0)e.progress=0;if(e.progress>1)e.progress=1;
        }while(j.take(','));
    }
    out.valid=true;return true;
}
unsigned entryCount(const Catalog& s,unsigned tab,unsigned filter) noexcept {unsigned n=0;const auto count=tab?s.jobCount:s.torrentCount;const auto& entries=tab?s.jobs:s.torrents;for(unsigned i=0;i<count;++i)if(visible(entries[i],tab,filter))++n;return n;}
const Entry* entryAt(const Catalog& s,unsigned tab,unsigned filter,unsigned index) noexcept {const auto count=tab?s.jobCount:s.torrentCount;const auto& entries=tab?s.jobs:s.torrents;for(unsigned i=0;i<count;++i)if(visible(entries[i],tab,filter)){if(index--==0)return &entries[i];}return nullptr;}
void formatBytes(double n,char* out,unsigned length) noexcept {const char* units[]={"B","KiB","MiB","GiB","TiB"};unsigned u=0;while(n>=1024&&u<4){n/=1024;++u;}std::snprintf(out,length,u?"%.1f %s":"%.0f %s",n,units[u]);}
void formatETA(const Entry& e,char* out,unsigned length) noexcept {
    if(e.complete){std::snprintf(out,length,"Completed");return;}
    if(e.eta<0||e.eta>315360000){std::snprintf(out,length,"ETA unavailable");return;}
    const auto seconds=static_cast<unsigned long long>(e.eta);
    const char* approx=e.etaEstimated?"~":"";
    if(seconds<60)std::snprintf(out,length,"ETA %s<1 min",approx);
    else if(seconds<3600)std::snprintf(out,length,"ETA %s%llu min",approx,seconds/60);
    else if(seconds<86400)std::snprintf(out,length,"ETA %s%llu h %llu min",approx,seconds/3600,(seconds%3600)/60);
    else std::snprintf(out,length,"ETA %s%llu d %llu h",approx,seconds/86400,(seconds%86400)/3600);
}

}
