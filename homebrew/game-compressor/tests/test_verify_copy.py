import hashlib,importlib.util,json,struct,tempfile,unittest,zlib
from pathlib import Path
spec=importlib.util.spec_from_file_location('verify_copy',Path(__file__).resolve().parents[1]/'tools/verify_copy.py');v=importlib.util.module_from_spec(spec);spec.loader.exec_module(v)
class VerifyTests(unittest.TestCase):
 def test_full_file_and_corruption(self):
  with tempfile.TemporaryDirectory() as temp:
   r=Path(temp);B=65536;raw=bytearray(B*4);source=b'original fixture '*5000
   raw[3:11]=b'EXFAT   ';raw[108]=9;raw[109]=7
   struct.pack_into('<IIIII',raw,80,24,1,128,3,2);struct.pack_into('<I',raw,24*512+8,0xffffffff)
   entry=B;raw[entry]=0x85;raw[entry+1]=2;raw[entry+32]=0xc0;raw[entry+33]=3;raw[entry+35]=8
   struct.pack_into('<Q',raw,entry+40,len(source));struct.pack_into('<I',raw,entry+52,3);struct.pack_into('<Q',raw,entry+56,len(source))
   raw[entry+64]=0xc1;raw[entry+66:entry+82]='file.bin'.encode('utf-16le');raw[B*2:B*2+len(source)]=source
   chunks=[bytes(raw[i:i+B]) for i in range(0,len(raw),B)];encoded=[zlib.compress(c) for c in chunks];offsets=[B]
   for e in encoded:offsets.append(offsets[-1]+len(e))
   pfsc=bytearray(B);pfsc[:4]=b'PFSC';struct.pack_into('<IIQQQQ',pfsc,8,6,B,B,1024,B,len(raw));struct.pack_into('<5Q',pfsc,1024,*offsets)
   outer=bytearray(B*4);struct.pack_into('<QQ',outer,0,2,20130315);struct.pack_into('<I',outer,32,B)
   inode=B+3*168;struct.pack_into('<QQ',outer,inode+8,offsets[-1],len(raw));struct.pack_into('<I',outer,inode+100,4)
   image=r/'image.ffpfsc';image.write_bytes(outer+pfsc+b''.join(encoded))
   side=bytearray(4096);side[:8]=b'PFSCVHS1';struct.pack_into('<IIQQQQII',side,8,1,4096,B,len(raw),len(raw),4,2,1)
   hashes=r/'image.ffpfsc.vhash';hashes.write_bytes(side+b''.join(hashlib.sha256(c).digest() for c in chunks))
   manifest=r/'source.json';manifest.write_text(json.dumps({'files':[dict(path='file.bin',size=len(source),sha256=hashlib.sha256(source).hexdigest())]}))
   result=v.verify(image,hashes,manifest);self.assertEqual(result['filesVerified'],1);self.assertEqual(result['pfscBlocksVerified'],4)
   original=hashes.read_bytes()
   partial=bytearray(original);struct.pack_into('<Q',partial,32,len(raw)-4096);partial[-32:]=hashlib.sha256(chunks[-1][:-4096]).digest();hashes.write_bytes(partial)
   image_reader=v.Image(image);self.assertEqual(image_reader.hashes(hashes),4);image_reader.file.close();hashes.write_bytes(original)
   bad=bytearray(original);bad[-1]^=1;hashes.write_bytes(bad)
   with self.assertRaises(ValueError):v.verify(image,hashes,manifest)
   hashes.write_bytes(original);manifest.write_text(json.dumps({'files':[]}))
   with self.assertRaises(ValueError):v.verify(image,hashes,manifest)
   damaged=bytearray(image.read_bytes());damaged[-1]^=1;image.write_bytes(damaged)
   with self.assertRaises((ValueError,zlib.error)):v.verify(image,hashes,manifest)
if __name__=='__main__':unittest.main()
