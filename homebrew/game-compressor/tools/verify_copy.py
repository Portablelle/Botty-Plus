#!/usr/bin/env python3
"""Read-only validation for the pinned worker's outer PFS / PFSC / exFAT output.
Verifies all encoded blocks against vhash, then all exFAT files against a separate
SHA-256 source manifest. This does not establish PS5 kernel-mount compatibility.
"""
import argparse,functools,hashlib,json,struct,zlib
from pathlib import Path

def u32(b,o):return struct.unpack_from('<I',b,o)[0]
def u64(b,o):return struct.unpack_from('<Q',b,o)[0]
def require(test,message):
 if not test:raise ValueError(message)
class Image:
 def __init__(self,path):
  self.file=Path(path).open('rb');self.size=Path(path).stat().st_size
  header=self.physical(0,65536)
  require((u64(header,0),u64(header,8),u32(header,32))==(2,20130315,65536),'Unsupported outer PFS')
  inode=self.physical(65536+3*168,168)
  self.base=u32(inode,100)*65536;self.stored=u64(inode,8)
  header=self.physical(self.base,48)
  require(header[:4]==b'PFSC' and u32(header,4)==0 and u32(header,8)==6,'Invalid PFSC header')
  require(u32(header,12)==65536 and u64(header,16)==65536,'Unsupported block size')
  self.logical=u64(header,40);count=self.logical//65536
  require(self.logical>0 and self.logical%65536==0 and count<=2**24,'Invalid logical size')
  table=u64(header,24);data=u64(header,32)
  require(table==1024 and table+(count+1)*8<=data and self.base+self.stored<=self.size,'Invalid PFSC bounds')
  self.offsets=struct.unpack('<'+'Q'*(count+1),self.physical(self.base+table,(count+1)*8))
  require(self.offsets[0]==data and self.offsets[-1]<=self.stored,'Invalid data offsets')
  require(all(0<b-a<=65536 for a,b in zip(self.offsets,self.offsets[1:])),'Invalid block spans')
 def physical(self,o,n):
  require(o>=0 and n>=0 and o+n<=self.size,'Read outside image')
  self.file.seek(o);b=self.file.read(n);require(len(b)==n,'Truncated image');return b
 @functools.lru_cache(maxsize=32)
 def block(self,i):
  start,end=self.offsets[i:i+2];stored=self.physical(self.base+start,end-start)
  if len(stored)==65536:return stored
  d=zlib.decompressobj();raw=d.decompress(stored,65537)
  require(len(raw)==65536 and d.eof and not d.unconsumed_tail and not d.unused_data,'Invalid compressed block')
  return raw
 def read(self,o,n):
  require(o>=0 and n>=0 and o+n<=self.logical,'Read outside nested image')
  b=bytearray()
  while n:
   chunk=min(n,65536-o%65536);b.extend(self.block(o//65536)[o%65536:o%65536+chunk]);o+=chunk;n-=chunk
  return bytes(b)
 def hashes(self,path):
  data=Path(path).read_bytes();require(len(data)>=4096,'Truncated vhash')
  require(data[:8]==b'PFSCVHS1' and u32(data,8)==1 and u32(data,12)==4096 and u64(data,16)==65536 and u32(data,52)==1,'Unsupported vhash')
  count=len(self.offsets)-1
  require(u64(data,24)==self.logical and u64(data,40)==count and len(data)==4096+count*32,'Vhash geometry mismatch')
  nested=u64(data,32);require(0<nested<=self.logical and self.logical-nested<65536,'Invalid nested image size')
  for i in range(count):
   raw=self.block(i);length=min(65536,nested-i*65536)
   require(hashlib.sha256(raw[:length]).digest()==data[4096+i*32:4096+(i+1)*32],f'PFSC block {i} differs from vhash')
   require(not any(raw[length:]),'Nonzero padding beyond nested image')
  return count
 def files(self):
  boot=self.read(0,512);require(boot[3:11]==b'EXFAT   ','Nested image is not exFAT')
  require(9<=boot[108]<=12 and boot[108]+boot[109]<=25,'Invalid exFAT geometry')
  sector=1<<boot[108];cluster=sector*(1<<boot[109]);fat=u32(boot,80)*sector;heap=u32(boot,88)*sector;count=u32(boot,92);root=u32(boot,96)
  require(heap+count*cluster<=self.logical,'Cluster heap exceeds image')
  def chain(first,size,contiguous=False):
   seen=set();remaining=size
   while remaining is None or remaining>0:
    require(2<=first<count+2 and first not in seen,'Invalid/cyclic cluster chain');seen.add(first)
    n=cluster if remaining is None else min(cluster,remaining)
    yield self.read(heap+(first-2)*cluster,n)
    if remaining is not None:remaining-=n
    if contiguous:first+=1
    else:
     first=u32(self.read(fat+first*4,4),0)
     if first>=0xfffffff8:
      require(remaining is None or remaining==0,'Short cluster chain');return
  result={};visited=set()
  def directory(first,size,contiguous,prefix):
   require(first not in visited and len(visited)<200000,'Duplicate/cyclic directory');visited.add(first)
   blocks=[];total=0
   for chunk in chain(first,size,contiguous):
    total+=len(chunk);require(total<=16*1024*1024,'Directory too large');blocks.append(chunk)
   entries=b''.join(blocks);i=0
   while i+32<=len(entries):
    entry=entries[i:i+32];i+=32
    if entry[0]==0:break
    if entry[0]!=0x85:continue
    n=entry[1];require(n>=2 and i+n*32<=len(entries),'Invalid file entry')
    children=[entries[i+j*32:i+(j+1)*32] for j in range(n)];i+=n*32
    stream=children[0];require(stream[0]==0xc0,'Missing stream entry')
    name=b''.join(e[2:32] for e in children[1:] if e[0]==0xc1)[:stream[3]*2].decode('utf-16le')
    require(name and name not in ['.','..'] and '/' not in name and '\\' not in name,'Invalid file name')
    path=prefix+name;first=u32(stream,20);length=u64(stream,24);valid=u64(stream,8)
    require(length==valid,'Uninitialized file data is not supported')
    isdir=bool(struct.unpack_from('<H',entry,4)[0]&16);contiguous=bool(stream[1]&2)
    if isdir:directory(first,length,contiguous,path+'/')
    else:
     require(path not in result,'Duplicate path');h=hashlib.sha256()
     for chunk in chain(first,length,contiguous):h.update(chunk)
     result[path]={'size':length,'sha256':h.hexdigest()}
  directory(root,None,False,'')
  return result

def verify(image,vhash,manifest):
 img=Image(image)
 try:
  blocks=img.hashes(vhash);actual=img.files();expected={r['path']:{'size':r['size'],'sha256':r['sha256']} for r in json.loads(Path(manifest).read_text())['files']}
  require(actual.keys()==expected.keys(),f'File set differs: missing {sorted(expected.keys()-actual.keys())}, extra {sorted(actual.keys()-expected.keys())}')
  for path in expected:require(actual[path]==expected[path],f'File differs from original: {path}')
  return dict(pfscBlocksVerified=blocks,filesVerified=len(actual),sourceBytes=sum(r['size'] for r in actual.values()),compressedBytes=img.size,kernelMountValidated=False)
 finally:img.file.close()
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('image');p.add_argument('vhash');p.add_argument('source_manifest');a=p.parse_args()
 print(json.dumps(verify(a.image,a.vhash,a.source_manifest),indent=2))
