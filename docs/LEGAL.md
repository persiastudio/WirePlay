# Licences, royalties and what you may distribute

Short version: you may redistribute WirePlay freely, including commercially.
There are no royalties and no per-unit fee. There are two obligations you
actually have to meet — keep the engine's source available, and keep FFmpeg
linked dynamically.

## Component by component

| Component | Licence | How it is linked | Obligation |
|---|---|---|---|
| `src/` (the application) | MIT | — | keep the copyright notice |
| `engine/wireplay_engine.py` | GPL-3.0 | **separate process**, over a pipe | provide its source |
| `pymobiledevice3` | GPL-3.0-or-later | imported by the engine only | provide its source |
| FFmpeg | LGPL-2.1+ | **dynamic** (DLL) | allow relinking |
| SDL2 | zlib | dynamic | none of substance |
| Python runtime | PSF | interpreter | none |

## The GPL boundary

`pymobiledevice3` is GPL-3.0. Linking it into the application would make the
whole application GPL, which is not a problem for a free tool but is a
surprise nobody wants.

So the engine runs as a **child process** and the two sides talk over a pipe
using the format in `WIRE_PROTOCOL.md`. Separate processes communicating at
arm's length is the boundary the FSF's own guidance treats as keeping programs
separate, so the application stays MIT while the installer can still ship the
engine in the same folder.

This is not a novel arrangement — it is what other tools in this space do for
the same reason — but it is a real constraint on the code: **do not import
pymobiledevice3 from the C++ side, and do not merge the engine into the
application binary.** If you do, the application becomes GPL-3.0.

The practical obligation is that anyone who receives a binary must be able to
get the engine's source. Shipping the `.py` file next to the executable, as the
bundling step does, satisfies it.

## FFmpeg: keep it dynamic

FFmpeg is LGPL-2.1+ *as configured by the vcpkg port*, which builds shared
libraries by default. LGPL requires that a recipient be able to relink against
a modified FFmpeg; with DLLs that means swapping the file, which they can do.

**A static FFmpeg build would change this.** You would then need to provide
objects or another relinking mechanism. Don't flip that switch without reading
LGPL-2.1 §6.

Two notes on the build configuration:

- The default vcpkg FFmpeg does not enable `--enable-gpl`, so no GPL-only
  component (x264, x265, the GPL postprocessing filters) is present. Keep it
  that way — enabling GPL would make FFmpeg itself GPL and drag the
  dynamic-linking argument apart.
- We only **decode**. Nothing here encodes or transcodes.

## Patents and royalties

This is the question that usually decides whether a tool can ship, so here it
is directly:

**HEVC / H.265.** Access Advance and Via LA administer HEVC pools. Their
obligations fall on those who *make, sell or distribute encoders and decoders*.
WirePlay distributes neither: the HEVC decoder is FFmpeg's, and FFmpeg's own
licence position is that codec patent licensing is the user's matter, not
upstream's. We ship FFmpeg's shared libraries unmodified. The **encoder is
inside iOS** — Apple has already dealt with it. If you fork this and add an
encoder of your own, that analysis changes and you should get an answer you
trust.

**AAC-ELD.** Decode-only again, via FFmpeg's AAC decoder. Same reasoning.

**H.264.** Not used. We do not decode it and we do not link an encoder.

**No proprietary Apple code, no MFi chip, no FairPlay.** This is the part worth
dwelling on, because it is where comparable projects carry real risk. An AirPlay
receiver has to answer Apple's `/fp-setup` handshake, and every open-source
implementation does that with a blob of precomputed FairPlay responses plus a
GPL decryption routine carried over from UxPlay/RPiPlay. Those bytes are
produced by Apple's FairPlay implementation and their redistribution sits in a
genuinely unsettled place.

WirePlay uses CoreDevice instead. There is no FairPlay handshake, no
precomputed Apple blob, and no MFi authentication anywhere in the tree. That is
a real advantage of this approach and not just an aesthetic one.

The honest counterweight: CoreDevice's protocols are reverse-engineered.
`pymobiledevice3` documents this about itself. Using it for a personal tool is
uncontroversial; whether a product built on it squares with Apple's developer
terms is a question for someone qualified to answer it, and it is not answered
here.

## Trademarks

"WirePlay" is this project's name. AirPlay, iPhone, iOS, iTunes and QuickTime
are Apple's. Nothing here is endorsed by or affiliated with Apple.
