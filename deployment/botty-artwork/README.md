# Cover resolver

Run `server.py` as the dedicated `botty-artwork` user with Python 3 and Pillow,
using the systemd service and authenticated TLS nginx route shipped alongside it.
The API key remains in `/opt/botty-artwork/api-key`, never in this repository.

Resolution uses normalized game names (scene tags, editions, punctuation, accents,
Roman numerals, trademark symbols), Steam game search/library artwork, Wikipedia
exact article lookup followed by search, then a Steam store banner if no portrait
is available. Matches must preserve the game identity and sequel number. Provider
IDs, source URLs, canonical titles and outcomes are saved next to RGB images.
Positive results last thirty days. Confirmed misses last one hour; network/provider
errors are not negative-cached. Different titles can resolve concurrently; duplicate
lookups coalesce and network concurrency is bounded to four.

Near-uniform provider images are rejected, including grey CDN placeholders returned
with HTTP 200. Existing cached placeholders are re-resolved rather than retained for
thirty days. Battlefield 6 has a verified PlayStation product mapping in the seed file.

For regional names or verified exceptions, `/opt/botty-artwork/aliases.json` can map
a normalized title to a canonical game name. It is read on each request and survives
updates. Example: `{"regionalname":"Canonical Game"}`. No arbitrary image URL is
accepted from the client. HTTPS destinations and redirects are allowlisted, response
sizes and image dimensions are bounded. Images retain their complete composition.

Verified PlayStation product pages can be registered in `/opt/botty-artwork/sources.json`
(mapping normalized game names to official product URLs; seed file provided). The
resolver reads Product JSON-LD, verifies the title and sequel, and downloads only
from allowlisted PlayStation origins. Publisher prefixes such as EA SPORTS and
trademark marks do not change identity. This mapping survives service upgrades.

Coverage cannot be guaranteed for every release: games missing from both sources
retain an explicit title placeholder. Do not silently substitute another sequel.
The audit and cached metadata identify unresolved names for later corrections.

References:
- https://partner.steamgames.com/doc/store/assets/libraryassets
- https://www.mediawiki.org/wiki/API:Pageimages

Run `python3 test_server.py` to check matching, cache reuse, fallback, outage retry,
origin restrictions and persistent aliases without public network requests.
