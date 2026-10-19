// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "switch-format.hpp"
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <fstream>
#include <sstream>
#include <iomanip>
namespace botty {
inline std::string switchHex(const unsigned char* b,size_t n){std::string s;
 for(size_t i=0;i<n;++i){s+='0'+(b[i]>>4);
 if(s.back()>'9')s.back()+='a'-'0'-10;
 s+='0'+(b[i]&15);
 if(s.back()>'9')s.back()+='a'-'0'-10;
 }return s;
 }
inline std::string switchTitleId(uint64_t v){std::ostringstream s;
 s<<std::hex<<std::uppercase<<std::setw(16)<<std::setfill('0')<<v;
 return s.str();
 }
class SwitchKeys {
 std::map<std::string,std::vector<unsigned char>> values;
public:
 explicit SwitchKeys(const fs::path& file){if(file.empty()||!fs::exists(file))return;
 const auto text=readText(file,1024*1024);
 std::istringstream lines(text);
 std::string line;
 while(std::getline(lines,line)){const auto eq=line.find('=');
 if(eq==std::string::npos)continue;
 auto key=asciiLower(line.substr(0,eq)),hex=asciiLower(line.substr(eq+1));
 const auto strip=[](std::string& s){s.erase(std::remove_if(s.begin(),s.end(),[](unsigned char c){return std::isspace(c);}),s.end());
 };
 strip(key);
 strip(hex);
 if(hex.empty()||hex.size()%2||hex.size()>128||hex.find_first_not_of("0123456789abcdef")!=std::string::npos)continue;
 std::vector<unsigned char> bytes;
 for(size_t i=0;i<hex.size();i+=2)bytes.push_back(static_cast<unsigned char>(std::stoul(hex.substr(i,2),nullptr,16)));
 if(!values.emplace(key,bytes).second)throw std::runtime_error("Duplicate private Switch key names");
 }}
 ~SwitchKeys(){for(auto& e:values)OPENSSL_cleanse(e.second.data(),e.second.size());
 }
 const std::vector<unsigned char>& get(const std::string& name,size_t size)const{auto it=values.find(name);
 if(it==values.end()||it->second.size()!=size)throw std::runtime_error("Private Switch metadata key unavailable ("+name+"). Refresh private provisioning in Portal.");
 return it->second;
 }
};
inline std::vector<unsigned char> switchDecrypt(const EVP_CIPHER* cipher,const unsigned char* key,const unsigned char* iv,const std::vector<unsigned char>& input){
 EVP_CIPHER_CTX* context=EVP_CIPHER_CTX_new();
 if(!context)throw std::runtime_error("Cannot initialize Switch metadata reader");
 struct Free{EVP_CIPHER_CTX* p;
 ~Free(){EVP_CIPHER_CTX_free(p);
 }} free{context};
 std::vector<unsigned char> out(input.size()+32);
 int n=0,last=0;
 if(EVP_DecryptInit_ex(context,cipher,nullptr,key,iv)!=1||EVP_CIPHER_CTX_set_padding(context,0)!=1||EVP_DecryptUpdate(context,out.data(),&n,input.data(),int(input.size()))!=1||EVP_DecryptFinal_ex(context,out.data()+n,&last)!=1)throw std::runtime_error("Cannot decrypt Switch metadata with provisioned keys");
 out.resize(size_t(n+last));
 return out;
}
inline std::array<unsigned char,32> switchDigest(const unsigned char* bytes,size_t n){std::array<unsigned char,32> out{};
 unsigned count=0;
 if(EVP_Digest(bytes,n,out.data(),&count,EVP_sha256(),nullptr)!=1||count!=32)throw std::runtime_error("Switch SHA-256 verification failed");
 return out;
 }
inline json inspectSwitchMetadata(const fs::path& file,const SwitchKeys& keys,const Reporter& report={},const std::function<void()>& check={}){
 SwitchContainer container(file);
 json titles=json::array();
 std::set<std::string> identities, metadataFiles;
 std::map<uint64_t,std::array<unsigned char,32>> verifiedContent;
 std::map<uint64_t,unsigned> ncaTypes, requiredTypes;
 std::map<uint64_t,uint64_t> ncaIds, requiredIds;
 uint64_t checked=0,total=0,omittedDeltaFragments=0;
 for(const auto& e:container.contents)total+=e.second.size;
 const auto verifyFile=[&](const SwitchMember& entry,const unsigned char* expected){
  std::array<unsigned char,32> wanted{};std::copy_n(expected,32,wanted.begin());
 const auto previous=verifiedContent.find(entry.start);
 if(previous!=verifiedContent.end()){
  if(previous->second!=wanted)throw std::runtime_error("Conflicting NCA content hashes");
  return;
 }
  EVP_MD_CTX* context=EVP_MD_CTX_new();
 if(!context)throw std::runtime_error("Cannot initialize content verification");
 struct Free{EVP_MD_CTX* p;
 ~Free(){EVP_MD_CTX_free(p);
 }} free{context};
 if(EVP_DigestInit_ex(context,EVP_sha256(),nullptr)!=1)throw std::runtime_error("Cannot initialize SHA-256");
  for(uint64_t at=0;at<entry.size;){if(check)check();
 auto b=container.read(entry.start+at,size_t(std::min<uint64_t>(1024*1024,entry.size-at)));
 if(EVP_DigestUpdate(context,b.data(),b.size())!=1)throw std::runtime_error("Switch content hash failed");
 at+=b.size();
 checked+=b.size();
 if(report)report({"Verifying Switch content hashes",file.filename().string(),checked,total});
 }
  std::array<unsigned char,32> digest{};
 unsigned n=0;
 if(EVP_DigestFinal_ex(context,digest.data(),&n)!=1||n!=32||CRYPTO_memcmp(digest.data(),expected,32))throw std::runtime_error("Switch content SHA-256 mismatch; nothing imported");
 verifiedContent.emplace(entry.start,wanted);
 };
 for(const auto& member:container.contents){if(asciiLower(fs::path(member.first).extension().string())!=".nca")continue;
 if(check)check();
 if(member.second.size<0xc00)throw std::runtime_error("Truncated NCA header");
 auto header=container.read(member.second.start,0xc00);
  if(std::memcmp(header.data()+0x200,"NCA3",4)){
   const auto& key=keys.get("header_key",32);
 for(unsigned sector=0;sector<6;++sector){std::array<unsigned char,16> iv{};
 iv[15]=sector; // Nintendo XTS sector numbers are encoded big endian.
 std::vector<unsigned char> block(header.begin()+sector*512,header.begin()+(sector+1)*512);
 block=switchDecrypt(EVP_aes_128_xts(),key.data(),iv.data(),block);
 std::copy(block.begin(),block.end(),header.begin()+sector*512);
 }
   if(std::memcmp(header.data()+0x200,"NCA3",4))throw std::runtime_error("Unsupported NCA header or incorrect private header key");
  }
  if(switchLE(header.data()+0x208,8)!=member.second.size)throw std::runtime_error("NCA length does not match the container");
 ncaTypes[member.second.start]=header[0x205];
 ncaIds[member.second.start]=switchLE(header.data()+0x210,8);
 if(header[0x205]!=1)continue;
  metadataFiles.insert(member.first);
  bool found=false;
  for(unsigned section=0;section<4;++section){const uint64_t start=switchLE(header.data()+0x240+section*16,4)*512,end=switchLE(header.data()+0x244+section*16,4)*512;
 if(!start&&!end)continue;
 if(start<0xc00||end<=start||end>member.second.size)throw std::runtime_error("Invalid metadata NCA section");
 const auto* fs=header.data()+0x400+section*512;
 const auto digest=switchDigest(fs,512);
 if(CRYPTO_memcmp(digest.data(),header.data()+0x280+section*32,32))throw std::runtime_error("Metadata NCA filesystem header hash mismatch");
 if(fs[2]!=1||fs[3]!=2)throw std::runtime_error("Unsupported metadata filesystem; manual inspection required");
   const auto layers=switchLE(fs+0x2c,4);
 if(layers<2||layers>5)throw std::runtime_error("Invalid metadata hash layers");
 const uint64_t region=switchLE(fs+0x30+(layers-1)*16,8),length=switchLE(fs+0x38+(layers-1)*16,8);
 if(region>end-start||length>end-start-region||length<16||length>16*1024*1024)throw std::runtime_error("Invalid CNMT partition bounds");
   std::vector<unsigned char> ctrKey;
   if(fs[4]!=1){if(fs[4]!=3)throw std::runtime_error("Unsupported metadata encryption; manual inspection required");
 if(std::any_of(header.begin()+0x230,header.begin()+0x240,[](unsigned char c){return c!=0;}))throw std::runtime_error("Rights-managed metadata needs manual inspection");
 const auto generation=std::max(header[0x206],header[0x220]);
 const auto index=header[0x207];
 if(index>2)throw std::runtime_error("Unsupported metadata key area");
 char name[64];
 std::snprintf(name,sizeof(name),"key_area_key_%s_%02x",index==0?"application":index==1?"ocean":"system",generation?generation-1:0);
 const auto& area=keys.get(name,16);
 ctrKey=switchDecrypt(EVP_aes_128_ecb(),area.data(),nullptr,std::vector<unsigned char>(header.begin()+0x320,header.begin()+0x330));
 }
   const auto readSection=[&](uint64_t at,size_t n){if(at>end-start||n>end-start-at)throw std::runtime_error("CNMT exceeds NCA section");
 if(fs[4]==1)return container.read(member.second.start+start+at,n);
 const uint64_t absolute=start+at,aligned=absolute&~uint64_t(15);
 const auto prefix=size_t(absolute-aligned);
 auto bytes=container.read(member.second.start+aligned,n+prefix);
 std::array<unsigned char,16> iv{};
 for(unsigned k=0;k<8;++k){iv[k]=fs[0x147-k];
 iv[15-k]=static_cast<unsigned char>((aligned/16)>>(k*8));
 }bytes=switchDecrypt(EVP_aes_128_ctr(),ctrKey.data(),iv.data(),bytes);
 bytes.erase(bytes.begin(),bytes.begin()+prefix);
 return bytes;
 };
   // Hierarchical SHA-256 binds the CNMT bytes to the filesystem header.
   // Each upper layer contains the hashes of blocks in the next layer.
   const auto blockSize=switchLE(fs+0x28,4);
   if(blockSize<32||blockSize>16*1024*1024||(blockSize&(blockSize-1)))throw std::runtime_error("Invalid metadata hash block size");
   std::vector<std::pair<uint64_t,std::vector<unsigned char>>> hashLayers;
   for(uint64_t layer=0;layer<layers;++layer){
    const auto at=switchLE(fs+0x30+layer*16,8),size=switchLE(fs+0x38+layer*16,8);
    if(!size||size>16*1024*1024||at>end-start||size>end-start-at)throw std::runtime_error("Invalid metadata hash layer bounds");
    for(const auto& previous:hashLayers)if(at<previous.first+previous.second.size()&&previous.first<at+size)throw std::runtime_error("Overlapping metadata hash layers");
    if(check)check();hashLayers.emplace_back(at,readSection(at,size_t(size)));
   }
   const auto master=switchDigest(hashLayers[0].second.data(),hashLayers[0].second.size());
   if(CRYPTO_memcmp(master.data(),fs+8,32))throw std::runtime_error("Metadata SHA-256 master hash mismatch");
   for(size_t layer=1;layer<hashLayers.size();++layer){
    const auto& table=hashLayers[layer-1].second;const auto& bytes=hashLayers[layer].second;
    if((bytes.size()+blockSize-1)/blockSize>table.size()/32)throw std::runtime_error("Metadata hash table is incomplete");
    for(size_t at=0,index=0;at<bytes.size();at+=size_t(blockSize),++index){
     if(check)check();const auto digest=switchDigest(bytes.data()+at,std::min<size_t>(size_t(blockSize),bytes.size()-at));
     if(CRYPTO_memcmp(digest.data(),table.data()+index*32,32))throw std::runtime_error("Metadata SHA-256 data hash mismatch");
    }
   }
   const auto& pfs=hashLayers.back().second;
 if(std::memcmp(pfs.data(),"PFS0",4))throw std::runtime_error("Cannot read metadata partition with provisioned keys");
 const auto entries=switchLE(pfs.data()+4,4),strings=switchLE(pfs.data()+8,4);
 if(!entries||entries>4096||strings>1024*1024||16+entries*24+strings>pfs.size())throw std::runtime_error("Invalid CNMT file table");
 const uint64_t dataAt=16+entries*24+strings;
   for(uint64_t i=0;i<entries;++i){const auto* e=pfs.data()+16+i*24;
 const auto offset=switchLE(e,8),bytes=switchLE(e+8,8),nameAt=switchLE(e+16,4);
 if(offset>pfs.size()-dataAt||bytes>pfs.size()-dataAt-offset||nameAt>=strings)throw std::runtime_error("Invalid CNMT member");
 const auto* name=reinterpret_cast<const char*>(pfs.data()+16+entries*24+nameAt);
 const auto* nul=static_cast<const char*>(std::memchr(name,0,size_t(strings-nameAt)));
 if(!nul)throw std::runtime_error("Unterminated CNMT name");
 if(std::string(name,nul).size()<5||std::string(name,nul).substr(std::string(name,nul).size()-5)!=".cnmt")continue;
 if(bytes<32)throw std::runtime_error("Truncated CNMT");
 const auto* cnmt=pfs.data()+dataAt+offset;
 const auto title=switchLE(cnmt,8),version=switchLE(cnmt+8,4),type=uint64_t(cnmt[12]),extended=switchLE(cnmt+14,2),count=switchLE(cnmt+16,2);
 if(type!=0x80&&type!=0x81&&type!=0x82)throw std::runtime_error("Unsupported Switch content type; not a game, update or DLC");
 if(32+extended+count*56>bytes||extended<16)throw std::runtime_error("Invalid CNMT content inventory");
 const auto base=type==0x80?title:switchLE(cnmt+32,8);
 if(!title||!base)throw std::runtime_error("CNMT title identity missing");
 if(ncaIds.at(member.second.start)!=title)throw std::runtime_error("Metadata NCA title identity does not match CNMT");
 const auto id=switchTitleId(title);
 if(!identities.insert(id).second)throw std::runtime_error("Duplicate title metadata");
    bool requiredContent=false;std::set<std::string> titleContent;
    for(uint64_t j=0;j<count;++j){const auto* record=cnmt+32+extended+j*56;
 const auto ncaName=switchHex(record+32,16)+".nca";
 if(!titleContent.insert(ncaName).second)throw std::runtime_error("Duplicate CNMT content reference");
 if(record[54]<1||record[54]>6)throw std::runtime_error("Unsupported CNMT content record");
 requiredContent=requiredContent||(type==0x82?record[54]==2:record[54]==1);
 const auto actual=container.contents.find(ncaName);
 // Full update packages do not need the optional delta fragments used to patch
 // an older installation. Never waive missing program/data/control content.
 if(actual==container.contents.end()&&type==0x81&&record[54]==6){++omittedDeltaFragments;continue;}
 if(actual==container.contents.end()||actual->second.size!=switchLE(record+48,6))throw std::runtime_error("Missing or incomplete NCA content "+ncaName+" (expected "+std::to_string(switchLE(record+48,6))+", actual "+(actual==container.contents.end()?std::string("absent; content type ")+std::to_string(record[54]):std::to_string(actual->second.size))+"); obtain the complete release");
 if(record[54]==1||record[54]==2){const unsigned expectedType=record[54]==1?0:4;
  const auto prior=requiredTypes.emplace(actual->second.start,expectedType);if(!prior.second&&prior.first->second!=expectedType)throw std::runtime_error("Conflicting NCA content types");}
 // Patch program NCAs keep the application's identity, while their metadata
 // NCA uses the patch title ID. The content ID offset applies to both.
 const uint64_t contentBase=type==0x81?base:title;
 const uint64_t contentId=contentBase+record[55];
 if(contentId<contentBase)throw std::runtime_error("CNMT content identity overflow");
 const auto priorId=requiredIds.emplace(actual->second.start,contentId);
 if(!priorId.second&&priorId.first->second!=contentId)throw std::runtime_error("Conflicting CNMT content identities");
 verifyFile(actual->second,record);
 }
    if(!requiredContent)throw std::runtime_error("CNMT has no required program or DLC data content");
    const auto required=type==0x82?switchLE(cnmt+40,4):0;
    titles.push_back({{"type",type==0x80?"game":type==0x81?"update":"dlc"},{"titleId",id},{"baseTitleId",switchTitleId(base)},{"version",version},{"requiredApplicationVersion",required},{"verified",true}});
 found=true;
   }
  }
  if(!found)throw std::runtime_error("Metadata NCA contains no readable CNMT");
 }
 if(titles.empty())throw std::runtime_error("No identifiable title metadata. A bare ticket/unlocker is not an importable game or DLC.");
 for(const auto& required:requiredIds)if(ncaIds.at(required.first)!=required.second)throw std::runtime_error("NCA program identity does not match CNMT");
 for(const auto& required:requiredTypes){
  const auto actual=ncaTypes.at(required.first);
  // CNMT Data may be stored as either NCA Data (4) or PublicData (5).
  if(actual!=required.second&&!(required.second==4&&actual==5))throw std::runtime_error("CNMT program or data does not match its NCA content type");
 }
 for(const auto& entry:container.contents)if(asciiLower(fs::path(entry.first).extension().string())==".nca"&&!metadataFiles.count(entry.first)&&!verifiedContent.count(entry.second.start))throw std::runtime_error("Unreferenced NCA content; select a reconciled package");
 std::set<std::string> bases;
 bool game=false;
 for(const auto& t:titles){bases.insert(t.at("baseTitleId"));
 game=game||t.at("type")=="game";
 }
 if(bases.size()!=1)throw std::runtime_error("Multiple unrelated games in one container; select separate game packages");
 json primary=titles[0];
 for(const auto& t:titles)if((game&&t.at("type")=="game")||(!game&&t.at("version")>primary.at("version")))primary=t;
 primary["titles"]=titles;primary["omittedDeltaFragments"]=omittedDeltaFragments;
 primary["kind"]=game?"roms":"updates";
 primary["verification"]="CNMT inventory and content SHA-256";
 primary["includesUpdate"]=game&&std::any_of(titles.begin(),titles.end(),[](const json& t){return t.at("type")=="update";});
 return primary;
}
}
