#ifndef __MDFN_PSX_TEXTURE_DUMPER_H
#define __MDFN_PSX_TEXTURE_DUMPER_H

#include <cstdint>

class PS_GPU;

class TextureDumper {
private:
   bool enabled;
   bool dump_texture_16bpp;
   bool dump_texture_page;
   bool dump_texture_poly;

   struct table_entry_t {
      uint32_t capacity;
      uint32_t len;
      uint32_t hashes[];
   };

   table_entry_t **tex_hash_table;

   bool hash_table_insert(uint32_t hash);

   void dump_area(PS_GPU *gpu,
                  unsigned u_cs_start, unsigned u_cs_end,
                  unsigned v_cs_start, unsigned v_cs_end,
                  unsigned u_start, unsigned u_end,
                  unsigned v_start, unsigned v_end,
                  uint16_t clut_x, uint16_t clut_y,
                  unsigned depth_shift);

public:
   TextureDumper();
   ~TextureDumper();

   bool is_enabled() const {
      return this->enabled;
   }

   void enable(bool en);

   void dump(PS_GPU *gpu,
             unsigned u_start, unsigned u_end,
             unsigned v_start, unsigned v_end,
             uint16_t clut_x, uint16_t clut_y,
             unsigned depth_shift);
};

#endif // __MDFN_PSX_TEXTURE_DUMPER_H
