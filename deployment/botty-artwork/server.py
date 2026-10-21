#!/usr/bin/env python3
"""Game cover resolver with bounded fetching, persistent SteamGridDB portraits and backgrounds."""
import html, hashlib, hmac, io, json, os, pathlib, re, threading, time, tempfile, unicodedata, urllib.error, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from PIL import Image, ImageOps, ImageFilter, ImageStat
CACHE = pathlib.Path(os.environ.get('BOTTY_ARTWORK_CACHE', '/var/cache/botty-artwork'))
KEY = pathlib.Path(os.environ.get('BOTTY_ARTWORK_KEY_FILE', '/opt/botty-artwork/api-key')).read_text().strip()
STEAMGRIDDB_KEY = pathlib.Path(os.environ.get('BOTTY_STEAMGRIDDB_KEY_FILE', '/opt/botty-artwork/steamgriddb-api-key'))
GAMES = pathlib.Path(os.environ.get('BOTTY_STEAMGRIDDB_GAMES', '/opt/botty-artwork/steamgriddb-games.json'))
PLAYSTATION_GAMES = pathlib.Path(os.environ.get('BOTTY_PLAYSTATION_GAMES', '/opt/botty-artwork/playstation-games.json'))
ALIASES = pathlib.Path(os.environ.get('BOTTY_ARTWORK_ALIASES', '/opt/botty-artwork/aliases.json'))
UA = 'Botty/0.3 (personal game library; https://github.com/Portablelle/Botty-Plus)'
HOSTS = {'www.steamgriddb.com', 'cdn2.steamgriddb.com', 'cdn.steamgriddb.com'}
STEAM_CDNS = {'shared.akamai.steamstatic.com', 'shared.fastly.steamstatic.com'}
SIZES = {'cover': (160, 240), 'hero': (640, 360)}
RESOLVER = 12
LOCKS = [threading.Lock() for _ in range(32)]
NETWORK = threading.BoundedSemaphore(4)
Image.MAX_IMAGE_PIXELS = 12000000

def allowed(url):
    p = urllib.parse.urlsplit(url)
    if p.scheme != 'https' or p.username or p.port not in (None, 443): return False
    if p.hostname in HOSTS: return True
    if p.hostname == 'store.playstation.com': return bool(re.fullmatch(r'/[a-z]{2}-[a-z]{2}/product/[A-Z0-9_-]+/?', p.path))
    if p.hostname == 'image.api.playstation.com': return bool(re.fullmatch(r'/vulcan/ap/rnd/\d{6}/\d{4}/[A-Za-z0-9_-]+\.(?:png|jpg|jpeg)', p.path))
    if p.hostname == 'store.steampowered.com': return p.path == '/api/storesearch/'
    if p.hostname == 'api.steampowered.com': return p.path == '/IStoreBrowseService/GetItems/v1/'
    if p.hostname in STEAM_CDNS:
        return bool(re.fullmatch(r'/store_item_assets/steam/apps/\d+/(?:[0-9a-f]{40}/)?library_(?:capsule(?:_2x)?|600x900(?:_2x)?|hero(?:_2x)?)\.(?:jpg|png)', p.path))
    return False

class Redirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not allowed(newurl) or (req.has_header('Authorization') and urllib.parse.urlsplit(newurl).hostname != 'www.steamgriddb.com'):
            raise ValueError('Untrusted artwork redirect')
        return super().redirect_request(req, fp, code, msg, headers, newurl)

def fetch(url, limit, headers=None):
    if not allowed(url): raise ValueError('Untrusted artwork source')
    with NETWORK, urllib.request.build_opener(Redirects).open(urllib.request.Request(url, headers={'User-Agent': UA, **(headers or {})}), timeout=4) as response:
        data = response.read(limit + 1)
        if len(data) > limit: raise ValueError('Artwork too large')
        return data

def clean_title(title):
    """Remove release metadata while preserving scene words in unmarked names."""
    title = title.replace('_', ' ')
    title = re.sub(r'\((?:RF|USA?|EUR?|JP[N]?|ASIA|US-EU|eShop)\)', ' ', title, flags=re.I)
    title = re.sub(r'(?<=[A-Za-z])\.(?=[A-Za-z])', ' ', title)
    platform_marker = r'\b(?:PS5|(?:CUSA|PPSA)\d+)\b|\s+(?:NSP|XCI)\b|\s+(?:Switch|NSW)(?=\s*(?:$|[-–—]|NSP\b|XCI\b|Switch\b|NSW\b|v\d|\[))'
    platform_release = bool(re.search(platform_marker + r'|\[FPKG\]', title, flags=re.I))
    title = re.sub(r'\[[^\]]*\]', ' ', title)
    release_marker = re.compile(platform_marker + r'|\s+(?:incl\.?|including|v\d+(?:\.\d+)*|update|MULTI\d*)\b', re.I)
    release_name = platform_release or bool(release_marker.search(title))
    title = release_marker.split(title, maxsplit=1)[0].rstrip(' .-:')
    # Alternate metadata and edition removal until stable, regardless of order.
    # Scene words are ambiguous in canonical names; require a release marker.
    while True:
        trimmed = re.sub(r'\s+(?:DLC|add[ -]?on)(?:\s+add[ -]?on)?(?:\s+(?:only|unlocker|pack|fix))?\s*$', '', title, flags=re.I)
        if release_name:
            trimmed = re.sub(r'[\s.:-]+(?:proper|repack|rerip|readnfo|internal)[\s.:-]*$', '', trimmed, flags=re.I)
        trimmed = re.sub(r'\s+(?:Nintendo Switch Edition|Master Collection Version|All Expansions Pack|Base Game(?:\s*\+)?|Digital Sound and Art Gallery)\s*$', '', trimmed, flags=re.I)
        trimmed = re.sub(r'\s+(?:eShop|JPN|CHT|USA|EUR|DEMO)\s*$', '', trimmed, flags=re.I) if release_name else trimmed
        trimmed = re.sub(r'\s+(?:PSVR2?|VR2)\s*$', '', trimmed, flags=re.I) if release_name else trimmed
        trimmed = re.sub(r'\s+(?:the shores|(?:standard|digital deluxe|deluxe|ultimate|complete|gold|precious|infernal|midnight|legacy|console|anniversary|definitive|premium|special|ballpark|collectors?|game of the year) edition|directors? ?cut|goty)\s*$', '', trimmed, flags=re.I)
        if trimmed == title: break
        title = trimmed
    return re.sub(r'\s+', ' ', title).strip(' .-')

EXPLICIT_YEAR = r'\(((?:19|20)\d{2})(?:\s+video game)?\)'

def without_release_year(title):
    return re.sub(r'\s*' + EXPLICIT_YEAR, '', title, flags=re.I)

def normalize(title):
    title = without_release_year(title).casefold().replace('&', ' and ').replace('™', '').replace('®', '').replace('+', ' and ')
    title = re.sub(r'^ea\s+sports\s+', '', title)
    title = ''.join(c for c in unicodedata.normalize('NFKD', title) if not unicodedata.combining(c))
    roman = {'ii':'2', 'iii':'3', 'iv':'4', 'v':'5', 'vi':'6', 'vii':'7', 'viii':'8', 'ix':'9', 'x':'10'}
    title = re.sub(r'\b(?:ii|iii|iv|v|vi|vii|viii|ix|x)\b', lambda m: roman[m.group()], title)
    return re.sub(r'[^\w]+', '', title)

def match_key(title):
    # Scene names commonly omit conjunctions and articles. Keep every number,
    # subtitle and edition word; a partial title/sequel is never sufficient.
    title = re.sub(r'(?<=\d)\s+plus\s+(?=\d)', ' ', title, flags=re.I)
    return normalize(re.sub(r'\b(?:and|the)\b|[&+]', ' ', title, flags=re.I))

def canonical_title(raw):
    title = clean_title(raw)
    try:
        aliases = json.loads(ALIASES.read_text())
        scoped = 'ps5:' + normalize(title) if re.search(r'\b(?:PS5|PPSA\d+)\b', raw, re.I) else ''
        title = aliases.get(scoped, aliases.get(normalize(title), title))
    except (OSError, ValueError): pass
    return title

def is_collection(title):
    # These Prowlarr releases contain many unrelated games, not one title.
    return bool(re.match(r'^(?:(?:Nintendo\s+)?(?:MIG\s+)?Switch(?:\s+MIG)?\s+(?:Game\s+)?(?:Library|Collection)\b|Top\s+\d+\s+Switch\s+Games\b|XBOX\s*360\s+Peer Pressure\b)', title, re.I))

def sgdb(path):
    try: key = STEAMGRIDDB_KEY.read_text().strip()
    except OSError: raise RuntimeError('SteamGridDB API key is not configured') from None
    if not re.fullmatch(r'[A-Za-z0-9_-]{16,128}', key):
        raise RuntimeError('SteamGridDB API key is not configured')
    data = json.loads(fetch('https://www.steamgriddb.com/api/v2/' + path, 1048576,
                            {'Authorization': 'Bearer ' + key}))
    if not data.get('success') or not isinstance(data.get('data'), list):
        raise RuntimeError('SteamGridDB request failed')
    return data['data']


def game(title, cache):
    # Overrides disambiguate remakes/editions without accepting arbitrary URLs.
    try: override = json.loads(GAMES.read_text()).get(normalize(title))
    except (OSError, ValueError): override = None
    if override is not None:
        if type(override) is not int or override <= 0: raise ValueError('Invalid SteamGridDB game ID')
        return override
    try:
        previous = json.loads(cache.read_text())
        if previous.get('title') == title and time.time() - previous['saved'] < 2592000:
            return previous['id']
    except (OSError, ValueError, KeyError, TypeError): pass
    # Only an explicit parenthesized year disambiguates releases. "2077" in
    # Cyberpunk and "2024" in Football Cup are parts of their actual names.
    year = re.search(EXPLICIT_YEAR, title, re.I)
    query = without_release_year(title)
    queries = [query]
    without_joiners = re.sub(r'\b(?:and|the)\b|[&+]', ' ', query, flags=re.I)
    without_joiners = re.sub(r'\s+', ' ', without_joiners).strip()
    if without_joiners != query: queries.append(without_joiners)
    words = without_joiners.split()
    if len(words) > 4:
        tail = ' '.join(words[-3:])
        if tail not in queries: queries.append(tail)
    matches = []
    for search in queries:
        rows = [item for item in sgdb('search/autocomplete/' + urllib.parse.quote(search, safe=''))
                if type(item.get('id')) is int and item['id'] > 0]
        if year:
            rows = [item for item in rows if time.gmtime(item.get('release_date') or 0).tm_year == int(year[1])]
        # SGDB names are canonical, not scene filenames. Cleaning them merged
        # originals with editions, DLC and even bracketed fan demakes.
        matches = [item for item in rows if normalize(item.get('name', '')) == normalize(title)]
        if not matches:
            matches = [item for item in rows if match_key(item.get('name', '')) == match_key(title)]
        if matches: break
    ids = {item['id'] for item in matches}
    if len(ids) != 1: return None # Never silently substitute another sequel/remake.
    ident = ids.pop()
    save_json(cache, {'title': title, 'id': ident, 'saved': time.time()})
    return ident


def candidates(ident, kind):
    dimensions = '600x900,660x930,342x482' if kind == 'cover' else '1920x620,3840x1240,1600x650'
    query = urllib.parse.urlencode({'dimensions': dimensions, 'types': 'static',
                                   'mimes': 'image/png,image/jpeg,image/webp', 'nsfw': 'false', 'humor': 'false'})
    rows = sgdb(('grids' if kind == 'cover' else 'heroes') + '/game/' + str(ident) + '?' + query)
    for item in sorted(rows, key=lambda row: (kind != 'cover' or (row.get('width'), row.get('height')) == (600, 900), row.get('score', 0)), reverse=True):
        if item.get('nsfw') or item.get('humor') or item.get('epilepsy') or item.get('animated'): continue
        size = (item.get('width'), item.get('height'))
        if size not in ([(600, 900), (660, 930), (342, 482)] if kind == 'cover' else [(1920, 620), (3840, 1240), (1600, 650)]): continue
        if not allowed(item.get('url', '')) or urllib.parse.urlsplit(item['url']).hostname == 'www.steamgriddb.com': continue
        yield {'provider': 'SteamGridDB', 'game': ident, 'id': item['id'], 'image': item['url'],
               'width': size[0], 'height': size[1], 'kind': kind}


def steam_candidates(title, kind, refresh=False):
    """Use the publisher's library assets, never a cropped store banner."""
    cache = CACHE / (hashlib.sha256(('steam-game-v1:' + title.casefold()).encode()).hexdigest() + '.steam.json')
    fields = ('library_capsule_2x', 'library_capsule') if kind == 'cover' else ('library_hero', 'library_hero_2x')
    item = None
    try:
        previous = json.loads(cache.read_text())
        assets = previous['item'].get('assets', {})
        ttl = 2592000 if any(assets.get(field) for field in fields) else 3600
        if not refresh and previous.get('title') == title and time.time() - previous['saved'] < ttl:
            item = previous['item']
    except (OSError, ValueError, KeyError, TypeError): pass
    if item is None:
        query = without_release_year(title)
        url = 'https://store.steampowered.com/api/storesearch/?' + urllib.parse.urlencode({'term': query, 'l': 'english', 'cc': 'fr'})
        rows = json.loads(fetch(url, 1048576)).get('items', [])
        rows = [row for row in rows if row.get('type') == 'app' and type(row.get('id')) is int and row['id'] > 0]
        matches = [row for row in rows if normalize(row.get('name', '')) == normalize(title)]
        if not matches: matches = [row for row in rows if match_key(row.get('name', '')) == match_key(title)]
        ids = sorted({row['id'] for row in matches})
        if not ids or len(ids) > 10: return
        request = {'ids': [{'appid': ident} for ident in ids],
                   'context': {'language': 'english', 'country_code': 'FR', 'steam_realm': 1},
                   'data_request': {'include_assets': True, 'include_release': True}}
        url = 'https://api.steampowered.com/IStoreBrowseService/GetItems/v1/?' + urllib.parse.urlencode({'input_json': json.dumps(request, separators=(',', ':'))})
        rows = json.loads(fetch(url, 1048576)).get('response', {}).get('store_items', [])
        year = re.search(EXPLICIT_YEAR, title, re.I)
        # GetItems exposes item_type (0 = app) separately from app subtype
        # in type (0 = game, 1 = demo, 4 = DLC), not an app_type field.
        matches = [row for row in rows if row.get('success') == 1
                   and row.get('item_type', 0) == 0 and row.get('type') == 0
                   and row.get('appid') in ids and match_key(row.get('name', '')) == match_key(title)
                   and (not year or time.gmtime(row.get('release', {}).get('steam_release_date') or 0).tm_year == int(year[1]))]
        if len(matches) != 1: return
        item = {key: matches[0][key] for key in ('appid', 'name')}
        item['assets'] = matches[0].get('assets') or {}
        save_json(cache, {'title': title, 'item': item, 'saved': time.time()})
    ident = item['appid']; assets = item['assets']
    template = assets.get('asset_url_format', '')
    if not template: return
    # Modern assets have a different hash directory for each library image.
    if not re.fullmatch(r'steam/apps/' + str(ident) + r'/\$\{FILENAME\}(?:\?t=\d+)?', template):
        raise ValueError('Unexpected Steam asset template')
    seen = set()
    for field in fields:
        filename = assets.get(field, '')
        if not filename or filename in seen: continue
        seen.add(filename)
        url = 'https://shared.akamai.steamstatic.com/store_item_assets/' + template.replace('${FILENAME}', filename)
        if not allowed(url): raise ValueError('Unexpected Steam asset path')
        yield {'provider': 'Steam', 'game': ident, 'id': ident, 'image': url, 'kind': kind}



def playstation_candidates(title, kind):
    # Only operator-verified product identities, never a URL supplied by the UI.
    try: url = json.loads(PLAYSTATION_GAMES.read_text()).get(normalize(title))
    except (OSError, ValueError): return
    if not url: return
    if not allowed(url) or urllib.parse.urlsplit(url).hostname != 'store.playstation.com':
        raise ValueError('Invalid PlayStation product')
    product_id = urllib.parse.urlsplit(url).path.rstrip('/').rsplit('/', 1)[-1]
    cache = CACHE / (hashlib.sha256(('ps-game-v1:' + title.casefold()).encode()).hexdigest() + '.ps.json')
    item = None
    try:
        previous = json.loads(cache.read_text())
        if previous.get('source') == url and previous.get('title') == title and time.time() - previous['saved'] < 2592000:
            item = previous['item']
    except (OSError, ValueError, KeyError, TypeError): pass
    if item is None:
        document = fetch(url, 2097152).decode('utf-8')
        # The JSON-LD SKU verifies the edition; concept/recommendation art can
        # belong to the base game or another product and must not be selected.
        verified = False
        for body in re.findall(r'<script[^>]+type=["\']application/ld\+json["\'][^>]*>(.*?)</script>', document, re.S | re.I):
            product = json.loads(html.unescape(body))
            if (isinstance(product, dict) and product.get('@type') == 'Product'
                and product.get('sku') == product_id
                and match_key(clean_title(product.get('name', ''))) == match_key(title)):
                verified = True; break
        if not verified: raise ValueError('PlayStation product identity mismatch')
        script = re.search(r'<script[^>]+id=["\']__NEXT_DATA__["\'][^>]*>(.*?)</script>', document, re.S | re.I)
        if not script: raise ValueError('PlayStation media metadata unavailable')
        sections = json.loads(script[1])['props']['pageProps']['batarangs']
        for section in ('background-image', 'media-carousel'):
            text = sections.get(section, {}).get('text', '')
            embedded = re.search(r'<script[^>]+type=["\']application/json["\'][^>]*>(.*?)</script>', text, re.S | re.I)
            if not embedded: continue
            data = json.loads(embedded[1])
            if data.get('args', {}).get('productId') != product_id: continue
            product = data.get('cache', {}).get('Product:' + product_id, {})
            if (product.get('id') == product_id and match_key(clean_title(product.get('name', ''))) == match_key(title)
                and isinstance(product.get('media'), list)):
                item = {'id': product_id, 'media': product['media']}; break
        if item is None: raise ValueError('PlayStation product media unavailable')
        save_json(cache, {'title': title, 'source': url, 'item': item, 'saved': time.time()})
    roles = ('PORTRAIT_BANNER',) if kind == 'cover' else ('GAMEHUB_COVER_ART', 'EDITION_KEY_ART', 'BACKGROUND')
    seen = set()
    for role in roles:
        for media in item['media']:
            image = media.get('url', '')
            if media.get('type') != 'IMAGE' or media.get('role') != role or image in seen: continue
            if not allowed(image) or urllib.parse.urlsplit(image).hostname != 'image.api.playstation.com':
                raise ValueError('Invalid PlayStation media source')
            seen.add(image)
            yield {'provider': 'PlayStation', 'game': item['id'], 'id': item['id'], 'image': image,
                   'kind': kind, 'role': role, 'article': url}



def usable_rgb(data, kind='cover'):
    if len(data) != SIZES[kind][0] * SIZES[kind][1] * 3: return False
    return any(max(data[channel::3]) - min(data[channel::3]) > 8 for channel in range(3))


def raster(candidate, kind='cover'):
    image = Image.open(io.BytesIO(fetch(candidate['image'], 8388608)))
    expected = (candidate.get('width'), candidate.get('height'))
    valid_size = image.size in ([(300, 450), (600, 900), (1200, 1800)] if kind == 'cover' else [(1920, 620), (3840, 1240)]) if candidate.get('provider') == 'Steam' else image.size == expected
    if candidate.get('provider') == 'PlayStation':
        width, height = image.size
        valid_size = (2 <= width <= 4096 and 2 <= height <= 4096 and
                      (abs(width / height - 2 / 3) < 0.02 if kind == 'cover' else 1.5 <= width / height <= 3.2))
    if getattr(image, 'n_frames', 1) != 1 or not valid_size:
        raise ValueError('Unexpected artwork format')
    image = image.convert('RGB')
    if not any(high - low > 8 for low, high in image.getextrema()):
        raise ValueError('Blank artwork placeholder')
    # Keep the entire hero: a central 16:9 crop can remove the actual illustration.
    if kind == 'hero':
        canvas = ImageOps.fit(image, SIZES[kind], method=Image.Resampling.LANCZOS).filter(ImageFilter.GaussianBlur(18))
        full = ImageOps.contain(image, SIZES[kind], method=Image.Resampling.LANCZOS)
        canvas.paste(full, ((canvas.width-full.width)//2, (canvas.height-full.height)//2))
        data = canvas.tobytes()
    else:
        data = ImageOps.fit(image, SIZES[kind], method=Image.Resampling.LANCZOS).tobytes()
    if not usable_rgb(data, kind): raise ValueError('Blank artwork placeholder')
    return data

def hero_quality(data):
    # The app darkens its left side under text. Prefer art visible on the right,
    # rather than a highly voted hero whose visible portion is almost flat colour.
    image = Image.frombytes('RGB', SIZES['hero'], data)
    return max(ImageStat.Stat(image.crop((image.width*3//5, image.height//5, image.width, image.height*4//5))).stddev)


def save_bytes(path, data):
    # Unique siblings also protect shared provider metadata across processes.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + '.', suffix='.tmp', delete=False) as stream:
            temporary = pathlib.Path(stream.name)
            stream.write(data)
        temporary.replace(path)
    finally:
        if temporary is not None: temporary.unlink(missing_ok=True)

def save_json(path, data):
    save_bytes(path, json.dumps(data).encode())

def cover(title, kind='cover'):
    if kind not in SIZES: raise ValueError('Invalid artwork kind')
    title = canonical_title(title)
    if not isinstance(title, str) or not title or len(title) > 200: raise ValueError('Invalid cover alias')
    if is_collection(title): return b''
    # Fresh namespace: never reuse the previous mixed-provider images.
    ident = hashlib.sha256((('sgdb-hero-v2:' if kind == 'hero' else 'sgdb-v1:') + title.casefold()).encode()).hexdigest()
    game_ident = hashlib.sha256(('sgdb-v1:' + title.casefold()).encode()).hexdigest()
    path = CACHE / (ident + '.' + kind + '.rgb'); metadata = CACHE / (ident + '.' + kind + '.json')
    with LOCKS[int(game_ident[:2], 16) % len(LOCKS)]:
        try: previous = json.loads(metadata.read_text())
        except (OSError, ValueError): previous = {}
        age = time.time() - path.stat().st_mtime if path.exists() else float('inf')
        if path.exists() and (previous.get('resolver') == RESOLVER or (previous.get('resolver') in (7, 8, 9, 10, 11) and path.stat().st_size > 0)) and age < (2592000 if path.stat().st_size else 3600):
            data = path.read_bytes()
            if not data or usable_rgb(data, kind): return data
        errors = []
        match = previous.get('match')
        if match and match.get('provider') == 'SteamGridDB' and match.get('kind') == kind:
            try:
                result = raster(match, kind)
                save_bytes(path, result)
                save_json(metadata, {**previous, 'resolver': RESOLVER, 'saved': time.time()}); return result
            except Exception as error: errors.append(type(error).__name__)
        selected = None
        best_quality = -1
        try:
            ident_game = game(title, CACHE / (game_ident + '.game.json'))
            for index, candidate in enumerate(candidates(ident_game, kind) if ident_game else []):
                if kind == 'hero' and index >= 3: break
                try: result = raster(candidate, kind)
                except Exception as error: errors.append(type(error).__name__); continue
                quality = hero_quality(result) if kind == 'hero' else 100
                if quality > best_quality: selected = (candidate, result); best_quality = quality
                if kind == 'cover' or quality >= 20: break
        except Exception as error: errors.append(type(error).__name__)
        if selected is None:
            try:
                # Resolve metadata again instead of pinning an expired saved URL.
                refresh_needed = False
                for candidate in steam_candidates(title, kind):
                    try: result = raster(candidate, kind)
                    except Exception as error:
                        errors.append(type(error).__name__)
                        refresh_needed |= isinstance(error, urllib.error.HTTPError) and error.code in (404, 410)
                        continue
                    selected = (candidate, result); break
                if selected is None and refresh_needed:
                    for candidate in steam_candidates(title, kind, refresh=True):
                        try: result = raster(candidate, kind)
                        except Exception as error: errors.append(type(error).__name__); continue
                        selected = (candidate, result); break
            except Exception as error: errors.append(type(error).__name__)
        if selected is None:
            try:
                for candidate in playstation_candidates(title, kind):
                    try: result = raster(candidate, kind)
                    except Exception as error: errors.append(type(error).__name__); continue
                    selected = (candidate, result); break
            except Exception as error: errors.append(type(error).__name__)
        if selected:
            candidate, result = selected
            save_bytes(path, result)
            save_json(metadata, {'resolver': RESOLVER, 'canonical': title, 'match': candidate, 'saved': time.time()})
            return result
        if errors: raise RuntimeError('Artwork temporarily unavailable')
        # Confirmed misses expire after an hour. Authentication/outage errors above never do.
        save_bytes(path, b'')
        save_json(metadata, {'resolver': RESOLVER, 'canonical': title, 'match': None, 'saved': time.time()})
        return b''

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_GET(self):
        if not hmac.compare_digest(self.headers.get('X-Api-Key', ''), KEY): self.send_error(403); return
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
        title = query.get('title', [''])[0]
        kind = query.get('kind', ['cover'])[0]
        if kind not in SIZES or not title or len(title) > 200 or any(ord(c) < 32 for c in title): self.send_error(400); return
        try: data = cover(title, kind)
        except Exception: self.send_error(503, 'Artwork temporarily unavailable'); return
        self.send_response(200); self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)

if __name__ == '__main__':
    CACHE.mkdir(parents=True, exist_ok=True)
    ThreadingHTTPServer(('127.0.0.1', 9697), Handler).serve_forever()
