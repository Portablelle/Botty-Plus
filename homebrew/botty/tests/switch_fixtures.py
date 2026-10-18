"""Synthetic containers with real PFS0/HFS0, NCA and CNMT layouts; no game content or keys."""
import hashlib
import struct
from pathlib import Path

def partition(files, hashed=False):
    names=b'';entries=b'';data=b''
    for name,value in files:
        entry=struct.pack('<QQII',len(data),len(value),len(names),0)
        if hashed:entry+=struct.pack('<Q',0)+bytes(32)
        entries+=entry;names+=name.encode()+b'\0';data+=value
    names+=bytes((-len(names)-16-len(entries))%16)
    return (b'HFS0' if hashed else b'PFS0')+struct.pack('<III',len(files),len(names),0)+entries+names+data

def package(kind='game', version=0, base=0x0100000000000000, corrupt=False, missing_delta=False, empty=False, duplicate=False, extra_nca=False, corrupt_metadata=False, wrong_program_id=False, wrong_meta_id=False, id_offset=0):
    title=base if kind=='game' else base+0x800 if kind=='update' else base+0x1001
    program=bytearray(4096)
    program[0x200:0x204]=b'NCA3';program[0x205]=4 if kind in ('dlc','unlocker') else 0
    struct.pack_into('<QQ',program,0x208,len(program),title+id_offset+(0x4000 if wrong_program_id else 0))
    program[-32:]=hashlib.sha256(f'{kind}:{version}'.encode()).digest()
    digest=hashlib.sha256(program).digest();name=digest[:16].hex()+'.nca'
    typ={'game':0x80,'update':0x81,'dlc':0x82,'unlocker':0x82}[kind]
    cnmt=bytearray(32);struct.pack_into('<QIBBHHH',cnmt,0,title,version,typ,0,16,1,0)
    extra=struct.pack('<QII',base if kind!='game' else base+0x800,0,0)
    if kind in ('dlc','unlocker'):extra=struct.pack('<QII',base,65536,0)
    cnmt+=extra+digest+digest[:16]+len(program).to_bytes(6,'little')+bytes([2 if kind in ('dlc','unlocker') else 1,id_offset])
    if missing_delta:
        struct.pack_into('<H',cnmt,16,2)
        cnmt+=bytes(48)+(4096).to_bytes(6,'little')+bytes([6,0])
    if empty:
        struct.pack_into('<H',cnmt,16,0);cnmt=cnmt[:48]
    if duplicate:
        struct.pack_into('<H',cnmt,16,2);cnmt+=cnmt[48:104]
    pfs=partition([('Content.cnmt',bytes(cnmt))])
    hashes=b''.join(hashlib.sha256(pfs[i:i+512]).digest() for i in range(0,len(pfs),512))
    hashes+=bytes(512-len(hashes))
    section=hashes+pfs;section+=bytes((-len(section))%512)
    meta=bytearray(0xc00+len(section));meta[0x200:0x204]=b'NCA3';meta[0x205]=1
    struct.pack_into('<QQ',meta,0x208,len(meta),title+(0x4000 if wrong_meta_id else 0))
    struct.pack_into('<II',meta,0x240,6,len(meta)//512)
    fs=bytearray(512);struct.pack_into('<HBBB',fs,0,2,1,2,1)
    struct.pack_into('<II',fs,0x28,512,2)
    struct.pack_into('<QQQQ',fs,0x30,0,512,512,len(pfs))
    fs[8:40]=hashlib.sha256(hashes).digest()
    meta[0x400:0x600]=fs;meta[0x280:0x2a0]=hashlib.sha256(fs).digest();meta[0xc00:]=section
    if corrupt_metadata:meta[0xc00+512+100]^=1
    if corrupt:program[-1]^=1
    files=[(name,bytes(program)),(hashlib.sha256(meta).digest()[:16].hex()+'.cnmt.nca',bytes(meta))]
    if extra_nca:
        other=program.copy();other[-1]^=2
        files.append((hashlib.sha256(other).digest()[:16].hex()+'.nca',bytes(other)))
    return partition(files)

def xci():
    # Convert the top-level synthetic NSP's two members into the secure HFS0.
    nsp=package();count,strings=struct.unpack_from('<II',nsp,4);data_at=16+count*24+strings;files=[]
    for i in range(count):
        off,size,name_off=struct.unpack_from('<QQI',nsp,16+i*24)
        start=16+count*24+name_off;name=nsp[start:nsp.index(0,start)].decode()
        files.append((name,nsp[data_at+off:data_at+off+size]))
    secure=partition(files,True);root=partition([('secure',secure)],True)
    header=bytearray(0xf000);header[0x100:0x104]=b'HEAD';struct.pack_into('<Q',header,0x130,len(header));return bytes(header)+root
