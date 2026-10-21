// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "core.hpp"
#include "switch-format.hpp"
#include <zlib.h>
#include <limits>
#include <vector>
namespace botty {
inline void extractZip(const fs::path& archive,const fs::path& destination,const Reporter& report,const std::function<bool()>& cancelled={}){
 const int fd=::open(archive.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
 if(fd<0)throw std::runtime_error("Cannot open ZIP archive");
 struct Close{int fd;
 ~Close(){::close(fd);
 }} close{fd};
 struct stat st{};
 if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<22)throw std::runtime_error("Invalid ZIP archive");
 const uint64_t size=st.st_size;
 const auto check=[&]{if(cancelled&&cancelled())throw std::runtime_error("Extraction cancelled");
 };
 const auto read=[&](uint64_t at,size_t n){check();
 if(at>size||n>size-at)throw std::runtime_error("Truncated ZIP archive");
 std::vector<unsigned char> b(n);
 size_t done=0;
 while(done<n){auto got=::pread(fd,b.data()+done,n-done,off_t(at+done));
 if(got<0&&errno==EINTR)continue;
 if(got<=0)throw std::runtime_error("Cannot read ZIP archive");
 done+=size_t(got);
 }return b;
 };
 auto tail=read(size-std::min<uint64_t>(size,65557),size_t(std::min<uint64_t>(size,65557)));
 size_t eocd=tail.size();
 for(size_t i=tail.size()-22;;--i){if(switchLE(tail.data()+i,4)==0x06054b50&&i+22+switchLE(tail.data()+i+20,2)==tail.size()){eocd=i;
 break;
 }if(i==0)break;
 }
 if(eocd==tail.size())throw std::runtime_error("ZIP end record missing");
 const auto* end=tail.data()+eocd;
 if(switchLE(end+4,2)||switchLE(end+6,2)||switchLE(end+8,2)!=switchLE(end+10,2))throw std::runtime_error("Split ZIP archives are not supported");
 uint64_t count=switchLE(end+10,2),centralSize=switchLE(end+12,4),centralAt=switchLE(end+16,4);
 const uint64_t endAt=size-tail.size()+eocd;
 if(count==65535||centralSize==0xffffffff||centralAt==0xffffffff){if(endAt<20)throw std::runtime_error("ZIP64 locator missing");
 const auto locator=read(endAt-20,20);
 if(switchLE(locator.data(),4)!=0x07064b50||switchLE(locator.data()+4,4)||switchLE(locator.data()+16,4)!=1)throw std::runtime_error("Invalid ZIP64 locator");
 const auto record=read(switchLE(locator.data()+8,8),56);
 if(switchLE(record.data(),4)!=0x06064b50||switchLE(record.data()+4,8)<44||switchLE(record.data()+16,4)||switchLE(record.data()+20,4)||switchLE(record.data()+24,8)!=switchLE(record.data()+32,8))throw std::runtime_error("Invalid ZIP64 directory");
 count=switchLE(record.data()+32,8);
 centralSize=switchLE(record.data()+40,8);
 centralAt=switchLE(record.data()+48,8);
 }
 if(!count||count>200000||centralSize>64*1024*1024||centralAt>endAt||centralSize>endAt-centralAt)throw std::runtime_error("ZIP directory exceeds limits");
 struct Member{fs::path name;
 uint64_t packed,bytes,data;
 uint32_t crc;
 unsigned method;
 bool directory;
 };
 std::vector<Member> members;
 std::set<std::string> names;
 uint64_t total=0,cursor=centralAt;
 std::vector<std::pair<uint64_t,uint64_t>> ranges;
 for(uint64_t i=0;i<count;++i){const auto h=read(cursor,46);
 if(switchLE(h.data(),4)!=0x02014b50)throw std::runtime_error("Invalid ZIP directory entry");
 const auto flags=switchLE(h.data()+8,2),method=switchLE(h.data()+10,2),nameLen=switchLE(h.data()+28,2),extraLen=switchLE(h.data()+30,2),commentLen=switchLE(h.data()+32,2);
 uint64_t packed=switchLE(h.data()+20,4),bytes=switchLE(h.data()+24,4),offset=switchLE(h.data()+42,4);
 const auto attrs=switchLE(h.data()+38,4),disk=switchLE(h.data()+34,2);
 if(flags&(1|64)||disk||nameLen==0||nameLen>4096||(method!=0&&method!=8)||cursor>centralAt+centralSize||46+nameLen+extraLen+commentLen>centralAt+centralSize-cursor)throw std::runtime_error("Unsupported or unsafe ZIP member");
  const auto fields=read(cursor+46,size_t(nameLen+extraLen));
 std::string raw(reinterpret_cast<const char*>(fields.data()),size_t(nameLen));
 if(raw.find('\0')!=std::string::npos||raw.find('\\')!=std::string::npos)throw std::runtime_error("Unsafe ZIP name");
 const bool directory=raw.back()=='/';
 if(directory)raw.pop_back();
 const auto name=safeRelative(raw);
 const auto mode=(attrs>>16)&0170000;
 if(mode&&mode!=(directory?0040000:0100000))throw std::runtime_error("ZIP links and special files are rejected");
 if(!names.insert(asciiLower(name.generic_string())).second)throw std::runtime_error("Duplicate ZIP paths are rejected");
  if(packed==0xffffffff||bytes==0xffffffff||offset==0xffffffff){bool found=false;
 for(size_t x=size_t(nameLen);x+4<=fields.size();){const auto tag=switchLE(fields.data()+x,2),length=switchLE(fields.data()+x+2,2);
 x+=4;
 if(length>fields.size()-x)throw std::runtime_error("Invalid ZIP extra field");
 if(tag==1){size_t at=x;
 const auto take=[&](){if(at+8>x+length)throw std::runtime_error("Missing ZIP64 size");
 const auto v=switchLE(fields.data()+at,8);
 at+=8;
 return v;
 };
 if(bytes==0xffffffff)bytes=take();
 if(packed==0xffffffff)packed=take();
 if(offset==0xffffffff)offset=take();
 found=true;
 break;
 }x+=size_t(length);
 }if(!found)throw std::runtime_error("Missing ZIP64 member metadata");
 }
  const auto local=read(offset,30);
 if(switchLE(local.data(),4)!=0x04034b50||switchLE(local.data()+6,2)!=flags||switchLE(local.data()+8,2)!=method||switchLE(local.data()+26,2)!=nameLen)throw std::runtime_error("ZIP local header mismatch");
 const auto localName=read(offset+30,size_t(nameLen));
 if(std::memcmp(localName.data(),fields.data(),size_t(nameLen)))throw std::runtime_error("ZIP member name changed");
 const uint64_t data=offset+30+nameLen+switchLE(local.data()+28,2);
  if(data>centralAt||packed>centralAt-data||bytes>std::numeric_limits<uint64_t>::max()-total||(directory&&bytes))throw std::runtime_error("Invalid ZIP member size");
 if(method==0&&bytes!=packed)throw std::runtime_error("Invalid stored ZIP size");
 total+=bytes;
 ranges.push_back({offset,data+packed});
 members.push_back({name,packed,bytes,data,uint32_t(switchLE(h.data()+16,4)),unsigned(method),directory});
 cursor+=46+nameLen+extraLen+commentLen;
 }
 if(cursor!=centralAt+centralSize)throw std::runtime_error("Unexpected ZIP directory data");
 std::sort(ranges.begin(),ranges.end());
 for(size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)throw std::runtime_error("Overlapping ZIP entries");
 const auto available=freeBytes(destination.parent_path());
 if(available<512ULL*1024*1024||total>available-512ULL*1024*1024)throw std::runtime_error("Not enough space for ZIP extraction plus 512 MiB reserve");
 if(!fs::create_directory(destination))throw std::runtime_error("ZIP staging already exists; partial files retained");
 Progress p{"Extracting ZIP and checking CRC","",0,total};
 std::vector<unsigned char> output(1024*1024);
 for(const auto& m:members){check();
 const auto target=destination/m.name;
 fs::create_directories(target.parent_path());
 if(target.parent_path()!=destination)containedExisting(destination,target.parent_path());
 if(m.directory)fs::create_directory(target);
 const int out=m.directory ? -1 : ::open(target.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
 if(!m.directory&&out<0)throw std::runtime_error("Cannot create ZIP output");
 Close closeOut{out};
 p.file=m.name.generic_string();
 uint64_t input=0,written=0;
 uLong crc=crc32(0,nullptr,0);
 z_stream z{};
 if(m.method==8&&inflateInit2(&z,-MAX_WBITS)!=Z_OK)throw std::runtime_error("Cannot initialize ZIP decoder");
 struct Inflate{z_stream* z;
 bool active;
 ~Inflate(){if(active)inflateEnd(z);
 }} inflateOwner{&z,m.method==8};
 int status=Z_OK;
  const auto emit=[&](const unsigned char* data,size_t n){if(n>m.bytes-written)throw std::runtime_error("ZIP decompressed size exceeds inventory");
 size_t done=0;
 while(done<n){auto got=::write(out,data+done,n-done);
 if(got<0&&errno==EINTR)continue;
 if(got<=0)throw std::runtime_error("Cannot write ZIP output");
 done+=size_t(got);
 }crc=crc32(crc,data,uInt(n));
 written+=n;
 p.bytes+=n;
 report(p);
 check();
 };
  while(input<m.packed){auto b=read(m.data+input,size_t(std::min<uint64_t>(1024*1024,m.packed-input)));
 input+=b.size();
 if(m.method==0)emit(b.data(),b.size());
 else{z.next_in=b.data();
 z.avail_in=uInt(b.size());
 do{z.next_out=output.data();
 z.avail_out=output.size();
 status=inflate(&z,Z_NO_FLUSH);
 if(status!=Z_OK&&status!=Z_STREAM_END)throw std::runtime_error("Corrupt ZIP compressed data");
 emit(output.data(),output.size()-z.avail_out);
 if(status==Z_STREAM_END&&(z.avail_in||input!=m.packed))throw std::runtime_error("Trailing ZIP compressed data");
 }while(z.avail_in||(!z.avail_out&&status!=Z_STREAM_END));
 }}
  if((m.method==8&&status!=Z_STREAM_END)||written!=m.bytes||crc!=m.crc)throw std::runtime_error("ZIP size or CRC mismatch; output not published");
 if(out>=0&&::fsync(out))throw std::runtime_error("Cannot flush ZIP output");
 }
 struct stat after{};
 if(::fstat(fd,&after)||after.st_size!=st.st_size||after.st_mtime!=st.st_mtime)throw std::runtime_error("ZIP source changed during extraction");
 report({"ZIP extraction verified","",total,total});
}
}
