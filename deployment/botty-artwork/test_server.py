import concurrent.futures, importlib.util, io, json, os, pathlib, tempfile, threading, time, unittest
from unittest.mock import patch
from PIL import Image
class Covers(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();root=pathlib.Path(self.temp.name);(root/'key').write_text('test')
  os.environ['BOTTY_STEAMGRIDDB_KEY_FILE']=str(root/'sgdb-key');(root/'sgdb-key').write_text('a'*32)
  os.environ['BOTTY_PLAYSTATION_GAMES']=str(root/'ps-games.json')
  os.environ['BOTTY_ARTWORK_ALIASES']=str(root/'aliases.json');os.environ['BOTTY_STEAMGRIDDB_GAMES']=str(root/'games.json')
  os.environ['BOTTY_ARTWORK_KEY_FILE']=str(root/'key');os.environ['BOTTY_ARTWORK_CACHE']=str(root)
  spec=importlib.util.spec_from_file_location('artwork',pathlib.Path(__file__).with_name('server.py'));self.module=importlib.util.module_from_spec(spec);spec.loader.exec_module(self.module)
  self.steam_candidates=self.module.steam_candidates;self.module.steam_candidates=lambda *_:iter(())
 def tearDown(self):self.temp.cleanup()
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
 def test_release_suffix_order(self):
  """Clean stacked scene, DLC, and edition suffixes in either order."""
  for suffix in ['DLC ONLY Deluxe Edition', 'PROPER Deluxe Edition',
                 'PROPER Deluxe Edition REPACK DLC ONLY', 'Deluxe Edition PROPER Ballpark Edition']:
   with self.subTest(suffix=suffix):
    self.assertEqual(self.module.clean_title('Example Game '+suffix+' PS5-PPSA00000[FPKG]'), 'Example Game')
  self.assertEqual(self.module.clean_title('Example Game DLC ONLY Deluxe Edition'), 'Example Game')
 def test_canonical_scene_words_preserved(self):
  """Keep canonical scene words unless another release marker is present."""
  for word in ['Proper', 'Repack', 'Rerip', 'Readnfo', 'Internal']:
   with self.subTest(word=word):
    title='Example '+word
    self.assertEqual(self.module.clean_title(title),title)
    self.assertEqual(self.module.clean_title(title+' Deluxe Edition'),title)
    for marker in [' PS5', ' PPSA00000', ' CUSA12345', '[FPKG]',
                   ' [PS5]', ' [PPSA00000]', ' [CUSA12345]',
                   '-PPSA12345', '.CUSA12345', ':PPSA12345',
                   ' UPDATE', ' v1.02', ' v10.02', ' MULTI', ' MULTI5',
                   ' incl DLC', ' incl. DLC', ' including DLC']:
     with self.subTest(marker=marker):
      self.assertEqual(self.module.clean_title(title+marker),'Example')
 def test_scene_words_before_release_metadata(self):
  """Release metadata enables cleanup of preceding scene and edition suffixes."""
  for marker in ['UPDATE', 'v1.02', 'v10.02', 'MULTI5', 'incl DLC', 'including DLC']:
   with self.subTest(marker=marker):
    title='Ratchet and Clank Rift Apart'
    self.assertEqual(self.module.clean_title(title+' PROPER '+marker),title)
    self.assertEqual(self.module.clean_title(title+' PROPER Deluxe Edition REPACK '+marker),title)
 def test_platform_marker_boundaries(self):
  """Recognize bracketed and punctuated platform markers, not embedded words."""
  for title in ['Example Proper [PPSA00000]', 'Example Internal [CUSA12345]',
                'Example Repack [PS5]', 'Example Proper-PPSA12345',
                'Example Internal-CUSA12345', 'Example Proper-[PPSA12345]',
                'Example Proper [ps5]', 'Example Proper-ppsa12345']:
   with self.subTest(title=title):
    self.assertEqual(self.module.clean_title(title),'Example')
  for title in ['Example Proper XPPSA12345', 'Example Proper PPSA12345X']:
   with self.subTest(title=title):
    self.assertEqual(self.module.clean_title(title),title)
  for title in ['Example Proper [CUSA12345X]', 'Example Proper [PS50]']:
   with self.subTest(title=title):
    self.assertEqual(self.module.clean_title(title),'Example Proper')
 def test_punctuation_joined_scene_suffixes(self):
  """Strip punctuated scene suffixes only when release metadata is present."""
  for separator in ['-', '.', ':']:
   for word in ['PROPER', 'REPACK', 'RERIP', 'READNFO', 'INTERNAL']:
    with self.subTest(separator=separator,word=word):
     title='Some Game'+separator+word
     self.assertEqual(self.module.clean_title(title+separator+'PPSA12345'),'Some Game')
     self.assertEqual(self.module.clean_title(title+' [CUSA12345]'),'Some Game')
     self.assertEqual(self.module.normalize(self.module.clean_title(title)),self.module.normalize(title))
  self.assertEqual(self.module.clean_title('Some Game-PROPER:REPACK-PPSA12345'),'Some Game')
  self.assertEqual(self.module.clean_title('Some Game Deluxe Edition-PROPER-PPSA12345'),'Some Game')
 def provider(self, missing_hero=False):
  calls=[]
  def fetch(url,limit,headers=None):
   calls.append((url,headers))
   if '/api/v2/' in url:
    self.assertEqual(headers,{'Authorization':'Bearer '+'a'*32})
    if '/search/' in url:rows=[{'id':42,'name':'Demo II','release_date':0},{'id':43,'name':'Demo III','release_date':0}]
    else:
     hero='/heroes/' in url
     rows=[] if hero and missing_hero else [{'id':99,'width':1920 if hero else 600,'height':620 if hero else 900,'url':'https://cdn2.steamgriddb.com/'+('hero' if hero else 'cover')+'.png','score':5}]
    return json.dumps({'success':True,'data':rows}).encode()
   size=(1920,620) if 'hero' in url else (600,900)
   image=Image.new('RGB',size,(20,60,80));image.paste((220,180,100),(0,0,size[0]//2,size[1]//2))
   out=io.BytesIO();image.save(out,format='PNG');return out.getvalue()
  self.module.fetch=fetch
  return calls
 def test_portrait_hero_and_persistent_cache(self):
  calls=self.provider()
  cover=self.module.cover('Demo III PS5-PPSA12345');hero=self.module.cover('Demo III Switch NSP','hero')
  self.assertEqual(len(cover),160*240*3);self.assertEqual(len(hero),640*360*3)
  self.assertEqual(len([url for url,_ in calls if '/search/' in url]),1)
  self.assertEqual(self.module.cover('Demo III','hero'),hero)
  count=len(calls);self.assertEqual(self.module.cover('Demo III'),cover);self.assertEqual(len(calls),count)
  self.assertTrue(any('dimensions=600x900' in url for url,_ in calls))
  metadata=''.join(p.read_text() for p in pathlib.Path(self.temp.name).glob('*.json'))
  self.assertNotIn('a'*32,metadata);self.assertIn('SteamGridDB',metadata)
 def test_missing_hero_does_not_poison_cover(self):
  calls=self.provider(missing_hero=True)
  self.assertEqual(self.module.cover('Demo III','hero'),b'')
  self.assertEqual(len(self.module.cover('Demo III')),115200)
  count=len(calls);self.assertEqual(self.module.cover('Demo III','hero'),b'');self.assertEqual(len(calls),count)
 def test_missing_key_and_outage_are_retried(self):
  self.module.STEAMGRIDDB_KEY.unlink()
  with self.assertRaises(RuntimeError):self.module.cover('Demo III')
  self.assertFalse(list(pathlib.Path(self.temp.name).glob('*.rgb')))
  self.module.STEAMGRIDDB_KEY.write_text('a'*32)
  self.module.fetch=lambda *_:(_ for _ in ()).throw(OSError('offline'))
  with self.assertRaises(RuntimeError):self.module.cover('Demo III')
  self.provider();self.assertEqual(len(self.module.cover('Demo III')),115200)
 def test_never_substitutes_sequel_or_ambiguous_remake(self):
  self.module.sgdb=lambda _: [{'id':1,'name':'Demo II'},{'id':2,'name':'Demo III'},{'id':3,'name':'Demo III'}]
  self.assertEqual(self.module.cover('Demo IV'),b'');self.assertEqual(self.module.cover('Demo III'),b'')
 def test_year_and_manual_game_id(self):
  self.module.sgdb=lambda _: [{'id':1,'name':'Demo','release_date':1104537600},{'id':2,'name':'Demo','release_date':1672531200}]
  path=pathlib.Path(self.temp.name)/'match.json'
  self.assertEqual(self.module.game('Demo (2023 video game)',path),2)
  self.module.GAMES.write_text(json.dumps({'demo':123}))
  self.assertEqual(self.module.game('Demo',path),123)
 def test_aliases_and_switch_release_names(self):
  self.assertEqual(self.module.clean_title('Demo III [0100000000000000] Switch NSP'), 'Demo III')
  self.assertEqual(self.module.clean_title('1-2-Switch'), '1-2-Switch')
  self.assertEqual(self.module.clean_title('Nintendo Switch Sports'), 'Nintendo Switch Sports')
  self.assertEqual(self.module.clean_title('Castlevania.Belmonts.Curse.DLC.Unlocker.PS5'), 'Castlevania Belmonts Curse')
  self.assertEqual(self.module.clean_title('Castlevania Belmonts Curse DLC Unlocker'), 'Castlevania Belmonts Curse')
  self.module.ALIASES.write_text(json.dumps({'regionalname':'Demo III'}));self.provider()
  self.assertEqual(len(self.module.cover('Regional Name')),115200)
 def test_real_prowlarr_platform_suffixes(self):
  cases = {
   'My Cozy Aquarium NSW-SUXXORS': 'My Cozy Aquarium',
   'Mario Kart 8 Deluxe (RF) PROPER NSW Switch': 'Mario Kart 8 Deluxe',
   'Yooka Laylee (eShop)(EU) NSW Switch': 'Yooka Laylee',
   'Bayonetta 2 NSW Switch': 'Bayonetta 2',
   'Dungeons 4 Nintendo Switch Edition Update v1 11 NSW-SUXXORS': 'Dungeons 4',
   'Hello Kitty Island Adventure Base Game + Update v1 0 NSW': 'Hello Kitty Island Adventure',
   'Castlevania Belmonts Curse Digital Sound and Art Gallery PS5-DUPLEX': 'Castlevania Belmonts Curse',
   'I Am Your Beast NSW-VENOM': 'I Am Your Beast',
   'Urban Jungle NSW-BREWS': 'Urban Jungle',
   'Football Cup 2024 NSW-SUXXORS': 'Football Cup 2024',
   'Castlevania Dominus Collection NSW-VENOM': 'Castlevania Dominus Collection',
   'Vertigo 2 VR2 PS5-DUPLEX': 'Vertigo 2',
   'BEYBLADE X EVOBATTLE v1 1 3 DLC Unlocker NSW-SUXXORS': 'BEYBLADE X EVOBATTLE',
  }
  for raw, expected in cases.items():
   with self.subTest(raw=raw): self.assertEqual(self.module.clean_title(raw), expected)
  for canonical in ['Nintendo Switch Sports', '1-2-Switch', 'Everybody 1-2-Switch!', 'VR2']:
   self.assertEqual(self.module.clean_title(canonical), canonical)
 def test_real_prowlarr_matching(self):
  cases = [
   ('Cyberpunk 2077', [{'id':5209422,'name':'Cyberpunk 2077','release_date':1607558400}], 5209422),
   ('Football Cup 2024', [{'id':42,'name':'Football Cup 2024','release_date':1672531200}], 42),
   ('Divinity Original Sin 2', [{'id':10425,'name':'Divinity: Original Sin 2'}, {'id':5264646,'name':'Divinity: Original Sin 2 - Definitive Edition'}],10425),
   ('Hollow Knight Silksong',[{'id':1043,'name':'Hollow Knight: Silksong'}, {'id':5512976,'name':'Hollow Knight Silksong [Gameboy Demake]'}],1043),
   ('Ratchet Clank Rift Apart',[{'id':5266059,'name':'Ratchet & Clank: Rift Apart'}],5266059),
   ('Tony Hawks Pro Skater 3 4',[{'id':5480866,'name':"Tony Hawk's Pro Skater 3 + 4"}],5480866),
   ('KINGDOM HEARTS HD 1 5 2 5 ReMIX',[{'id':5250081,'name':'Kingdom Hearts HD 1.5 + 2.5 ReMIX'}],5250081),
   ('The Relic First Guardian',[{'id':5450064,'name':'The Relic: The First Guardian'}],5450064),
  ]
  for title, rows, expected in cases:
   with self.subTest(title=title):
    self.module.sgdb=lambda _, rows=rows:rows
    self.assertEqual(self.module.game(title,pathlib.Path(self.temp.name)/str(expected)),expected)
  self.module.sgdb=lambda _: [{'id':1,'name':'Demo II'}, {'id':2,'name':'Demo III'}]
  self.assertIsNone(self.module.game('Demo IV',pathlib.Path(self.temp.name)/'missing'))
 def test_query_joiners_retry_without_substituting_a_sequel(self):
  calls=[]
  def search(path):
   calls.append(path)
   return [{'id':2,'name':'Ratchet & Clank: Rift Apart'}] if 'Ratchet%20Clank%20Rift' in path else [{'id':1,'name':'Ratchet & Clank'}]
  self.module.sgdb=search
  self.assertEqual(self.module.game('Ratchet and Clank Rift Apart',pathlib.Path(self.temp.name)/'game'),2)
  self.assertEqual(len(calls),2)
 def test_scoped_remake_alias_and_explicit_year(self):
  self.module.ALIASES.write_text(json.dumps({'ps5:residentevil4':'Resident Evil 4 (2023)'}))
  self.assertEqual(self.module.canonical_title('Resident Evil 4 PS5-DUPLEX'),'Resident Evil 4 (2023)')
  self.assertEqual(self.module.canonical_title('Resident Evil 4 NSW-VENOM'),'Resident Evil 4')
  self.module.sgdb=lambda _: [{'id':3168,'name':'Resident Evil 4','release_date':1105488000}, {'id':5332120,'name':'Resident Evil 4','release_date':1679616000}]
  self.assertEqual(self.module.game('Resident Evil 4 (2023)',pathlib.Path(self.temp.name)/'game'),5332120)
  self.assertIsNone(self.module.game('Resident Evil 4',pathlib.Path(self.temp.name)/'ambiguous'))
 def test_alternate_portraits_are_uniform_and_prefer_standard(self):
  self.module.sgdb=lambda _: [
   {'id':1,'width':660,'height':930,'score':100,'url':'https://cdn2.steamgriddb.com/one.png'},
   {'id':2,'width':600,'height':900,'score':1,'url':'https://cdn2.steamgriddb.com/two.png'},
   {'id':3,'width':342,'height':482,'score':10,'url':'https://cdn2.steamgriddb.com/three.png'},
   {'id':4,'width':1024,'height':1024,'score':100,'url':'https://cdn2.steamgriddb.com/square.png'},
  ]
  self.assertEqual([c['id'] for c in self.module.candidates(42,'cover')],[2,1,3])
  for size in [(660,930),(342,482)]:
   image=Image.new('RGB',size,(10,20,40));image.paste((200,30,10),(0,0,size[0]//2,size[1]))
   source=io.BytesIO();image.save(source,format='PNG');self.module.fetch=lambda *_:source.getvalue()
   self.assertEqual(len(self.module.raster({'image':'fixture','width':size[0],'height':size[1]})),115200)
 def test_additional_actual_release_suffixes(self):
  for raw,expected in [
   ('7 Days to Die Console Edition PS5-PPSA16645[FPKG]','7 Days to Die'),
   ('Castlevania Belmonts Curse Midnight Edition NSW-VENOM','Castlevania Belmonts Curse'),
   ('Castlevania Belmonts Curse DEMO NSW-VENOM','Castlevania Belmonts Curse'),
   ('Frostpunk All Expansions Pack NSW-SUXXORS','Frostpunk'),
   ('METAL GEAR SOLID 4 Guns of the Patriots Master Collection Version Incl DLC PS5-DUPLEX','METAL GEAR SOLID 4 Guns of the Patriots'),
  ]:
   with self.subTest(raw=raw): self.assertEqual(self.module.clean_title(raw),expected)
  self.assertEqual(self.module.match_key('KINGDOM HEARTS HD 1 5 plus 2 5 ReMIX'),self.module.match_key('Kingdom Hearts HD 1.5 + 2.5 ReMIX'))
 def test_collection_packs_do_not_match_an_arbitrary_game(self):
  for raw in ['Switch Library Update Pack 270', 'Nintendo MIG Switch Game Collection / Vol 1', 'Nintendo Switch Game Library July 2019 Vol 1', 'Top 70 Switch Games of all time']:
   with self.subTest(raw=raw): self.assertEqual(self.module.cover(raw),b'')
  for raw in ['Castlevania Dominus Collection', 'Metal Gear Solid Master Collection Vol. 1', 'Nintendo Switch Sports']:
   self.assertFalse(self.module.is_collection(raw))
 def test_search_tail_still_requires_the_complete_title(self):
  self.module.sgdb=lambda path: [{'id':1,'name':'Other Game: Special Subtitle Name'}, {'id':2,'name':'Some Game with Special Subtitle Name'}] if path.endswith('Special%20Subtitle%20Name') else []
  self.assertEqual(self.module.game('Some Game with Special Subtitle Name',pathlib.Path(self.temp.name)/'game'),2)
 def steam_provider(self, title='EA SPORTS FC 27', appid=4080220, returned_name=None, app_type=0):
  self.module.steam_candidates=self.steam_candidates;self.module.sgdb=lambda _:[]
  calls=[];assets={'asset_url_format':'steam/apps/'+str(appid)+'/${FILENAME}?t=123',
   'library_capsule_2x':'a'*40+'/library_capsule_2x.jpg',
   'library_capsule':'b'*40+'/library_capsule.jpg','library_hero':'c'*40+'/library_hero.jpg'}
  def fetch(url,limit,headers=None):
   calls.append((url,headers));self.assertIsNone(headers)
   if '/storesearch/' in url:
    return json.dumps({'items':[{'id':appid,'name':title,'type':'app'},{'id':99,'name':'EA SPORTS FC 26','type':'app'}]}).encode()
   if '/GetItems/' in url:
    return json.dumps({'response':{'store_items':[{'appid':appid,'name':returned_name or title,'type':app_type,'success':1,'assets':assets,'release':{'steam_release_date':1790290781}}]}}).encode()
   size=(1920,620) if 'library_hero' in url else (600,900)
   im=Image.new('RGB',size,(10,20,40));im.paste((240,180,20),(0,0,size[0]//2,size[1]))
   out=io.BytesIO();im.save(out,format='PNG');return out.getvalue()
  self.module.fetch=fetch
  return calls,assets
 def test_steam_fallback_covers_and_heroes_with_modern_asset_hashes(self):
  for title,appid in [('EA SPORTS FC 27',4080220),('BLUE REFLECTION Quartet',3825390)]:
   with self.subTest(title=title):
    calls,_=self.steam_provider(title,appid)
    self.assertEqual(len(self.module.cover(title)),115200)
    self.assertEqual(len(self.module.cover(title,'hero')),691200)
    self.assertEqual(sum('/storesearch/' in u for u,_ in calls),1)
    self.assertTrue(any('/'+str(appid)+'/'+'a'*40+'/library_capsule_2x.jpg' in u for u,_ in calls))
    count=len(calls);self.assertEqual(len(self.module.cover(title)),115200);self.assertEqual(len(calls),count)
    metadata=[json.loads(p.read_text()) for p in pathlib.Path(self.temp.name).glob('*.cover.json')]
    self.assertTrue(any(p['match']['provider']=='Steam' and p['match']['game']==appid for p in metadata))
 def test_steam_does_not_substitute_another_year_or_dlc(self):
  self.steam_provider(returned_name='EA SPORTS FC 26')
  self.assertEqual(self.module.cover('EA SPORTS FC 27'),b'')
  self.steam_provider(title='BLUE REFLECTION Quartet',appid=3825390,app_type=1)
  self.assertEqual(self.module.cover('BLUE REFLECTION Quartet'),b'')
 def test_steam_fallback_does_not_override_steamgriddb(self):
  self.provider()
  self.module.steam_candidates=lambda *_:self.fail('SteamGridDB must stay first')
  self.assertEqual(len(self.module.cover('Demo III')),115200)
 def test_invalid_steam_assets_and_square_images_are_rejected(self):
  _,assets=self.steam_provider()
  assets['library_capsule_2x']='../private.jpg'
  with self.assertRaisesRegex(ValueError,'asset path'):list(self.module.steam_candidates('EA SPORTS FC 27','cover'))
  for url in ['https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/123/header.jpg',
              'https://api.steampowered.com/other-api',
              'https://shared.akamai.steamstatic.com.evil.test/store_item_assets/steam/apps/123/library_capsule.jpg']:
   self.assertFalse(self.module.allowed(url))
  im=Image.new('RGB',(1024,1024),(10,20,40));out=io.BytesIO();im.save(out,format='PNG')
  self.module.fetch=lambda *_:out.getvalue()
  with self.assertRaisesRegex(ValueError,'Unexpected artwork format'):self.module.raster({'provider':'Steam','image':'fixture'})
 def test_steam_outage_does_not_become_a_confirmed_miss(self):
  self.steam_provider();self.module.fetch=lambda *_:(_ for _ in ()).throw(OSError('offline'))
  with self.assertRaises(RuntimeError):self.module.cover('EA SPORTS FC 27')
  self.assertFalse(list(pathlib.Path(self.temp.name).glob('*.rgb')))
  self.steam_provider();self.assertEqual(len(self.module.cover('EA SPORTS FC 27')),115200)
 def test_legacy_steam_library_names_and_missing_double_resolution(self):
  _,assets=self.steam_provider(title='Afterdream',appid=1995880)
  assets.update(library_capsule_2x='library_600x900_2x.jpg',library_capsule='library_600x900.jpg')
  fetch=self.module.fetch;missing=[]
  def request(url,*args):
   if 'library_600x900_2x.jpg' in url:
    missing.append(url);raise self.module.urllib.error.HTTPError(url,404,'missing',{},None)
   return fetch(url,*args)
  self.module.fetch=request
  self.assertEqual(len(self.module.cover('Afterdream')),115200)
  self.assertEqual(len(missing),1)
  image=Image.new('RGB',(1200,1800),(10,20,40));image.paste((200,30,10),(0,0,600,1800))
  source=io.BytesIO();image.save(source,format='PNG');self.module.fetch=lambda *_:source.getvalue()
  self.assertEqual(len(self.module.raster({'provider':'Steam','image':'fixture'})),115200)
 def playstation_provider(self, name='NHL® 27 Standard Edition PS5', product_id='UP0006-PPSA34063_00-NHL27STANDEDITON'):
  self.module.sgdb=lambda _:[]
  url='https://store.playstation.com/en-us/product/'+product_id+'/'
  self.module.PLAYSTATION_GAMES.write_text(json.dumps({'nhl27':url,'dragonsdogma2darkarisen':url}))
  image='https://image.api.playstation.com/vulcan/ap/rnd/202610/0000/'
  product={'id':product_id,'name':name,'media':[
   {'role':'MASTER','type':'IMAGE','url':image+'square.png'},
   {'role':'GAMEHUB_COVER_ART','type':'IMAGE','url':image+'hero.png'},
   {'role':'PORTRAIT_BANNER','type':'IMAGE','url':image+'portrait.png'},
  ]}
  envelope={'args':{'productId':product_id},'cache':{'Product:'+product_id:product,
   'Concept:other':{'media':[{'role':'PORTRAIT_BANNER','type':'IMAGE','url':image+'wrong-game.png'}]}}}
  ld={'@type':'Product','name':name,'sku':product_id}
  calls=[]
  def fetch(target,limit,headers=None):
   calls.append(target);self.assertIsNone(headers)
   if target==url:
    text='<script type="application/json">'+json.dumps(envelope)+'</script>'
    nxt={'props':{'pageProps':{'batarangs':{'background-image':{'text':text}}}}}
    return ('<script type="application/ld+json">'+json.dumps(ld)+'</script><script id="__NEXT_DATA__" type="application/json">'+json.dumps(nxt).replace('<',chr(92)+'u003c').replace('>',chr(92)+'u003e')+'</script>').encode()
   self.assertNotIn('wrong-game',target);self.assertNotIn('square',target)
   size=(720,1080) if 'portrait' in target else (1920,1080)
   im=Image.new('RGB',size,(10,20,40));im.paste((230,160,20),(0,0,size[0]//2,size[1]))
   out=io.BytesIO();im.save(out,format='PNG');return out.getvalue()
  self.module.fetch=fetch
  return calls,ld,product
 def test_playstation_verified_portrait_and_product_specific_background(self):
  for title,name,ident in [('NHL 27','NHL® 27 Standard Edition PS5','UP0006-PPSA34063_00-NHL27STANDEDITON'),
                           ('Dragons Dogma 2 Dark Arisen',"Dragon's Dogma 2: Dark Arisen",'UP0102-PPSA09663_00-DD2DA00000000000')]:
   calls,_,_=self.playstation_provider(name,ident)
   self.assertEqual(len(self.module.cover(title)),115200)
   self.assertEqual(len(self.module.cover(title,'hero')),691200)
   self.assertEqual(sum('store.playstation.com' in url for url in calls),1)
   count=len(calls);self.assertEqual(len(self.module.cover(title)),115200);self.assertEqual(count,len(calls))
   meta=[json.loads(p.read_text()) for p in pathlib.Path(self.temp.name).glob('*.cover.json')]
   self.assertTrue(any(x['match']['provider']=='PlayStation' and x['match']['game']==ident for x in meta))
 def test_playstation_does_not_accept_another_edition_or_untrusted_source(self):
  _,ld,product=self.playstation_provider()
  ld['sku']='wrong'
  with self.assertRaisesRegex(ValueError,'identity mismatch'):list(self.module.playstation_candidates('NHL 27','cover'))
  _,ld,product=self.playstation_provider(name="Dragon's Dogma 2")
  with self.assertRaisesRegex(ValueError,'identity mismatch'):list(self.module.playstation_candidates('Dragons Dogma 2 Dark Arisen','cover'))
  self.module.PLAYSTATION_GAMES.write_text(json.dumps({'nhl27':'https://evil.test/en-us/product/UP0006-PPSA34063_00-NHL27STANDEDITON/'}))
  with self.assertRaisesRegex(ValueError,'Invalid PlayStation product'):list(self.module.playstation_candidates('NHL 27','cover'))
  self.assertFalse(self.module.allowed('https://image.api.playstation.com.evil.test/vulcan/ap/rnd/202610/0000/image.png'))
 def test_playstation_remains_after_steam(self):
  self.steam_provider()
  self.module.playstation_candidates=lambda *_:self.fail('PlayStation must not replace an available Steam image')
  self.assertEqual(len(self.module.cover('EA SPORTS FC 27')),115200)
 def test_parenthesized_fan_game_is_not_the_base_game(self):
  self.module.sgdb=lambda _: [{'id':5512976,'name':'Hollow Knight Silksong (Gameboy Demake)'}]
  self.assertIsNone(self.module.game('Hollow Knight Silksong',pathlib.Path(self.temp.name)/'game'))
  self.assertNotEqual(self.module.match_key('Hollow Knight Silksong (Gameboy Demake)'),self.module.match_key('Hollow Knight Silksong'))
  self.assertEqual(self.module.normalize('Resident Evil 4 (2023 video game)'),self.module.normalize('Resident Evil 4'))
  self.steam_provider(title='Hollow Knight Silksong (Gameboy Demake)',appid=12)
  self.assertEqual(self.module.cover('Hollow Knight Silksong'),b'')
 def test_failed_saved_steam_url_refreshes_metadata(self):
  calls,assets=self.steam_provider()
  self.module.cover('EA SPORTS FC 27')
  image=next(pathlib.Path(self.temp.name).glob('*.cover.rgb'));os.utime(image,(0,0))
  assets['library_capsule_2x']='d'*40+'/library_capsule_2x.jpg'
  assets['library_capsule']='e'*40+'/library_capsule.jpg'
  fetch=self.module.fetch
  def request(url,*args):
   if '/'+('a'*40)+'/' in url or '/'+('b'*40)+'/' in url:
    raise self.module.urllib.error.HTTPError(url,404,'removed',{},None)
   return fetch(url,*args)
  self.module.fetch=request
  self.assertEqual(len(self.module.cover('EA SPORTS FC 27')),115200)
  self.assertEqual(sum('/GetItems/' in url for url,_ in calls),2)
  metadata=json.loads(next(pathlib.Path(self.temp.name).glob('*.cover.json')).read_text())
  self.assertIn('d'*40,metadata['match']['image'])
 def test_atomic_metadata_writes_use_unique_temporary_files(self):
  target=pathlib.Path(self.temp.name)/'concurrent.json';barrier=threading.Barrier(2)
  replace=pathlib.Path.replace;temporaries=[]
  def synchronized_replace(source,destination):
   temporaries.append(str(source));barrier.wait(timeout=5)
   return replace(source,destination)
  with patch.object(pathlib.Path,'replace',synchronized_replace):
   with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
    list(pool.map(lambda value:self.module.save_json(target,{'value':value}),[1,2]))
  self.assertEqual(len(set(temporaries)),2)
  self.assertIn(json.loads(target.read_text())['value'],[1,2])
  self.assertFalse(list(pathlib.Path(self.temp.name).glob('concurrent.json.*.tmp')))
 def test_cover_and_hero_coalesce_game_resolution(self):
  calls=self.provider();game=self.module.game;barrier=threading.Barrier(2);gate=threading.Lock()
  active=0;maximum=0
  def resolve(*args):
   nonlocal active,maximum
   with gate:active+=1;maximum=max(maximum,active)
   try:time.sleep(.01);return game(*args)
   finally:
    with gate:active-=1
  self.module.game=resolve
  def download(kind):barrier.wait(timeout=5);return self.module.cover('Demo III',kind)
  with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:images=list(pool.map(download,['cover','hero']))
  self.assertEqual([len(data) for data in images],[115200,691200])
  self.assertEqual(maximum,1)
  self.assertEqual(sum('/search/' in url for url,_ in calls),1)
 def test_untrusted_sources_and_redirects(self):
  for url in ['http://localhost/private','https://evil.test/image','https://cdn2.steamgriddb.com.evil.test/a','https://cdn2.steamgriddb.com:444/a']:
   self.assertFalse(self.module.allowed(url))
  req=self.module.urllib.request.Request('https://www.steamgriddb.com/api/v2/test',headers={'Authorization':'Bearer secret'})
  with self.assertRaises(ValueError):self.module.Redirects().redirect_request(req,None,302,'',{},'https://cdn2.steamgriddb.com/a')
 def test_reject_landscape_grid_and_blank_image(self):
  self.module.sgdb=lambda _: [{'id':1,'width':1920,'height':620,'url':'https://cdn2.steamgriddb.com/hero.png'}]
  self.assertEqual(list(self.module.candidates(42,'cover')),[])
  out=io.BytesIO();Image.new('RGB',(600,900),(75,75,75)).save(out,format='PNG')
  self.module.fetch=lambda *_:out.getvalue()
  with self.assertRaisesRegex(ValueError,'Blank artwork'):self.module.raster({'image':'fixture','width':600,'height':900})
 def test_corruption_and_old_provider_cache_are_not_reused(self):
  self.provider();self.module.cover('Demo III')
  image=next(pathlib.Path(self.temp.name).glob('*.cover.rgb'));image.write_bytes(b'broken')
  self.assertEqual(len(self.module.cover('Demo III')),115200)
  meta=next(pathlib.Path(self.temp.name).glob('*.cover.json'))
  data=json.loads(meta.read_text());data['resolver']=6;data['match']['provider']='Wikipedia';meta.write_text(json.dumps(data))
  image.write_bytes(bytes([75])*115200)
  self.assertTrue(self.module.usable_rgb(self.module.cover('Demo III')))
 def test_resolver_upgrade_retries_misses_and_keeps_good_images(self):
  self.provider();expected=self.module.cover('Demo III')
  meta=next(pathlib.Path(self.temp.name).glob('*.cover.json'))
  image=next(pathlib.Path(self.temp.name).glob('*.cover.rgb'))
  value=json.loads(meta.read_text());value['resolver']=7;meta.write_text(json.dumps(value))
  fetch=self.module.fetch
  self.module.fetch=lambda *_:self.fail('A valid persistent image must survive the matching upgrade')
  self.assertEqual(self.module.cover('Demo III'),expected)
  self.module.fetch=fetch
  value['match']=None;meta.write_text(json.dumps(value));image.write_bytes(b'')
  self.assertEqual(self.module.cover('Demo III'),expected)
 def test_hero_preserves_edge_subjects(self):
  image=Image.new('RGB',(1920,620),(240,220,20));image.paste((10,20,40),(0,0,300,620));image.paste((10,20,40),(1800,0,1920,620))
  source=io.BytesIO();image.save(source,format='PNG');self.module.fetch=lambda *_:source.getvalue()
  data=self.module.raster({'image':'fixture','width':1920,'height':620},'hero')
  rendered=Image.frombytes('RGB',(640,360),data)
  self.assertEqual(rendered.getpixel((10,180)),(10,20,40))
  self.assertEqual(rendered.getpixel((630,180)),(10,20,40))
 def test_hero_prefers_visible_composition(self):
  self.module.game=lambda *_:42
  self.module.candidates=lambda *_:iter([{'id':1},{'id':2}])
  flat=Image.new('RGB',(640,360),(240,220,20));flat.paste((10,20,40),(0,0,100,360))
  visible=Image.new('RGB',(640,360),(240,220,20));visible.paste((10,20,40),(500,0,640,360))
  self.module.raster=lambda candidate,kind: (flat if candidate['id']==1 else visible).tobytes()
  self.assertEqual(self.module.cover('Demo III','hero'),visible.tobytes())
if __name__=='__main__':unittest.main()
