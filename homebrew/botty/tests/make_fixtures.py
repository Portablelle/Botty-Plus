"""Tiny, original RAR4 stored fixtures. No third-party game data."""
import json, pathlib, struct, zlib
MAGIC=b'Rar!\x1a\x07\x00'
def header(kind, flags, tail):
    body=struct.pack('<BHH',kind,flags,7+len(tail))+tail
    return struct.pack('<H',zlib.crc32(body)&0xffff)+body

def file_header(name, content, total=None, flags=0, crc=None, attr=0x81a4):
    name=name.encode(); total=len(content) if total is None else total
    tail=struct.pack('<IIBIIBBHI',len(content),total,3,zlib.crc32(content) if crc is None else crc,0,20,0x30,len(name),attr)+name
    return header(0x74,0x8000|flags,tail)+content

def single(path, files, solid=False):
    payload=MAGIC+header(0x73,8 if solid else 0,b'\0'*6)
    for name,content in files:payload+=file_header(name,content)
    path.write_bytes(payload+header(0x7b,0x4000,b''))

def build(root):
    root.mkdir(parents=True,exist_ok=True)
    single(root/'app.rar',[('Demo/sce_sys/param.json',json.dumps({'titleId':'PPSA12345'}).encode()),('Demo/eboot.bin',b'original test bytes'*400)])
    single(root/'solid.rar',[('one.bin',b'a'*1024),('two.bin',b'b'*1024)],solid=True)
    single(root/'traversal.rar',[('../escape.txt',b'must never appear')])
    single(root/'absolute.rar',[('/tmp/escape.txt',b'must never appear')])
    bad=bytearray((root/'app.rar').read_bytes());bad[-20]^=1;(root/'bad-crc.rar').write_bytes(bad)
    folder=root/'multipart';folder.mkdir(exist_ok=True)
    total=b''.join(bytes([i%251])*37 for i in range(163))
    for i in range(163):
        ext='rar' if i==0 else chr(ord('r')+(i-1)//100)+f'{(i-1)%100:02d}'
        part=total[i*37:(i+1)*37];last=i==162
        flags=(1 if i else 0)|(0 if last else 2)
        data=MAGIC+header(0x73,1|(0x100 if i==0 else 0),b'\0'*6)
        data+=file_header('content.bin',part,len(total),flags,zlib.crc32(total) if last else zlib.crc32(part))
        data+=header(0x7b,0x4000|(0 if last else 1),b'')
        (folder/('sample.'+ext)).write_bytes(data)
    (root/'multipart-expected.bin').write_bytes(total)
    return root
if __name__=='__main__':build(pathlib.Path(__file__).parent/'fixtures')
