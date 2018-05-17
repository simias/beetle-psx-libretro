#include <unistd.h>
#include <fcntl.h>

#include "texture_dumper.h"
#include "psx.h"

bool texture_dump_enabled = true;
bool dump_texture_page = true;
bool dump_texture_poly = true;
bool dump_16bpp_textures = false;

#define DEPTH_SHIFT_16BPP  0
#define DEPTH_SHIFT_8BPP   1
#define DEPTH_SHIFT_4BPP   2

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

class Tga {
private:
   int fd;

public:
   Tga(const char *path, unsigned width, unsigned height, unsigned depth_shift) {
      bool paletted = (depth_shift != DEPTH_SHIFT_16BPP);
      uint16_t palette_length;

      switch (depth_shift) {
      case DEPTH_SHIFT_4BPP:
         palette_length = 16;
         break;
      case DEPTH_SHIFT_8BPP:
         palette_length = 256;
         break;
      case DEPTH_SHIFT_16BPP:
         palette_length = 0;
         break;
      }

      fd = open (path, O_WRONLY | O_CREAT, 0644);

      if (fd < 0) {
         throw "Can't open TGA file";
      }

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
         (uint8_t)palette_length,
         (uint8_t)(palette_length >> 8),
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

      write(header, sizeof(header));
   }

   ~Tga() {
      close (fd);
   }

   void write(uint8_t *buf, size_t len) {
      ::write (fd, buf, len);
   }
};

TextureDumper::TextureDumper()
   :enabled(true), dump_texture_page(true), dump_texture_poly(true)
{
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

      entry = (table_entry_t *)realloc(entry, sizeof(table_entry_t) + capacity * sizeof(uint32_t));
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

   if (!dump_16bpp_textures && depth_shift == DEPTH_SHIFT_16BPP) {
      /* Ignore */
      return;
   }

   if (dump_texture_page) {
      dump_area(gpu,
                page_x, page_x + (0xff >> depth_shift),
                page_y, page_y + 0xff,
                clut_x, clut_y,
                depth_shift);
   }
}


void TextureDumper::dump_area(PS_GPU *gpu,
                              unsigned u_start, unsigned u_end,
                              unsigned v_start, unsigned v_end,
                              uint16_t clut_x, uint16_t clut_y,
                              unsigned depth_shift)
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
      // XXX implement me
      return;
      break;
   }

   // Checksum CLUT (if any)
   for (unsigned x = clut_x; x <= clut_x + clut_width; x++) {
      uint16_t t = texel_fetch(gpu, x, clut_y);

      djb2_update(&hash, t);
   }

   // Checksum pixel data. In order to find a decent compromise
   // between speed and correctness we checksum one in every 4 VRAM
   // "pixels" horizontally and vertically, therefore speeding up the
   // process by a factor of about 16 while still being relatively
   // unlikely of missing a texture change.
   for (unsigned x = u_start; x <= u_end; x += 4) {
      for (unsigned y = v_start; y <= v_end; y += 4) {
         uint16_t t = texel_fetch(gpu, x, y);

         djb2_update(&hash, t);
      }
   }

   if (!hash_table_insert(hash)) {
      // We already dumped this texture
      return;
   }

   printf("Checksummed new page: %08x\n", hash);
}
