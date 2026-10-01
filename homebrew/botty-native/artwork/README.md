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

## Native UI artwork (00.006.000)

Four original assets were generated with the built-in `image_gen` tool on
2026-10-01. They are decorative Botty artwork, never substitutes presented as
real game covers. Source PNGs are retained here:

- `nebula.png`: original space environment; `assets/nebula.rgb`, 960 x 540 RGB.
- `courier.png`: coral data-courier robot; `assets/courier.rgba`, 512 x 512 RGBA.
- `extractor.png`: golden decompression engine; `assets/extractor.rgba`, same format.
- `vault.png`: mint cartridge vault; `assets/vault.rgba`, same format.

Rebuild with `python3 tools/build-backdrop.py` (macOS `sips`) and
`python3 tools/build-illustrations.py` (Pillow 12). The native renderer loads
fixed-size data once; it needs no image decoder, network request or continuous
animation for these assets. Missing/invalid decoration leaves a usable UI.

### Platform mark and game cases

`ps5-logo.svg` and its PNG raster come from Sony's PlayStation 5 wordmark,
via [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:PlayStation_5_logo_and_wordmark.svg).
The file page identifies Sony Interactive Entertainment and labels the simple
text logo PD-textlogo; the mark remains Sony's trademark. Source SVG URL:
https://upload.wikimedia.org/wikipedia/commons/c/cb/PlayStation_5_logo_and_wordmark.svg

`python3 tools/build-platform-logo.py` bakes the alpha mask into
`src/platform_logo.hpp`. The UI draws a white platform band and blue case edge
around provider artwork. This is a UI case treatment, not a claim that the source
image is a scan of a physical retail package. Provider images continue to come
from the existing exact-title PlayStation / Steam / Wikipedia resolver. Missing
images remain explicitly labeled, with no invented game artwork.

### Generation prompts

The environment used this complete prompt:

> Use case: stylized-concept. Asset type: background artwork for a native PlayStation 5 gaming library UI, landscape 16:9. Original cinematic science-fiction vista, obsidian terrain and a vast luminous orbital ring rising above a distant planet on the RIGHT HALF, electric violet aurora and cyan light trails sweeping across midnight blue space. Premium AAA game art, rich atmospheric depth, sharp elegant lighting, restrained stars and subtle mist, exciting and immersive. Composition: left 55 percent very dark calm navy negative space for large white UI text; interesting planet and orbit details concentrated top right and right edge; bottom quarter fades into near-black navy to support game covers. Beautiful violet/cyan accents, no bright white highlights. NO text, letters, logos, symbols, interface elements, watermarks or characters. This is ONLY the background art, not an interface mockup.

Each transparent illustration used the following prefix, its subject paragraph,
and the common suffix. `transparent_background` was `true`.

Prefix:

> Use case: stylized-concept. Asset type: custom isolated 3D illustration integrated into a native PlayStation gaming UI. Create

Courier subject:

> a premium futuristic gaming data-courier robot, glossy warm ivory ceramic helmet with dark glass visor and two expressive coral LED eyes, compact floating body, chunky graphite mechanical limbs, a small luminous coral data capsule carried under one arm, subtle cyan accents. Dynamic three-quarter pose floating diagonally upward, elegant adult collector design, sophisticated playful gaming personality. Orange-coral rim light, believable PBR materials, soft studio reflections. Full object entirely visible, no text or logos.

Extractor subject:

> a premium futuristic decompression engine for a gaming console app: one large floating beveled graphite archive cube opening into three separated precision-engineered layers, glowing amber gold core, several small solid golden data cubes orbiting outwards, elegant turbine rings and tiny amber emissive strips. Dynamic three-quarter isometric product render, highly crafted hard-surface model, smoked glass, anodized titanium and satin gold details. Full object entirely visible, no text or logos.

Vault subject:

> a premium futuristic gaming vault: three sculptural ivory and dark graphite game cartridges standing at dynamic angles inside an open levitating circular console dock, beautiful mint green light shining between the cartridges, a thin mint holographic orbital ring, precision machined silver edges, subtle glass panels, exquisite high-end gaming hardware product design. Three-quarter isometric composition, playful but sophisticated, crisp PBR render. Full object entirely visible, no text or logos.

Suffix:

> Square composition centered with generous 8% margin, dramatic soft studio lighting, premium art-directed product render, high detail without visual clutter. Transparent background, clean silhouette, no floor plane, no environment, no cast shadow outside the object. Not an interface mockup. No letters, numbers, logos, watermark or existing branded characters.
