#include <unistd.h>
#include <fcntl.h>

#include "texture_dumper.h"
#include "psx.h"

bool texture_dump_enabled = true;

#define DEPTH_SHIFT_16BPP  0
#define DEPTH_SHIFT_8BPP   1
#define DEPTH_SHIFT_4BPP   2

#define HASH_TABLE_SIZE 0x10000UL

static inline uint32_t djb2_init(void)
{
   return 5381;
}

static inline void djb2_update(uint32_t *h, uint32_t v)
{
   uint32_t hash = *h;

   hash = ((hash << 5) + hash) + v; /* hash * 33 + c */

   *h = hash;
}

TextureDumper::TextureDumper()
   :enabled(true),
    dump_texture_16bpp(false),
    dump_texture_page(true),
    dump_texture_poly(true)
{
   this->tex_hash_table = new table_entry_t *[HASH_TABLE_SIZE];

   for (unsigned i = 0; i < HASH_TABLE_SIZE; i++) {
      this->tex_hash_table[i] = NULL;
   }
}

TextureDumper::~TextureDumper()
{
   for (unsigned i = 0; i < HASH_TABLE_SIZE; i++) {
      table_entry_t *e = this->tex_hash_table[i];

      if (e != NULL) {
         free(e);
      }
   }

   delete [] this->tex_hash_table;
}

bool TextureDumper::hash_table_insert(uint32_t hash)
{
   size_t index = hash % HASH_TABLE_SIZE;

   table_entry_t *entry = tex_hash_table[index];

   if (entry == NULL) {
      uint32_t capacity = 16;

      entry = (table_entry_t *)malloc(sizeof(table_entry_t) + capacity * sizeof(uint32_t));
      if (entry == NULL) {
         return false;
      }

      tex_hash_table[index] = entry;

      entry->capacity = capacity;
      entry->len = 1;
      entry->hashes[0] = hash;

      return true;
   }

   for (uint32_t i = 0; i < entry->len; i++) {
      if (entry->hashes[i] == hash) {
         // Already in the table
         return false;
      }
   }

   // Not found, insert in the table
   if (entry->capacity == entry->len) {
      uint32_t capacity = entry->capacity * 2;

      entry = (table_entry_t *)realloc(entry,
                                       sizeof(table_entry_t) +
                                       capacity * sizeof(uint32_t));
      if (entry == NULL) {
         return false;
      }

      tex_hash_table[index] = entry;
   }

   entry->hashes[entry->len] = hash;
   entry->len++;
   return true;
}

void TextureDumper::dump(PS_GPU *gpu,
                         unsigned u_start, unsigned u_end,
                         unsigned v_start, unsigned v_end,
                         uint16_t clut_x, uint16_t clut_y,
                         unsigned depth_shift)
{
   uint32_t page_x = gpu->TexPageX;
   uint32_t page_y = gpu->TexPageY;

   if (!dump_texture_16bpp && depth_shift == DEPTH_SHIFT_16BPP) {
      /* Ignore */
      return;
   }

   if (dump_texture_page) {
      dump_area(gpu,
                page_x, page_x + 0xff,
                page_y, page_y + 0xff,
                clut_x, clut_y,
                depth_shift);
   }

   if (dump_texture_poly) {
      dump_area(gpu,
                page_x + u_start, page_x + u_end,
                page_y + v_start, page_y + v_end,
                clut_x, clut_y,
                depth_shift);
   }
}

static inline uint8_t bpp_5to8(uint8_t v) {
   return (v << 3) | (v >> 2);
}

void TextureDumper::dump_area(PS_GPU *gpu,
                              unsigned u_start, unsigned u_end,
                              unsigned v_start, unsigned v_end,
                              uint16_t clut_x, uint16_t clut_y,
                              unsigned depth_shift)
{
   uint32_t hash = djb2_init();
   unsigned clut_width;
   unsigned val_width;
   bool paletted = true;

   unsigned width = u_end - u_start + 1;
   unsigned height = v_end - v_start + 1;
   unsigned width_vram = width >> depth_shift;

   switch (depth_shift) {
   case DEPTH_SHIFT_4BPP:
      clut_width = 16;
      val_width = 4;
      break;
   case DEPTH_SHIFT_8BPP:
      clut_width = 256;
      val_width = 8;
      break;
   case DEPTH_SHIFT_16BPP:
      clut_width = 0;
      val_width = 16;
      paletted = false;
      // XXX implement me
      return;
      break;
   }

   // Checksum CLUT (if any)
   for (unsigned x = clut_x; x < clut_x + clut_width; x++) {
      uint16_t t = texel_fetch(gpu, x, clut_y);

      djb2_update(&hash, t);
   }

   // Checksum pixel data. In order to find a decent compromise
   // between speed and correctness we checksum one in every 4 VRAM
   // "pixels" horizontally and vertically, therefore speeding up the
   // process by a factor of about 16 while still being relatively
   // unlikely of missing a texture change.
   for (unsigned y = 0; y < height ; y += 4) {
      for (unsigned x = 0; x < width_vram; x += 4) {
         uint16_t t = texel_fetch(gpu, u_start + x, v_start + y);

         djb2_update(&hash, t);
      }
   }

   if (!hash_table_insert(hash)) {
      // We already dumped this texture
      return;
   }

   printf("Checksummed new page: %08x\n", hash);

   char filename[128];

   snprintf(filename, sizeof (filename), "%s/dump-%dbpp-%08X.tga", "/tmp", val_width, hash);

   int fd = open (filename, O_WRONLY | O_CREAT, 0644);

   if (fd < 0) {
      return;
   }

   /* TARGA writing code follows */

   uint8_t header[] = {
      // ID len
      0,
      // Color Map Type
      (uint8_t)paletted,
      // Image type
      paletted ? (uint8_t)1 : (uint8_t)2,
      // Color map first entry index
      0,
      0,
      // Color map length
      (uint8_t)clut_width,
      (uint8_t)(clut_width >> 8),
      // Color map entry size
      paletted ? (uint8_t)32 : (uint8_t)0,
      // X origin
      0,
      0,
      // Y origin
      0,
      0,
      // Image width
      (uint8_t)width,
      (uint8_t)(width >> 8),
      // Image height
      (uint8_t)height,
      (uint8_t)(height >> 8),
      // Pixel depth
      paletted ? (uint8_t)8 : (uint8_t)32,
      // Image descriptor
      0,
   };

   write(fd, header, sizeof(header));

   if (paletted) {
      /* Dump the CLUT */
      uint8_t buf[256 * 4];

      unsigned index = 0;

      for (unsigned x = clut_x; x < clut_x + clut_width; x++) {
         uint16_t t = texel_fetch(gpu, x, clut_y);

         if (t == 0) {
            /* Transparent pixel */
            buf[index++] = 0;
            buf[index++] = 0;
            buf[index++] = 0;
            buf[index++] = 0;
         } else {
            /* B */
            buf[index++] = bpp_5to8((t >> 10) & 0x1f);
            /* G */
            buf[index++] = bpp_5to8((t >> 5) & 0x1f);
            /* R */
            buf[index++] = bpp_5to8(t & 0x1f);
            /* A */
            buf[index++] = 0xff;
         }
      }

      write(fd, buf, index);

      /* Dump image data */
      index = 0;
      unsigned val_mask = (1 << val_width) - 1;

      for (unsigned dy = 0; dy < height ; dy++) {
         for (unsigned dx = 0; dx < width; dx++) {
            unsigned x = dx;
            unsigned y = height - dy - 1;

            unsigned t_x = x >> depth_shift;
            unsigned align = x & ((1U << depth_shift) - 1);
            align *= val_width;

            uint16_t t = texel_fetch(gpu,
                                     u_start + t_x,
                                     v_start + y);

            buf[index++] = (t >> align) & val_mask;

            if (index == sizeof(buf)) {
               write(fd, buf, index);
               index = 0;
            }
         }
      }
      write(fd, buf, index);
   } else {
      // Implement me
   }

   close(fd);
}
