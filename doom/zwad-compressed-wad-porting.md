# ZWAD compressed-WAD implementation and porting guide

> **Status in this repository:** ported and implemented. This document was
> originally written against prboom-plus's file layout (`src/w_wad.c`,
> `src/w_mmap.c`, a host `run-prboom.sh`, zlib's streaming `inflate()` API);
> our vendored engine's layout and target differ in a few ways this port
> adapted for:
> - Files live at `components/prboom/w_wad.h`/`.c` and
>   `components/prboom/d_main.c` (`CheckIWAD()`), not under `src/`.
> - There is no separate `w_mmap.c` here - `W_CacheLumpNum()` in `w_wad.c`
>   is the only cache entry point (`W_LockLumpNum` is just a macro alias to
>   it), so the compressed-cache-path change described below in section 5
>   only needed to land in one place.
> - This engine has no debug memory HUD (section 7 doesn't apply here);
>   `W_CompressedBufferUsage()` still exists and is logged once at startup
>   instead.
> - ESP-IDF has no system zlib. On-device inflate uses a vendored
>   decompression-only subset of miniz (`components/tinfl`,
>   `tinfl_decompress()`) instead of zlib's `inflateInit`/`inflate`/
>   `inflateEnd`; the two APIs are streaming/chunked in the same shape, so
>   the algorithm described below carried over directly. The host-side
>   compressor (`tools/wadcompress.c`, `tools/compress-wad.sh`) still uses
>   the system zlib, since it only ever runs on a dev machine.
> - WADs are discovered on the `littlefs` partition, not an SD card or CLI
>   arg - see `findIwadInDir()`/`isIwadFile()` in `main/main.cpp`, which
>   also now recognize the `.zwad` extension and `"ZWAD"` header.
>
> Compress a WAD with `tools/compress-wad.sh in.wad out.zwad`, then flash it
> with the same `tools/flash_wads.sh` used for plain WADs.

## Scope

This document describes the complete implementation used in this repository
to load a compressed Doom WAD without expanding the whole WAD into memory.
It is for porting the feature to another Doom-engine codebase.

The implementation is a custom format called **ZWAD**.  It is not compatible
with unmodified source ports or general WAD editors.  A ZWAD-aware engine can
use it in place of an IWAD or PWAD.

The design goal is random access with the least persistent decompression
memory: every lump is an independent zlib stream, the directory remains
plain, and only the lump requested by the engine is decompressed.

## Format written by the compressor

The format is produced by [tools/wadcompress.c](../tools/wadcompress.c).  It
reads a normal `IWAD` or `PWAD`, zlib-compresses every non-empty lump
independently, then writes this layout:

```text
+-----------------------------+
| WAD header, 12 bytes         |
| id = "ZWAD"                  |
| numlumps (little-endian)     |
| directory offset             |
+-----------------------------+
| zlib stream for lump 0       |
+-----------------------------+
| zlib stream for lump 1       |
| ...                          |
+-----------------------------+
| uncompressed ZWAD directory  |
+-----------------------------+
```

The header keeps the normal WAD header layout:

```c
typedef struct {
  char identification[4];      /* "ZWAD" */
  int  numlumps;               /* little-endian */
  int  infotableofs;           /* little-endian */
} wadinfo_t;
```

A normal WAD directory record is 16 bytes.  A ZWAD record is 20 bytes:

```c
typedef struct {
  int  filepos;                /* offset of the lump's zlib stream */
  int  size;                   /* uncompressed lump length */
  int  compressed_size;        /* number of stored zlib bytes */
  char name[8];                /* fixed-width WAD lump name */
} zwadfilelump_t;
```

`zwadfilelump_t` belongs in a shared WAD header; here it is defined in
[src/w_wad.h](../src/w_wad.h).  Do not use `filelump_t` to read ZWAD
directories: the 16-vs-20-byte stride difference corrupts every entry after
the first.

Empty lumps have `size == 0` and no stream needs to be stored.  Names stay
uncompressed, which allows startup code to identify maps without inflating any
data.

## Components that must be ported

| Component | Current location | Required responsibility |
| --- | --- | --- |
| Compressor | `tools/wadcompress.c` | Write `ZWAD` header, independent zlib streams, and 20-byte directory. |
| Shared structures | `src/w_wad.h` | Define `zwadfilelump_t`; retain compressed length in run-time lump metadata. |
| WAD registration | `W_AddFile()` in `src/w_wad.c` | Recognize `ZWAD`, read its directory with the correct entry size, and retain per-lump compressed size. |
| Lump reads | `W_ReadLump()` in `src/w_wad.c` | Inflate one lump on demand into the caller's destination buffer. |
| Cache integration | `W_CacheLumpNum()` / `W_LockLumpNum()` in `src/w_mmap.c` | Make compressed lumps use the existing zone cache instead of a mapped file address. |
| Early IWAD identification | `CheckIWAD()` in `src/d_main.c` | Read the ZWAD directory correctly before normal WAD initialization. |
| Memory display | `W_CompressedBufferUsage()` plus HUD code in `src/hu_stuff.c` | Add the fixed decompression workspace separately to engine memory. |
| User tooling | `compress-wad.sh`, `run-prboom.sh` | Build/use the compressor and accept a compressed WAD path. |

## 1. Compressor: `tools/wadcompress.c`

The compressor must first parse the source WAD's ordinary header and
16-byte `filelump_t` directory.  For every source lump:

1. Read exactly its original uncompressed `size` bytes.
2. Compress it with zlib (`compress2` is sufficient for the writer).
3. Write the compressed bytes immediately to the output file.
4. Remember the output offset, original length, compressed length, and
   original eight-byte name.
5. After all streams, write the array of `zwadfilelump_t` records.
6. Seek back and write the `ZWAD` header containing the final directory
   offset.

The independent-stream rule is essential.  A single zlib stream for the whole
WAD would require replaying or retaining all preceding data to access a later
lump, which defeats Doom's random lump access pattern.

The helper script is:

```bash
./compress-wad.sh wads/doom2.wad wads/doom2-compressed.wad
```

It invokes `build/wadcompress`, rebuilding that tool when needed, and refuses
to overwrite an existing output file.

## 2. Shared metadata: `src/w_wad.h`

Add the 20-byte on-disk entry shown above.  Then add a field to the engine's
resolved lump record:

```c
typedef struct {
  /* existing fields */
  int compressed_size;         /* zero for a regular WAD lump */
} lumpinfo_t;
```

The important invariant is:

```text
compressed_size == 0  => ordinary direct/mapped read path
compressed_size > 0   => zlib inflate path
```

If zero-length compressed lumps are supported, use an explicit per-WAD
`is_zwad` flag as well, because their compressed size is also zero.

## 3. WAD registration: `W_AddFile()` in `src/w_wad.c`

`W_AddFile()` is the normal loader's directory parser.  Port all of these
changes together:

1. Accept `ZWAD` in addition to `IWAD` and `PWAD` when validating the header.
2. Set a local `compressed_wad` boolean from the header identifier.
3. Allocate and read `zwadfilelump_t` entries for ZWAD; use `filelump_t` for
   ordinary files.
4. Populate the normal internal `lumpinfo_t` table in both cases:
   `position`, uncompressed `size`, eight-byte `name`, and WAD source.
5. Copy `compressed_size` for ZWAD entries; initialize it to zero for normal
   entries.
6. Record that a compressed WAD was loaded so the fixed workspace can be
   reported to the memory HUD.

Only directory parsing happens here.  Do not inflate all lumps while building
`lumpinfo_t`; that would turn compressed file storage into a whole-WAD memory
allocation.

All disk integers need the engine's normal little-endian conversion before
they are used as lengths or offsets.

## 4. On-demand inflation: `W_ReadLump()` in `src/w_wad.c`

`W_ReadLump(int lump, void *dest)` is the correct central read point because
both direct reads and cache loads already call it.

For an ordinary lump, preserve the existing seek/read implementation.  For a
ZWAD lump:

1. Seek to `lumpinfo[lump].position`.
2. Initialize a `z_stream` with `inflateInit`.
3. Set `next_out = dest` and `avail_out = lumpinfo[lump].size`.
4. Read compressed input in chunks into one shared static input array.
5. Call `inflate(..., Z_NO_FLUSH)` until it returns `Z_STREAM_END`.
6. Verify that decompression ended cleanly and produced exactly the advertised
   uncompressed size.
7. Call `inflateEnd` on every success and error path.

This implementation uses a single 32 KiB input buffer:

```c
#define ZWAD_INPUT_BUFFER_SIZE 32768
static byte zwad_input_buffer[ZWAD_INPUT_BUFFER_SIZE];
```

The output buffer is not extra memory: it is the destination supplied by the
existing WAD cache.  The 32 KiB array is the only persistent decompression
overhead in the current design.  It is shared, so WAD reading must remain
serialized or the buffer must become per-thread/per-read state.

Never trust the directory blindly.  A port should reject negative offsets and
sizes, check each compressed range against the file size, and reject truncated
or overproducing zlib streams.

## 5. Cache and memory mapping: `src/w_mmap.c`

Ordinary WADs can expose lump data through a memory map.  A compressed lump
cannot: the mapped bytes are zlib input, not the bytes expected by the Doom
renderer, sound loader, or map loader.

Therefore add a compressed-lump cache path, represented here by
`W_CacheCompressedLump()`:

```c
if (lumpinfo[lump].compressed_size > 0) {
  if (!cachelump[lump].cache)
    W_ReadLump(lump,
      Z_Malloc(W_LumpLength(lump), PU_CACHE, &cachelump[lump].cache));
  return cachelump[lump].cache;
}
```

The essential behavior is:

- Allocate the uncompressed lump in the **existing zone cache** on first use.
- Call the new `W_ReadLump()` to inflate into that allocation.
- Return the cached uncompressed pointer on later accesses.
- Keep the normal map-backed/direct-cache behavior unchanged for ordinary
  WADs.

Port the conditional to every cache entry point.  In this codebase that means
both `W_CacheLumpNum()` and `W_LockLumpNum()`, including platform-specific
branches.  A common mistake is fixing only `W_CacheLumpNum()` while a renderer
or sound loader uses the lock API; it then receives compressed bytes or uses
the normal mapped-cache lock rules incorrectly.

Compressed cache entries begin unlocked (`locks = 0`).  Mapped normal entries
retain their special lock value (`-1` in this engine).  `W_UnlockLumpNum()`
must avoid decrementing a compressed entry below zero.

The cache allocation is intentionally counted as normal engine memory once a
lump is used.  Compression reduces storage and startup I/O, but it cannot
avoid the engine holding any decoded lump that it actively needs.

## 6. Early game detection: `CheckIWAD()` in `src/d_main.c`

This function was the source of the Doom II launch failure.  It executes
before `W_AddFile()` and reads the WAD directory directly to detect `MAP01`-
`MAP30`, Doom episode maps, and special names.

The old normal-WAD-only logic effectively did this:

```c
require_header("IWAD");
fileinfo = read_directory(sizeof(filelump_t));
scan(fileinfo[i].name);
```

For ZWAD, `sizeof(filelump_t)` is 16, while actual records are 20 bytes.  The
name scan becomes misaligned after record zero, so Doom II `MAPxx` names are
missed and the game later tries to start a Doom `E1M9` demo.

The required change:

```c
dboolean compressed_wad = !strncmp(header.identification, "ZWAD", 4);

if (strncmp(header.identification, "IWAD", 4) && !compressed_wad)
  warn_about_missing_iwad_tag();

if (compressed_wad)
  zwadinfo = read_directory(sizeof(*zwadinfo));
else
  fileinfo = read_directory(sizeof(*fileinfo));

for (each directory entry) {
  const char *name = compressed_wad ? zwadinfo[i].name : fileinfo[i].name;
  scan_existing_game_detection_rules(name);
}
```

No lump decompression is needed in this function because all required
information is in the uncompressed directory.  Free the selected directory
allocation after scanning.  Apply the same repair to any other code that
opens a WAD and reads its directory without calling the main WAD loader
(launchers, setup tools, demo tools, validators, or map browsers).

## 7. Memory monitor integration

The feature exposes:

```c
size_t W_CompressedBufferUsage(void);
```

It returns `32768` while any compressed WAD is active and zero otherwise. The
HUD in [src/hu_stuff.c](../src/hu_stuff.c) displays it separately:

```text
ENGINE <zone/cache total> + ZWAD 32K = <combined total>
```

This makes the memory accounting explicit:

- `ENGINE`: decoded lumps retained by the cache and other engine allocations.
- `ZWAD 32K`: fixed shared zlib input workspace.
- It excludes graphics framebuffers and sound/music buffers, consistent with
  the requested engine-only memory display.

Do not count the compressed WAD file length as RAM unless the new port really
maps or buffers that file.  This implementation reads compressed chunks from
disk, so the main process allocation attributable to ZWAD itself is the 32 KiB
workspace plus the ordinary decoded lump cache.

## 8. Build and launch requirements

The CMake configuration must link zlib and define `HAVE_LIBZ`; otherwise
`W_ReadLump()` should fail clearly when given a ZWAD rather than silently
treating it as raw data.

The launcher [run-prboom.sh](../run-prboom.sh) accepts the WAD path as its
first positional argument and supplies the engine resource WAD through
`DOOMWADPATH`:

```bash
./run-prboom.sh wads/doom2-compressed.wad
./run-prboom.sh wads/doom2-compressed.wad -warp 1 1
```

## Porting verification checklist

1. Compress a known Doom II IWAD with the ported compressor.
2. Start the engine using the compressed path.
3. Confirm startup prints `playing: DOOM 2: Hell on Earth`.
4. Confirm it reaches `W_InitCache` and `R_Init` without `Unknown Game
   Version` or a lookup for `d_e1m9`.
5. Enter at least one level, test textures/sprites/flats, music, and sound;
   these touch different lump access paths.
6. Check the HUD reports the ZWAD workspace separately from engine cache
   memory.
7. Run the same tests with an ordinary IWAD to confirm no regression in the
   direct/mapped path.

## Common failure modes

| Symptom | Cause | Fix |
| --- | --- | --- |
| `IWAD tag ... not present` | Header validation accepts only `IWAD`. | Accept `ZWAD` in all WAD validators. |
| `Unknown Game Version`, then `d_e1m9 not found` for Doom II | Early detector read 20-byte ZWAD records with a 16-byte stride. | Update `CheckIWAD()` or equivalent to select the directory type. |
| Corrupt texture/map/audio data | Compressed bytes were returned as if they were lump bytes. | Route ZWAD reads through `inflate` in the central read function. |
| Crash or invalid pointer in renderer | Mapped-file cache path used for a compressed lump. | Allocate/inflate compressed lumps into the normal zone cache. |
| Memory unexpectedly equals full decoded WAD | All lumps are inflated at startup or into a global buffer. | Decompress only on first cache request, using the existing cache destination. |
| Intermittent corrupted data in a threaded port | Shared 32 KiB buffer is used concurrently. | Serialize reads or give each worker an independent input buffer. |

