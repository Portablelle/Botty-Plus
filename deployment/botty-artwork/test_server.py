import importlib.util, io, json, os, pathlib, tempfile, unittest
from PIL import Image
class Covers(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();root=pathlib.Path(self.temp.name);(root/'key').write_text('test')
  os.environ['BOTTY_ARTWORK_KEY_FILE']=str(root/'key');os.environ['BOTTY_ARTWORK_CACHE']=str(root)
  spec=importlib.util.spec_from_file_location('artwork',pathlib.Path(__file__).with_name('server.py'));self.module=importlib.util.module_from_spec(spec);spec.loader.exec_module(self.module)
 def tearDown(self):self.temp.cleanup()
 def test_exact_cover_and_cache(self):
  image=io.BytesIO();picture=Image.new('RGB',(20,30),(20,60,80));picture.paste((220,180,100),(0,0,10,15));picture.save(image,format='PNG');calls=[]
  def fetch(url,limit):
   calls.append(url)
   return json.dumps({'query':{'pages':[{'title':"Marvel's Demo",'thumbnail':{'source':'https://upload.wikimedia.org/test.png'}}]}}).encode() if 'api.php' in url else image.getvalue()
  self.module.fetch=fetch
  result=self.module.cover('marvels demo incl DLC');self.assertEqual(len(result),115200);self.assertEqual(self.module.cover('marvels demo incl DLC'),result);self.assertEqual(len(calls),3)
 def test_never_substitutes_sequel(self):
  self.module.fetch=lambda *_:json.dumps({'query':{'pages':[{'title':'Demo 2','thumbnail':{'source':'https://upload.wikimedia.org/test.png'}}]}}).encode()
  self.assertEqual(self.module.cover('demo'),b'')
 def test_accents_and_punctuation(self):
  self.assertEqual(self.module.normalize("Ghost of Yōtei"), self.module.normalize("ghost of yotei"))
  self.assertEqual(self.module.normalize("Marvel's Wolverine"), self.module.normalize("marvels wolverine"))
 def test_release_names(self):
  self.assertEqual(self.module.clean_title('Little Nightmares III The Shores'), 'Little Nightmares III')
  self.assertEqual(self.module.clean_title('007 First Light Deluxe Edition PROPER PS5-PPSA11386[FPKG]'), '007 First Light')
  self.assertEqual(self.module.clean_title('Example Game REPACK PS5-PPSA00000[FPKG]'), 'Example Game')
  self.assertEqual(self.module.clean_title('Super Mega Baseball 4 Ballpark Edition PS5-PPSA06142[FPKG]'), 'Super Mega Baseball 4')
  self.assertEqual(self.module.clean_title('God of War Sons of Sparta PROPER PS5-PPSA28997[FPKG]'), 'God of War Sons of Sparta')
  self.assertEqual(self.module.clean_title('Kena Bridge of Spirits PROPER PS5-PPSA01746[FPKG]'), 'Kena Bridge of Spirits')
  self.assertEqual(self.module.clean_title('God of War Sons of Sparta Deluxe Edition DLC ONLY PS5-PPSA28997[FPKG]'), 'God of War Sons of Sparta')
  self.assertEqual(self.module.clean_title('Super Mega Baseball 4 Ballpark Edition DLC ONLY PS5-PPSA06142[FPKG]'), 'Super Mega Baseball 4')
  self.assertEqual(self.module.clean_title('Example Game Deluxe Edition DLC Addon PROPER REPACK PS5-PPSA00000[FPKG]'), 'Example Game')
  self.assertEqual(self.module.clean_title('Example Game Deluxe Edition PROPER DLC Addon PS5-PPSA00000[FPKG]'), 'Example Game')
  self.assertEqual(self.module.clean_title('God of War Ghost of Sparta PS5'), 'God of War Ghost of Sparta')
  self.assertEqual(self.module.normalize('Little Nightmares III'), self.module.normalize('Little Nightmares 3'))
  self.assertNotEqual(self.module.normalize('Little Nightmares III'), self.module.normalize('Little Nightmares II'))
  self.assertEqual(self.module.normalize('ratchet and clank rift apart'), self.module.normalize('Ratchet & Clank: Rift Apart'))
 def test_steam_and_fallback(self):
  image=io.BytesIO();picture=Image.new('RGB',(20,30),(20,60,80));picture.paste((220,180,100),(0,0,10,15));picture.save(image,format='PNG')
  def fetch(url,limit):
   if 'storesearch' in url:return json.dumps({'items':[{'id':1,'name':'Demo II'},{'id':2,'name':'Demo III'}]}).encode()
   if '/apps/2/' in url:return image.getvalue()
   raise AssertionError('Wrong sequel or unnecessary fallback')
  self.module.fetch=fetch
  self.assertEqual(len(self.module.cover('Demo 3')),115200)
 def test_temporary_failure_retried(self):
  self.module.steam_candidates=lambda _:iter([])
  def offline(_):raise OSError('offline')
  self.module.wikipedia_candidates=offline
  with self.assertRaises(RuntimeError):self.module.cover('Example')
  self.module.wikipedia_candidates=lambda _:iter([{'provider':'Wikipedia','image':'https://upload.wikimedia.org/example.png'}])
  self.module.raster=lambda _:b'x'*115200
  self.assertEqual(len(self.module.cover('Example')),115200)
 def test_persistent_alias(self):
  aliases=pathlib.Path(self.temp.name)/'aliases.json';aliases.write_text(json.dumps({'regionalname':'Canonical Game'}));self.module.ALIASES=aliases
  names=[]
  def candidate(title):names.append(title);yield {'provider':'Steam','image':'https://cdn.akamai.steamstatic.com/example.jpg'}
  self.module.steam_candidates=candidate;self.module.raster=lambda _:b'x'*115200
  self.assertEqual(len(self.module.cover('Regional Name')),115200);self.assertEqual(names,['Canonical Game'])
 def test_playstation_identity(self):
  sources=pathlib.Path(self.temp.name)/'sources.json';sources.write_text(json.dumps({'nhl27':'https://store.playstation.com/en-us/product/TEST/'}));self.module.SOURCES=sources
  self.module.fetch=lambda *_:b'<script type="application/ld+json">{"@type":"Product","name":"NHL&#174; 27 Standard Edition PS5","image":"https://image.api.playstation.com/test.png"}</script>'
  self.assertEqual(len(list(self.module.playstation_candidates('EA SPORTS™ NHL™ 27'))),1)
  self.assertEqual(list(self.module.playstation_candidates('NHL 26')),[])
  self.module.fetch=lambda *_:b'<script type="application/ld+json">{"@type":"Product","name":"NHL 26","image":"https://image.api.playstation.com/test.png"}</script>'
  self.assertEqual(list(self.module.playstation_candidates('NHL 27')),[])
 def test_origin_confinement(self):
  with self.assertRaises(ValueError):self.module.fetch('http://localhost/private',100)
 def test_sparta_release_uses_verified_playstation_source(self):
  self.module.SOURCES=pathlib.Path(__file__).with_name('sources.json')
  calls=[]
  def fetch(url,limit):
   calls.append(url)
   return b'<script type="application/ld+json">{"@type":"Product","name":"God of War Sons of Sparta","image":"https://image.api.playstation.com/sparta.png"}</script>'
  self.module.fetch=fetch
  title=self.module.clean_title('God of War Sons of Sparta PROPER PS5-PPSA28997[FPKG]')
  candidates=list(self.module.playstation_candidates(title))
  self.assertEqual(len(candidates),1)
  self.assertIn('UP9000-PPSA28997_00-SONSOFSPARTAPS50',calls[0])
  self.assertEqual(list(self.module.playstation_candidates('God of War Ghost of Sparta')),[])
 def test_blank_provider_image_falls_back(self):
  blank=io.BytesIO();Image.new('RGB',(20,30),(75,75,75)).save(blank,format='JPEG')
  picture=Image.new('RGB',(20,30),(20,60,80));picture.paste((220,180,100),(0,0,10,15));real=io.BytesIO();picture.save(real,format='PNG')
  self.module.playstation_candidates=lambda _:iter([])
  self.module.steam_candidates=lambda _:iter([{'provider':'Steam','image':'blank'}])
  self.module.wikipedia_candidates=lambda _:iter([{'provider':'Wikipedia','image':'real'}])
  self.module.fetch=lambda url,_:blank.getvalue() if url=='blank' else real.getvalue()
  data=self.module.cover('Battlefield 6');self.assertTrue(self.module.usable_rgb(data))
  metadata=list(pathlib.Path(self.temp.name).glob('*.json'))
  self.assertEqual(json.loads(metadata[0].read_text())['match']['provider'],'Wikipedia')
 def test_cached_grey_tile_is_replaced(self):
  import hashlib
  root=pathlib.Path(self.temp.name);ident=hashlib.sha256(b'v4:battlefield6').hexdigest()
  # Includes the small JPEG variation observed in the real stale cache.
  grey=bytes(73+i%9 for i in range(115200));(root/(ident+'.rgb')).write_bytes(grey)
  (root/(ident+'.json')).write_text(json.dumps({'resolver':6}))
  self.module.playstation_candidates=lambda _:iter([{'provider':'PlayStation','image':'real'}])
  expected=bytes([20,40,60,200,180,160])*19200
  self.module.raster=lambda _:expected
  self.assertEqual(self.module.cover('Battlefield 6'),expected)
  self.assertEqual(self.module.cover('Battlefield 6'),expected)
 def test_placeholder_with_isolated_noise_is_rejected_after_resize(self):
  picture=Image.new('RGB',(600,900),(75,75,75));picture.putpixel((300,450),(120,120,120))
  data=io.BytesIO();picture.save(data,format='PNG');self.module.fetch=lambda *_:data.getvalue()
  with self.assertRaisesRegex(ValueError,'Blank artwork'):self.module.raster({'image':'fixture'})
if __name__=='__main__':unittest.main()
