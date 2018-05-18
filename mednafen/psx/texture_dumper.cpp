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
    dump_texture_16bpp(true),
    dump_texture_page(true),
    dump_texture_poly(true),
    blend(true)
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
                         unsigned depth_shift,
                         enum blending_modes blend_mode)
{
   uint32_t page_x = gpu->TexPageX;
   uint32_t page_y = gpu->TexPageY;
   bool poly_unique = false;
   uint32_t poly_hash = 0;
   bool page_unique = false;
   uint32_t page_hash = 0;


   if (!this->blend) {
      blend_mode = BLEND_MODE_OPAQUE;
   }

   if (!dump_texture_16bpp && depth_shift == DEPTH_SHIFT_16BPP) {
      /* Ignore */
      return;
   }

   u_start += page_x;
   u_end   += page_x;
   v_start += page_y;
   v_end   += page_y;

   /* Here's the logic for the checksumming:
    *
    * - Polygon dumps:
    *
    * That's simple: we dump the polygon if the checksum of the
    * bounding rectangle of the texture is unique.
    *
    * - Page dumps:
    *
    * That's a little more complicated.
    *
    * At first I tried dumping the page if the checksum of the entire
    * page was new. Unfortunately that doesn't work well when the
    * texture page overlaps a framebuffer or other fast-changing zone
    * of VRAM which happens to be pretty common. In this situation the
    * page gets dumped repeatedly even though the portion that
    * actually contains texture dosn't change.
    *
    * My 2nd attempt was to only checksum the polygon (like for the
    * polygon dump above). If the polygon texture's checksum is new
    * we dump the whole page. This solves the issue of texture pages
    * overlapping the framebuffer but it can still result in a bunch
    * of unnecessary duplicate dumps because if different polygons
    * sample different textures from the *same* page (with the same
    * CLUT etc...) the polygon checksum will be different even though
    * the page is exactly the same.
    *
    * Therefore final solution is to do both checks: we checksum the
    * polygon texture and if it's new we checksum the whole page to
    * figure out if we haven't yet dumped it before.
    */
   if (dump_texture_page || dump_texture_poly) {
      poly_hash = checksum_area(gpu,
                                u_start, u_end,
                                v_start, v_end,
                                clut_x, clut_y,
                                depth_shift, blend_mode);

      poly_unique = hash_table_insert(poly_hash);

      if (dump_texture_page && poly_unique) {
         page_hash = checksum_area(gpu,
                                   page_x, page_x + 0xff,
                                   page_y, page_y + 0xff,
                                   clut_x, clut_y,
                                   depth_shift, blend_mode);
         page_unique = hash_table_insert(page_hash);
      }
   }

   if (dump_texture_page && page_unique) {
      dump_area(gpu,
                page_x, page_x + 0xff,
                page_y, page_y + 0xff,
                clut_x, clut_y,
                depth_shift, blend_mode,
                page_hash);
   }

   if (dump_texture_poly && poly_unique) {
      /* Ignore textures if they're too small */
      if (u_end - u_start > 4 || v_end - v_start > 4) {
         dump_area(gpu,
                   u_start, u_end,
                   v_start, v_end,
                   clut_x, clut_y,
                   depth_shift, blend_mode,
                   poly_hash);
      }
   }

}

uint32_t TextureDumper::checksum_area(PS_GPU *gpu,
                                      unsigned u_start, unsigned u_end,
                                      unsigned v_start, unsigned v_end,
                                      uint16_t clut_x, uint16_t clut_y,
                                      unsigned depth_shift,
                                      enum blending_modes blend_mode)
{
   uint32_t hash = djb2_init();
   unsigned clut_width;

   switch (depth_shift) {
   case DEPTH_SHIFT_4BPP:
      clut_width = 16;
      break;
   case DEPTH_SHIFT_8BPP:
      clut_width = 256;
      break;
   case DEPTH_SHIFT_16BPP:
      clut_width = 0;
      break;
   }

   // Checksum blend mode
   djb2_update(&hash, (uint8_t)blend_mode);

   // Checksum CLUT (if any)
   for (unsigned x = clut_x; x < clut_x + clut_width; x++) {
      uint16_t t = texel_fetch(gpu, x, clut_y);

      djb2_update(&hash, t);
   }

   unsigned width = u_end - u_start + 1;
   unsigned height = v_end - v_start + 1;
   unsigned width_vram = width >> depth_shift;

   // Checksum texture data
   for (unsigned y = 0; y < height ; y ++) {
      for (unsigned x = 0; x < width_vram; x ++) {
         uint16_t t = texel_fetch(gpu, u_start + x, v_start + y);

         djb2_update(&hash, t);
      }
   }

   return hash;
}

static inline uint8_t bpp_5to8(uint8_t v) {
   return (v << 3) | (v >> 2);
}

static inline void write_col_1555_BGRA8888(uint8_t *buf,
                                           uint16_t col,
                                           enum blending_modes blend_mode) {
   if (col != 0) {
      bool semi_transp = ((col >> 15) != 0);
      uint8_t b = bpp_5to8((col >> 10) & 0x1f);
      uint8_t g = bpp_5to8((col >> 5) & 0x1f);
      uint8_t r = bpp_5to8(col & 0x1f);

      buf[0] = b;
      buf[1] = g;
      buf[2] = r;
      buf[3] = 0xff;
   } else {
      /* Transparent pixel */
      buf[0] = 0;
      buf[1] = 0;
      buf[2] = 0;
      buf[3] = 0;
   }
}

void TextureDumper::dump_area(PS_GPU *gpu,
                              unsigned u_start, unsigned u_end,
                              unsigned v_start, unsigned v_end,
                              uint16_t clut_x, uint16_t clut_y,
                              unsigned depth_shift,
                              enum blending_modes blend_mode,
                              uint32_t hash)
{
   unsigned width = u_end - u_start + 1;
   unsigned height = v_end - v_start + 1;
   unsigned width_vram = width >> depth_shift;
   unsigned clut_width;
   unsigned val_width;
   bool paletted = true;

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
      break;
   }

   /* Dump the full page */
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

         write_col_1555_BGRA8888(buf + index, t, blend_mode);
         index += 4;
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
      // Dump "truecolor" data
      uint8_t buf[256 * 4];
      unsigned index = 0;

      for (unsigned dy = 0; dy < height ; dy++) {
         for (unsigned dx = 0; dx < width; dx++) {
            unsigned x = dx;
            unsigned y = height - dy - 1;

            uint16_t t = texel_fetch(gpu,
                                     u_start + x,
                                     v_start + y);

            write_col_1555_BGRA8888(buf + index, t, blend_mode);
            index += 4;

            if (index == sizeof(buf)) {
               write(fd, buf, index);
               index = 0;
            }
         }
      }
      write(fd, buf, index);
   }

   close(fd);
}
