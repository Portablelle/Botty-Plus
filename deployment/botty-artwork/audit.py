#!/usr/bin/env python3
"""Audit a sanitized Prowlarr title corpus on the artwork host; never needs Prowlarr credentials."""
import argparse, concurrent.futures, hashlib, importlib.util, json, pathlib, re, time, urllib.parse


def audit(resolver, row, images=False):
    raw = row if isinstance(row, str) else row['title']
    title = resolver.canonical_title(raw) if hasattr(resolver, 'canonical_title') else resolver.clean_title(raw)
    result = {'title': raw, 'canonical': title}
    started = time.monotonic()
    if hasattr(resolver, 'is_collection') and resolver.is_collection(title):
        return {**result, 'status': 'not-single-game', 'elapsedMs': 0}
    try:
        key = hashlib.sha256(('sgdb-v1:' + title.casefold()).encode()).hexdigest()
        if images:
            result['coverBytes'] = len(resolver.cover(raw))
            if result['coverBytes']:
                metadata = json.loads((resolver.CACHE / (key + '.cover.json')).read_text())
                match = metadata['match']
                result.update(status='covered', game=match['game'], provider=match['provider'])
                result['elapsedMs'] = round((time.monotonic() - started) * 1000)
                return result
        ident = resolver.game(title, resolver.CACHE / (key + '.game.json'))
        result['game'] = ident
        if ident:
            result.update(status='no-portrait' if images else 'matched', provider='SteamGridDB')
        else:
            # A Steam game may exist without a usable portrait.
            steam_key = hashlib.sha256(('steam-game-v1:' + title.casefold()).encode()).hexdigest()
            try:
                previous = json.loads((resolver.CACHE / (steam_key + '.steam.json')).read_text())
                if previous.get('title') == title: result.update(game=previous['item']['appid'], provider='Steam')
            except (OSError, ValueError, KeyError): pass
            if result.get('game'):
                result['status'] = 'no-portrait' if images else 'matched'
            else:
                query = re.sub(r'\s*\([^)]*\)', '', title)
                result['candidates'] = [{k: x.get(k) for k in ('id', 'name', 'release_date')}
                                        for x in resolver.sgdb('search/autocomplete/' + urllib.parse.quote(query, safe=''))]
                result['status'] = 'unmatched' if result['candidates'] else 'no-search-results'
    except Exception as exc:
        # Request exceptions can include URLs; only persist their class.
        result.update(status='error', error=type(exc).__name__)
        if hasattr(exc, 'code'): result['httpStatus'] = exc.code
    result['elapsedMs'] = round((time.monotonic() - started) * 1000)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resolver', default=str(pathlib.Path(__file__).with_name('server.py')))
    parser.add_argument('--corpus', required=True); parser.add_argument('--output', required=True)
    parser.add_argument('--images', action='store_true'); parser.add_argument('--workers', type=int, choices=(1, 2), default=2)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('artwork', args.resolver)
    resolver = importlib.util.module_from_spec(spec); spec.loader.exec_module(resolver)
    rows = json.loads(pathlib.Path(args.corpus).read_text())
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        results = list(pool.map(lambda row: audit(resolver, row, args.images), rows))
    counts = {s: sum(r['status'] == s for r in results) for s in sorted({r['status'] for r in results})}
    report = {'resolver': resolver.RESOLVER, 'saved': time.time(), 'imagesChecked': args.images,
              'total': len(results), 'counts': counts, 'results': results}
    with pathlib.Path(args.output).open('x') as out: json.dump(report, out, indent=2, ensure_ascii=False); out.write('\n')
    print(json.dumps({'total': len(results), 'counts': counts, 'output': args.output}))


if __name__ == '__main__': main()
