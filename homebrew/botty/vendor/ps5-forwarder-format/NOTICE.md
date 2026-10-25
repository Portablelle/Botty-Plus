# PS5 Forwarder Format

Upstream: https://github.com/heydemoura/ps5-forwarder-format
Revision: `22e8762ad7dbd01e18bd8c0e95f86461fdf8aaf4` (GPL-3.0-or-later).

`source.tar.gz` contains the complete pinned repository, including its license,
launcher and forwarder sources, specification and prebuilt template. Botty embeds
the upstream template unchanged in its manager. No launcher is injected while
creating a tile; the upstream tile starts its bundled loopback launcher on demand.

Rebuilding the template requires the dedicated Linux host; see the archive's
README and Makefile. The source archive is included in Botty's corresponding sources.
