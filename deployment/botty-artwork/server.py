#!/usr/bin/env python3
"""Game cover resolver with bounded fetching, persistent matches and two providers."""
import html, hashlib, hmac, io, json, os, pathlib, re, threading, time, unicodedata, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from PIL import Image, ImageOps
CACHE = pathlib.Path(os.environ.get('BOTTY_ARTWORK_CACHE', '/var/cache/botty-artwork'))
KEY = pathlib.Path(os.environ.get('BOTTY_ARTWORK_KEY_FILE', '/opt/botty-artwork/api-key')).read_text().strip()
SOURCES = pathlib.Path(os.environ.get('BOTTY_ARTWORK_SOURCES', '/opt/botty-artwork/sources.json'))
ALIASES = pathlib.Path(os.environ.get('BOTTY_ARTWORK_ALIASES', '/opt/botty-artwork/aliases.json'))
UA = 'Botty/0.3 (personal game library; https://github.com/Portablelle/botty-ps5)'
HOSTS = {'store.playstation.com', 'image.api.playstation.com', 'en.wikipedia.org', 'upload.wikimedia.org', 'store.steampowered.com', 'cdn.akamai.steamstatic.com', 'shared.akamai.steamstatic.com', 'cdn.cloudflare.steamstatic.com'}
LOCKS = [threading.Lock() for _ in range(32)]
NETWORK = threading.BoundedSemaphore(4)
Image.MAX_IMAGE_PIXELS = 12000000

def allowed(url):
    p = urllib.parse.urlsplit(url)
    return p.scheme == 'https' and p.hostname in HOSTS and not p.username and p.port in (None, 443)

class Redirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not allowed(newurl): raise ValueError('Untrusted artwork redirect')
        return super().redirect_request(req, fp, code, msg, headers, newurl)

def fetch(url, limit):
    if not allowed(url): raise ValueError('Untrusted artwork source')
    with NETWORK, urllib.request.build_opener(Redirects).open(urllib.request.Request(url, headers={'User-Agent': UA}), timeout=4) as response:
        data = response.read(limit + 1)
        if len(data) > limit: raise ValueError('Artwork too large')
        return data

def clean_title(title):
    """Remove release metadata while preserving scene words in unmarked names."""
    title = title.replace('_', ' ')
    title = re.sub(r'(?<=[A-Za-z])\.(?=[A-Za-z])', ' ', title)
    platform_marker = r'\b(?:PS5|(?:CUSA|PPSA)\d+)\b'
    platform_release = bool(re.search(platform_marker + r'|\[FPKG\]', title, flags=re.I))
    title = re.sub(r'\[[^\]]*\]', ' ', title)
    release_marker = re.compile(platform_marker + r'|\s+(?:incl\.?|including|v\d+(?:\.\d+)*|update|MULTI\d*)\b', re.I)
    release_name = platform_release or bool(release_marker.search(title))
    title = release_marker.split(title, maxsplit=1)[0].rstrip(' .-:')
    # Alternate metadata and edition removal until stable, regardless of order.
    # Scene words are ambiguous in canonical names; require a release marker.
    while True:
        trimmed = re.sub(r'\s+(?:DLC|add[ -]?on)(?:\s+add[ -]?on)?(?:\s+only)?\s*$', '', title, flags=re.I)
        if release_name:
            trimmed = re.sub(r'[\s.:-]+(?:proper|repack|rerip|readnfo|internal)[\s.:-]*$', '', trimmed, flags=re.I)
        trimmed = re.sub(r'\s+(?:the shores|(?:standard|digital deluxe|deluxe|ultimate|complete|gold|precious|anniversary|definitive|premium|special|ballpark|collectors?|game of the year) edition|directors? ?cut|goty)\s*$', '', trimmed, flags=re.I)
        if trimmed == title: break
        title = trimmed
    return re.sub(r'\s+', ' ', title).strip(' .-')

def normalize(title):
    title = re.sub(r'\s*\([^)]*\)', '', title.casefold()).replace('&', ' and ').replace('™', '').replace('®', '').replace('+', ' and ')
    title = re.sub(r'^ea\s+sports\s+', '', title)
    title = ''.join(c for c in unicodedata.normalize('NFKD', title) if not unicodedata.combining(c))
    roman = {'ii':'2', 'iii':'3', 'iv':'4', 'v':'5', 'vi':'6', 'vii':'7', 'viii':'8', 'ix':'9', 'x':'10'}
    title = re.sub(r'\b(?:ii|iii|iv|v|vi|vii|viii|ix|x)\b', lambda m: roman[m.group()], title)
    return re.sub(r'[^\w]+', '', title)

def steam_candidates(title):
    query = urllib.parse.urlencode({'term': title, 'l': 'english', 'cc': 'us'})
    data = json.loads(fetch('https://store.steampowered.com/api/storesearch/?' + query, 262144))
    for item in data.get('items', [])[:20]:
        if normalize(clean_title(item.get('name', ''))) != normalize(title): continue
        app = str(item.get('id', ''))
        if not app.isdigit(): continue
        yield {'provider': 'Steam', 'title': item['name'], 'id': app,
               'article': 'https://store.steampowered.com/app/' + app,
               'image': 'https://cdn.akamai.steamstatic.com/steam/apps/' + app + '/library_600x900.jpg'}
        details = json.loads(fetch('https://store.steampowered.com/api/appdetails?appids=' + app + '&l=english', 1048576)).get(app, {}).get('data', {})
        if normalize(clean_title(details.get('name', ''))) == normalize(title) and details.get('header_image'):
            yield {'provider': 'Steam', 'title': item['name'], 'id': app, 'kind': 'banner',
                   'article': 'https://store.steampowered.com/app/' + app, 'image': details['header_image']}


def wikipedia_candidates(title):
    capital = title.title()
    capital = re.sub(r'\b(?:Ii|Iii|Iv|Vi|Vii|Viii|Ix|Nhl|Nba|Nfl)\b', lambda m: m.group().upper(), capital)
    common = dict(action='query', format='json', formatversion=2, prop='pageimages', redirects=1,
                  piprop='thumbnail', pithumbsize=320, pilicense='any', pilimit=10)
    queries = [dict(common, titles='|'.join(dict.fromkeys([title, capital, capital+' (video game)']))),
               dict(common, generator='search', gsrsearch='intitle:' + title, gsrlimit=10, gsrnamespace=0)]
    seen=set()
    for query in queries:
        data = json.loads(fetch('https://en.wikipedia.org/w/api.php?' + urllib.parse.urlencode(query), 262144))
        for page in data.get('query', {}).get('pages', []):
            if normalize(page.get('title', '')) != normalize(title) or re.search(r'\((?:series|franchise)\)', page.get('title', ''), re.I): continue
            image = page.get('thumbnail', {}).get('source')
            if image and image not in seen:
                seen.add(image)
                yield {'provider': 'Wikipedia', 'title': page['title'], 'id': page.get('pageid'),
                       'article': 'https://en.wikipedia.org/wiki/' + urllib.parse.quote(page['title']), 'image': image}

def playstation_candidates(title):
    # Operator-verified product identities, never a URL supplied by the client.
    try: sources = json.loads(SOURCES.read_text())
    except (OSError, ValueError): return
    url = sources.get(normalize(title))
    if not url: return
    parsed = urllib.parse.urlsplit(url)
    if parsed.hostname != 'store.playstation.com' or not re.fullmatch(r'/[a-z]{2}-[a-z]{2}/product/[A-Z0-9_-]+/', parsed.path):
        raise ValueError('Invalid PlayStation product')
    document = fetch(url, 2097152).decode('utf-8')
    for body in re.findall(r'<script[^>]+type=["\']application/ld\+json["\'][^>]*>(.*?)</script>', document, re.S | re.I):
        product = json.loads(html.unescape(body))
        if product.get('@type') != 'Product' or normalize(clean_title(product.get('name', ''))) != normalize(title): continue
        if isinstance(product.get('image'), str):
            yield {'provider': 'PlayStation', 'title': product['name'], 'id': product.get('sku'), 'article': url, 'image': product['image']}


def usable_rgb(data):
    if len(data) != 115200: return False
    # Some CDNs return a successful JPEG response containing only a grey tile.
    # Allow compression noise, but require actual contrast in at least one channel.
    return any(max(data[channel::3]) - min(data[channel::3]) > 8 for channel in range(3))

def raster(candidate):
    image = Image.open(io.BytesIO(fetch(candidate['image'], 2097152))).convert('RGB')
    if not any(high - low > 8 for low, high in image.getextrema()):
        raise ValueError('Blank artwork placeholder')
    data = ImageOps.pad(image, (160, 240), color=(18, 25, 35), method=Image.Resampling.LANCZOS).tobytes()
    if not usable_rgb(data): raise ValueError('Blank artwork placeholder')
    return data

def save_json(path, data):
    temp = path.with_suffix('.tmp-json'); temp.write_text(json.dumps(data)); temp.replace(path)

def cover(title):
    original = clean_title(title)
    try: title = json.loads(ALIASES.read_text()).get(normalize(original), original)
    except (OSError, ValueError): title = original
    if not isinstance(title, str) or not title or len(title) > 200: raise ValueError('Invalid cover alias')
    ident = hashlib.sha256(('v4:' + normalize(title)).encode()).hexdigest()
    path = CACHE / (ident + '.rgb'); metadata = CACHE / (ident + '.json')
    with LOCKS[int(ident[:2], 16) % len(LOCKS)]:
        try: previous = json.loads(metadata.read_text())
        except (OSError, ValueError): previous = {}
        age = time.time() - path.stat().st_mtime if path.exists() else float('inf')
        if path.exists() and (path.stat().st_size or previous.get('resolver') == 6) and age < (2592000 if path.stat().st_size else 3600):
            data = path.read_bytes()
            if not data or usable_rgb(data): return data
        try: previous = json.loads(metadata.read_text())
        except (OSError, ValueError): previous = {}
        errors = []; deferred = []
        # Reuse the saved provider ID/image before doing name resolution again.
        if previous.get('match'):
            try:
                result = raster(previous['match']); temporary = path.with_suffix('.tmp'); temporary.write_bytes(result); temporary.replace(path); return result
            except Exception as error: errors.append(type(error).__name__)
        for provider in (playstation_candidates, steam_candidates, wikipedia_candidates):
            try:
                for candidate in provider(title):
                    if candidate.get('kind')=='banner': deferred.append(candidate); continue
                    try: result = raster(candidate)
                    except Exception as error: errors.append(type(error).__name__); continue
                    temporary = path.with_suffix('.tmp'); temporary.write_bytes(result); temporary.replace(path)
                    save_json(metadata, {'resolver': 6, 'requested': original, 'canonical': title, 'match': candidate, 'saved': time.time()})
                    return result
            except Exception as error: errors.append(type(error).__name__)
        for candidate in deferred:
            try:
                result = raster(candidate); temporary = path.with_suffix('.tmp'); temporary.write_bytes(result); temporary.replace(path)
                save_json(metadata, {'resolver': 6, 'requested': original, 'canonical': title, 'match': candidate, 'saved': time.time()}); return result
            except Exception as error: errors.append(type(error).__name__)
        save_json(metadata, {'resolver': 6, 'requested': original, 'canonical': title, 'match': None, 'errors': errors, 'saved': time.time()})
        # Provider outages must not turn into permanent "no cover" results.
        if errors: raise RuntimeError('Artwork providers temporarily unavailable')
        path.write_bytes(b''); return b''

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_GET(self):
        if not hmac.compare_digest(self.headers.get('X-Api-Key', ''), KEY): self.send_error(403); return
        title = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query).get('title', [''])[0]
        if not title or len(title) > 200 or any(ord(c) < 32 for c in title): self.send_error(400); return
        try: data = cover(title)
        except Exception: self.send_error(503, 'Artwork temporarily unavailable'); return
        self.send_response(200); self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)

if __name__ == '__main__':
    CACHE.mkdir(parents=True, exist_ok=True)
    ThreadingHTTPServer(('127.0.0.1', 9697), Handler).serve_forever()
