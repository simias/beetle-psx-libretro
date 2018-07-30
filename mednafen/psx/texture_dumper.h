#ifndef __MDFN_PSX_TEXTURE_DUMPER_H
#define __MDFN_PSX_TEXTURE_DUMPER_H

#include <cstdint>
#include "../../rsx/rsx_intf.h" /* for blending_modes */

class PS_GPU;

class TextureDumper {
private:
   bool enabled;
   bool dump_texture_16bpp;
   bool dump_texture_page;
   bool dump_texture_poly;
   bool blend;
   char *dump_dir;
   unsigned count;

   struct table_entry_t {
      uint32_t capacity;
      uint32_t len;
      uint32_t hashes[];
   };

   table_entry_t **tex_hash_table;

   bool hash_table_insert(uint32_t hash);

   uint32_t checksum_area(PS_GPU *gpu,
                          unsigned page_x,
                          unsigned u_start, unsigned u_end,
                          unsigned page_y,
                          unsigned v_start, unsigned v_end,
                          uint16_t clut_x, uint16_t clut_y,
                          unsigned depth_shift,
                          enum blending_modes blend_mode);

   void dump_area(PS_GPU *gpu,
                  unsigned page_x,
                  unsigned u_start, unsigned u_end,
                  unsigned page_y,
                  unsigned v_start, unsigned v_end,
                  uint16_t clut_x, uint16_t clut_y,
                  unsigned depth_shift,
                  enum blending_modes blend_mode,
                  uint32_t hash);

public:
   TextureDumper();
   ~TextureDumper();

   bool is_enabled() const {
      return this->enabled;
   }

   void enable(bool en);

   void set_dump_dir(const char *dir);

   void set_dump_config(bool dump_16bpp, bool dump_page,
                        bool dump_poly, bool preserve_blend);

   void dump(PS_GPU *gpu,
             unsigned u_start, unsigned u_end,
             unsigned v_start, unsigned v_end,
             uint16_t clut_x, uint16_t clut_y,
             unsigned depth_shift,
             enum blending_modes blend_mode);
};

#endif // __MDFN_PSX_TEXTURE_DUMPER_H
