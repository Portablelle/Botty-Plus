# A53 PPR patch

Upstream: https://github.com/drakmor/ppr-patch
Revision: `fd4c8224563130e9698d3b2b2f44712826ceb525`
License: GPL-3.0 (bundled). Complete corresponding source: `ppr-patch-source.tar.gz`.

Unmodified `build/a53_ppr_install.elf`, built with PS5 Payload SDK v0.43 on the VPS using the upstream Makefile target. The standard installer uses paired transport and does not enable the separate global-clock acceleration option. Profiles cover exact firmware/target mappings through 11.40; this does not establish console acceptance on all profiles.

Opt-in only. Installation requires idle PPR reads, mounts, APR binds and unmounts. The launcher sends it before ShadowMountPlus and waits for the user to confirm the successful payload notification. Socket delivery alone cannot establish successful installation.
