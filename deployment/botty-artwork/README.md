# Game artwork resolver

SteamGridDB is the primary image provider for PS5 and Switch. If the game or
requested image is unavailable there, the resolver uses official Steam library
assets as a fallback. Operator-verified PlayStation Store products provide a
final fallback for the configured console-only releases/editions. The authenticated
`/artwork?title=...&kind=cover|hero` route returns RGB bytes to the console service:

- `cover` prefers static **600×900** grids, also accepting **660×930** and
  **342×482** portraits when needed. Every image is fitted into **160×240**;
  landscape banners and square icons are excluded.
- `hero` selects static **1920×620**, **3840×1240** or **1600×650** heroes and fits
  them into a **640×360** canvas while preserving the complete composition, with
  blurred fill above/below wide artwork. Up to three top-ranked heroes are checked
  to prefer illustration visible on the right, away from the UI text scrim.

Steam fallback matches the complete game name, fetches typed game metadata from
`IStoreBrowseService/GetItems`, and uses `library_capsule[_2x]` or
`library_hero[_2x]`. Modern per-image hash paths are read from the metadata; old
hard-coded CDN paths can return 404 even for available artwork. Steam requires
no additional API key. Store banners, demos, DLC and unrelated annual editions
are never substituted for the game portrait. All providers share the same
160×240 / 640×360 output dimensions and persistent caches.

For PlayStation Store, `/opt/botty-artwork/playstation-games.json` maps normalized
names to verified HTTPS product URLs (override with `BOTTY_PLAYSTATION_GAMES`).
NHL 27 and Dragon’s Dogma 2: Dark Arisen are included. The resolver checks the
JSON-LD product name/SKU and reads only that exact product’s media from the
embedded store data. `PORTRAIT_BANNER` supplies the cover; `GAMEHUB_COVER_ART`,
`EDITION_KEY_ART` and `BACKGROUND` supply the background. Concept/base-game,
upsell, screenshot, logo and square master images are excluded. This preserves
the actual Dark Arisen edition rather than selecting the original game’s art.
No PlayStation account, session or API key is required. Product media metadata
and resulting images persist in the existing caches.

The app uses a blurred cover behind its text if neither provider has a hero. It keeps a
uniform title placeholder if there is no portrait; square Switch icons, Steam
banners and Wikipedia images are no longer substituted.

## API key

On the artwork host (the current installation uses SSH alias `test`), put only the
SteamGridDB API key in **`/opt/botty-artwork/steamgriddb-api-key`**. The service reads
it on each uncached API lookup; replacing the key needs no restart.

```sh
sudo install -o root -g botty-artwork -m 0640 /dev/null /opt/botty-artwork/steamgriddb-api-key
sudoedit /opt/botty-artwork/steamgriddb-api-key
```

Run the first command only when creating the file; it would erase an existing key.
Generate a key at https://www.steamgriddb.com/profile/preferences/api . To use another
path, set `BOTTY_STEAMGRIDDB_KEY_FILE` in the systemd unit's environment. Keep this
key on the server, outside source control and console packages. The existing
`/opt/botty-artwork/api-key` is a separate key: it still authenticates the console's
Prowlarr/artwork proxy requests.

## Matching and caches

Release metadata, regions, editions, accents, punctuation and Roman numerals are normalized.
Switch scene suffixes (`NSW-GROUP`, `NSW Switch`) and PS5 VR markers are removed.
Only a parenthesized year disambiguates releases; numbers inside a game name,
such as Cyberpunk 2077, remain part of its identity. Canonical SteamGridDB names
are compared without release cleanup so editions, DLC and fan demakes stay distinct.
A match must preserve title/sequel identity. Multiple matching games (for example
a remake) require an explicit year in the canonical alias or a game-ID override.
Missing conjunctions and articles can be normalized and a distinctive subtitle
can be queried again, while the complete game identity still has to match.
No first-result fuzzy match is silently accepted.

`/opt/botty-artwork/aliases.json` maps normalized names to canonical titles. A
`ps5:` prefix limits an alias to explicitly marked PS5 releases, allowing the
correct Resident Evil 4 / Demon’s Souls remake without affecting Switch names.
Optional `/opt/botty-artwork/steamgriddb-games.json` maps normalized titles to
positive numeric SteamGridDB game IDs, for example `{"astrobot": 5454475}`. These
operator files are read on lookup and survive upgrades. `sources.json` from the
previous resolver is preserved but no longer used. When changing an override,
back up/remove that title's `sgdb-v1` cache entries to apply it immediately.

Successful Steam game metadata is cached alongside SteamGridDB matches.
Server matches, portraits and heroes persist in `/var/cache/botty-artwork`.
Positive images and game matches last thirty days; confirmed misses last an hour.
Authentication, rate limits and network errors are retried rather than saved as
missing artwork. A title lock coalesces concurrent cover/hero lookups; network
concurrency is bounded to four. Credentials never enter saved metadata. Images,
response sizes and HTTPS origins/redirects are checked, including rejecting
landscape grids, animated images and blank CDN tiles.

The console service uses separate `cache/sgdb-covers-v1` and
`cache/sgdb-heroes-v2` directories under its root, bounded to 512 portraits and 128
heroes. Full release names reach the resolver so scene markers and DLC Unlocker
suffixes can be cleaned correctly. The native app has its own persistent `/download0/botty-sgdb-covers-v1.cache`
and `/download0/botty-sgdb-backgrounds-v2.cache` files with the same capacities
(about 56 and 84 MiB). They work after closing/reopening the app, without a service
request for a cached image. Least recently used native entries are replaced;
checksums reject damaged or interrupted writes. Delete these two files with the
app closed to reset the native caches. The new namespaces automatically bypass
older mixed-format caches; those old files can be removed later with the app
closed. They are not deleted by this update.

## Deployment and checks

Use Python 3, Pillow, the dedicated `botty-artwork` user and the supplied systemd
unit. The listener stays on `127.0.0.1:9697`; nginx exposes only its authenticated
HTTPS route. Preserve both key files, aliases, game-ID overrides, ownership and
rollback copies when updating. Deploy the matching console service/native app to
enable Switch portraits and heroes throughout the UI. Do not interrupt active
extraction/compression or replace an open native title.

Run `python3 deployment/botty-artwork/test_server.py` for deterministic provider,
matching, format, cache, secret-handling and outage checks, without public requests.
See https://www.steamgriddb.com/api/v2 for the provider API.


## Audit real Prowlarr results

Keep raw Prowlarr responses and credentials in ignored private backups. Export a
sanitized JSON array containing only title strings or objects with a `title` field.
On the artwork host, run under the artwork service user:

```sh
sudo -u botty-artwork python3 audit.py --resolver /opt/botty-artwork/server.py \
  --corpus /private/prowlarr-titles.json --images --output /private/artwork-audit.json
```

The audit checks every release, downloads/verifies the final portrait with
`--images`, and records matched game IDs, missing portraits, unresolved candidates
and request failures separately, including the actual image provider. It never downloads torrents or changes console
files. At most two workers run. Known multi-game library/collection packs are reported as `not-single-game`.
Reports contain no credentials or download URLs;
existing output files are not overwritten. Game-library bundles and releases for
other consoles can appear in category 1000; keep these distinct when interpreting
coverage of individual PS5/Switch games. Do not substitute a previous sequel or
a different game just to eliminate a missing-artwork result.
