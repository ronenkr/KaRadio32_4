/*
 * wadcompress - turns an ordinary IWAD/PWAD into a ZWAD: every non-empty
 * lump becomes an independent zlib stream, so the ESP32 engine (see
 * ../components/prboom/w_wad.c) can inflate just the one lump it needs
 * instead of expanding the whole WAD into RAM. Full format description:
 * ../zwad-compressed-wad-porting.md.
 *
 * This is a host-side build tool only - it links the system zlib and is
 * never compiled into the firmware. The firmware's own decompressor is the
 * vendored tinfl component (../components/tinfl), not this file.
 *
 * Usage: wadcompress <input.wad> <output.zwad>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#pragma pack(push, 1)
typedef struct {
  char identification[4];
  int32_t numlumps;
  int32_t infotableofs;
} wadinfo_t;

typedef struct {
  int32_t filepos;
  int32_t size;
  char name[8];
} filelump_t;

typedef struct {
  int32_t filepos;
  int32_t size;
  int32_t compressed_size;
  char name[8];
} zwadfilelump_t;
#pragma pack(pop)

static void die(const char *msg, const char *arg)
{
  fprintf(stderr, "wadcompress: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
  exit(1);
}

int main(int argc, char **argv)
{
  if (argc != 3) {
    fprintf(stderr, "usage: %s <input.wad> <output.zwad>\n", argv[0]);
    return 1;
  }

  const char *in_path = argv[1];
  const char *out_path = argv[2];

  FILE *in = fopen(in_path, "rb");
  if (!in)
    die("can't open input", in_path);

  wadinfo_t header;
  if (fread(&header, sizeof(header), 1, in) != 1)
    die("can't read header from", in_path);

  if (strncmp(header.identification, "IWAD", 4) && strncmp(header.identification, "PWAD", 4))
    die("not an IWAD/PWAD", in_path);

  int numlumps = header.numlumps;
  if (numlumps <= 0)
    die("WAD has no lumps", in_path);

  filelump_t *dir = malloc(sizeof(filelump_t) * (size_t)numlumps);
  if (!dir)
    die("out of memory reading directory", NULL);

  if (fseek(in, header.infotableofs, SEEK_SET) != 0 ||
      fread(dir, sizeof(filelump_t), (size_t)numlumps, in) != (size_t)numlumps)
    die("can't read directory from", in_path);

  // Refuse to clobber an existing output file (matches compress-wad.sh's
  // own check, but guards direct invocation of this binary too).
  FILE *probe = fopen(out_path, "rb");
  if (probe) {
    fclose(probe);
    die("output already exists, refusing to overwrite", out_path);
  }

  FILE *out = fopen(out_path, "wb");
  if (!out)
    die("can't create output", out_path);

  // Header goes last (we don't know the final directory offset until every
  // lump's compressed size is known), but reserve its space now.
  wadinfo_t out_header;
  memset(&out_header, 0, sizeof(out_header));
  if (fwrite(&out_header, sizeof(out_header), 1, out) != 1)
    die("write failed on", out_path);

  zwadfilelump_t *out_dir = malloc(sizeof(zwadfilelump_t) * (size_t)numlumps);
  if (!out_dir)
    die("out of memory building output directory", NULL);

  unsigned char *raw_buf = NULL, *comp_buf = NULL;
  size_t raw_cap = 0;
  uLong comp_cap = 0;

  for (int i = 0; i < numlumps; i++) {
    int32_t size = dir[i].size;
    if (size < 0)
      die("negative lump size in", in_path);

    out_dir[i].filepos = (int32_t)ftell(out);
    out_dir[i].size = size;
    memcpy(out_dir[i].name, dir[i].name, 8);

    if (size == 0) {
      // No stream stored for an empty lump - the directory name alone is
      // enough for callers (e.g. CheckIWAD()) that only need to identify
      // maps without inflating anything.
      out_dir[i].compressed_size = 0;
      continue;
    }

    if ((size_t)size > raw_cap) {
      raw_cap = (size_t)size;
      raw_buf = realloc(raw_buf, raw_cap);
      if (!raw_buf)
        die("out of memory reading lump", dir[i].name);
    }

    if (fseek(in, dir[i].filepos, SEEK_SET) != 0 ||
        fread(raw_buf, 1, (size_t)size, in) != (size_t)size)
      die("can't read lump from", in_path);

    uLong bound = compressBound((uLong)size);
    if (bound > comp_cap) {
      comp_cap = bound;
      comp_buf = realloc(comp_buf, comp_cap);
      if (!comp_buf)
        die("out of memory compressing lump", dir[i].name);
    }

    uLongf comp_len = comp_cap;
    // compress2() writes a standard zlib stream (2-byte header + deflate
    // data + 4-byte Adler-32 trailer) - exactly what the on-device tinfl
    // decompressor expects with TINFL_FLAG_PARSE_ZLIB_HEADER set.
    int zret = compress2(comp_buf, &comp_len, raw_buf, (uLong)size, Z_BEST_COMPRESSION);
    if (zret != Z_OK)
      die("zlib compress2 failed on lump", dir[i].name);

    if (fwrite(comp_buf, 1, comp_len, out) != comp_len)
      die("write failed on", out_path);

    out_dir[i].compressed_size = (int32_t)comp_len;
  }

  free(raw_buf);
  free(comp_buf);

  int32_t dir_offset = (int32_t)ftell(out);
  if (fwrite(out_dir, sizeof(zwadfilelump_t), (size_t)numlumps, out) != (size_t)numlumps)
    die("write failed on", out_path);

  memcpy(out_header.identification, "ZWAD", 4);
  out_header.numlumps = numlumps;
  out_header.infotableofs = dir_offset;

  if (fseek(out, 0, SEEK_SET) != 0 ||
      fwrite(&out_header, sizeof(out_header), 1, out) != 1)
    die("can't rewrite header on", out_path);

  fclose(out);
  fclose(in);
  free(dir);
  free(out_dir);

  printf("wadcompress: %s -> %s (%d lumps)\n", in_path, out_path, numlumps);
  return 0;
}
