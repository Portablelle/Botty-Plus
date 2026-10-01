# Botty+ home-screen artwork

Original generated PNGs are kept here; runtime assets live in `sce_sys/`.

- `icon0.png`: opaque 512 × 512 PNG.
- `pic0.dds`: selection background, 3840 × 2160, DX10 BC7 UNORM,
  one mip, opaque alpha. The generated source is upscaled for the PS5 slot.
- Launch-screen artwork is unchanged.

Format conversion used PSGFX's imaging pipeline at commit
`a9d73d9fea3382955476fd56ec3c74e08ea2e0ab` (https://github.com/elripalda/psgfx),
with stb resizing and bc7enc; no change to the generated design. Both generated
originals are retained. The runtime package and portal include the converted art.

Console deployment synchronizes the native sce_sys, the registered title's
sce_sys, /user/appmeta and /system_data/priv/appmeta for PPSA99071, including
/user/app/PPSA99071/icon0.png. Original files were backed up with the suffix
`.before-botty-plus-artwork`; local rollback manifest is under build/artwork-rollback.
The official application registration refresh succeeded. The shell may retain
previous art until the selection changes or the console is restarted.
