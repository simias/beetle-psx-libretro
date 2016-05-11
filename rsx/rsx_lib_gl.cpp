#include "rsx_lib_gl.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h> /* exit() */

#include <boolean.h>

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
#include <glsm/glsm.h>
#endif

static retro_video_refresh_t rsx_gl_video_cb;
static retro_environment_t   rsx_gl_environ_cb;
extern uint8_t widescreen_hack;
extern uint8_t psx_gpu_upscale_shift;

static RetroGl* static_renderer; 


/* Already defined in rustation-libretro/GlRenderer.h */
#if 0
/* Width of the VRAM in 16bit pixels */
static const uint16_t VRAM_WIDTH_PIXELS = 1024;

/* Height of the VRAM in lines */
static const uint16_t VRAM_HEIGHT = 512;
#endif

static bool rsx_gl_is_pal = false;

/* The are a few hardware differences between PAL and NTSC consoles,
 * in particular the pixelclock runs slightly slower on PAL consoles. */

/* The translated code already has an enum class named VideoClock */
#if 0
enum VideoClock
{
   Ntsc,
   Pal,
};
#endif

static bool fb_ready = false;

static void context_reset(void)
{
   printf("context_reset.\n");
   glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);

   if (!glsm_ctl(GLSM_CTL_STATE_SETUP, NULL))
      return;

   fb_ready = true;
}

static void context_destroy(void)
{
}

static bool context_framebuffer_lock(void *data)
{
   if (fb_ready)
      return false;
   return true;
}

void renderer_gl_free(void)
{
#if 0
   if !static_renderer.is_null()
   {
      let _ = Box::from_raw(static_renderer);
      static_renderer = ptr::null_mut();
   }
#endif
}

RetroGl* maybe_renderer()
{
  return static_renderer;
}
RetroGl* renderer()
{
  RetroGl* r = maybe_renderer();
  if (r != nullptr) {
    return r;
  } else {
    printf("Attempted to use a NULL renderer\n");
    exit(EXIT_FAILURE);
  }
}

void set_renderer(RetroGl* renderer)
{
  static_renderer = renderer;
}

void drop_renderer()
{
  static_renderer = nullptr;  
}

void rsx_gl_init(void)
{
#if 0
   static mut first_init: bool = true;

   unsafe {
      if first_init {
         retrolog::init();
         first_init = false;
      }
   }
#endif
}

bool rsx_gl_open(bool is_pal)
{
   glsm_ctx_params_t params = {0};

   params.context_reset         = context_reset;
   params.context_destroy       = context_destroy;
   params.environ_cb            = rsx_gl_environ_cb;
   params.stencil               = false;
   params.imm_vbo_draw          = NULL;
   params.imm_vbo_disable       = NULL;
   params.framebuffer_lock      = context_framebuffer_lock;

   if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
      return false;

   rsx_gl_is_pal = is_pal;

   VideoClock clock = is_pal ? VideoClock::Pal : VideoClock::Ntsc;
   set_renderer( RetroGl::getInstance(clock) );

   return true;
}

void rsx_gl_close(void)
{
    drop_renderer();
}

void rsx_gl_refresh_variables(void)
{
    if (static_renderer != nullptr) {
      static_renderer->refresh_variables();
    }
}

void rsx_gl_prepare_frame(void)
{
   if (!fb_ready)
      return;

   glsm_ctl(GLSM_CTL_STATE_BIND, NULL);

   renderer()->prepare_render();
}

void rsx_gl_finalize_frame(const void *fb, unsigned width,
      unsigned height, unsigned pitch)
{
   if (!fb_ready)
      return;
  
   rsx_gl_video_cb(RETRO_HW_FRAME_BUFFER_VALID,
         width, height, pitch);

   glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);

   renderer()->finalize_frame();

}

void rsx_gl_set_environment(retro_environment_t callback)
{
   rsx_gl_environ_cb = callback;
#if 0
    libretro::set_environment(callback);
#endif
}

void rsx_gl_set_video_refresh(retro_video_refresh_t callback)
{
   rsx_gl_video_cb = callback;
#if 0
   libretro::set_video_refresh(callback);
#endif
}

/* Precise FPS values for the video output for the given
 * VideoClock. It's actually possible to configure the PlayStation GPU
 * to output with NTSC timings with the PAL clock (and vice-versa)
 * which would make this code invalid but it wouldn't make a lot of
 * sense for a game to do that.
 */
static float video_output_framerate(void)
{
   /* NTSC - 53.690MHz GPU clock frequency, 263 lines per field,
    * 3413 cycles per line */
   return rsx_gl_is_pal ? 49.76 : 59.81;
}

void rsx_gl_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = video_output_framerate();
   info->timing.sample_rate    = 44100;
   info->geometry.base_width   = MEDNAFEN_CORE_GEOMETRY_BASE_W << psx_gpu_upscale_shift;
   info->geometry.base_height  = MEDNAFEN_CORE_GEOMETRY_BASE_H << psx_gpu_upscale_shift;
   info->geometry.max_width    = MEDNAFEN_CORE_GEOMETRY_MAX_W << psx_gpu_upscale_shift;
   info->geometry.max_height   = MEDNAFEN_CORE_GEOMETRY_MAX_H << psx_gpu_upscale_shift;
   info->geometry.aspect_ratio = !widescreen_hack ? MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO : (float)16/9;
#if 0
    struct retro_system_av_info info = renderer->get_system_av_info();
#endif
}

/* Draw commands */

void rsx_gl_set_draw_offset(int16_t x, int16_t y)
{
   renderer()->gl_renderer()->set_draw_offset(x, y);
}

void  rsx_gl_set_draw_area(uint16_t x,
      uint16_t y,
      uint16_t w,
      uint16_t h)
{
   uint16_t top_left[2]   = {x, y};
   uint16_t dimensions[2] = {w, h};
   renderer()->gl_renderer()->set_draw_area(top_left, dimensions);
}

void rsx_gl_set_display_mode(uint16_t x,
      uint16_t y,
      uint16_t w,
      uint16_t h,
      bool depth_24bpp)
{
   uint16_t top_left[2]   = {x, y};
   uint16_t dimensions[2] = {w, h};
   renderer()->gl_renderer()->set_display_mode(top_left, dimensions, depth_24bpp);
}

void rsx_gl_push_triangle(
      int16_t p0x,
      int16_t p0y,
      int16_t p1x,
      int16_t p1y,
      int16_t p2x,
      int16_t p2y,
      uint32_t c0,
      uint32_t c1,
      uint32_t c2,
      uint16_t t0x,
      uint16_t t0y,
      uint16_t t1x,
      uint16_t t1y,
      uint16_t t2x,
      uint16_t t2y,
      uint16_t texpage_x,
      uint16_t texpage_y,
      uint16_t clut_x,
      uint16_t clut_y,
      uint8_t texture_blend_mode,
      uint8_t depth_shift,
      bool dither,
      int blend_mode)
{
   SemiTransparencyMode semi_transparency_mode = SemiTransparencyMode::Add;
   bool semi_transparent = false;
   switch (blend_mode) {
   case -1:
      semi_transparent = false;
      semi_transparency_mode = SemiTransparencyMode::Add;
      break;
   case 0:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode::Average;
      break;
   case 1:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode::Add;
      break;
   case 2:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode::SubtractSource;
      break;
   case 3:
      semi_transparent = true;
      semi_transparency_mode = SemiTransparencyMode::AddQuarterSource;
      break;
   default:
      exit(EXIT_FAILURE);
   }

   CommandVertex v[3] = 
   {
      {
          {p0x, p0y},   /* position */
          {(uint8_t) c0, (uint8_t) (c0 >> 8), (uint8_t) (c0 >> 16)}, /* color */
          {t0x, t0y},   /* texture_coord */
          {texpage_x, texpage_y}, 
          {clut_x, clut_y},         
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither
      },
      {
          {p1x, p1y}, /* position */
          {(uint8_t) c1, (uint8_t) (c1 >> 8), (uint8_t) (c1 >> 16)}, /* color */
          {t1x, t1y}, /* texture_coord */
          {texpage_x, texpage_y}, 
          {clut_x, clut_y},   
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither
      },
      {
          {p2x, p2y}, /* position */
          {(uint8_t) c2, (uint8_t) (c2 >> 8), (uint8_t) (c2 >> 16)}, /* color */
          {t2x, t2y}, /* texture_coord */
          {texpage_x, texpage_y}, 
          {clut_x, clut_y},   
          texture_blend_mode,
          depth_shift,
          (uint8_t) dither
      }
   };

   renderer()->gl_renderer()->push_triangle(v, semi_transparency_mode);
}

void rsx_gl_fill_rect(uint32_t color,
      uint16_t x, uint16_t y,
      uint16_t w, uint16_t h)
{

   uint16_t top_left[2]   = {x, y};
   uint16_t dimensions[2] = {w, h};
   uint8_t col[3] = {(uint8_t) color, (uint8_t) (color >> 8), (uint8_t) (color >> 16)};  

   renderer()->gl_renderer()->fill_rect(col, top_left, dimensions);
}

void rsx_gl_copy_rect(
      uint16_t src_x, uint16_t src_y,
      uint16_t dst_x, uint16_t dst_y,
      uint16_t w, uint16_t h)
{
    uint16_t src_pos[2] = {src_x, src_y};
    uint16_t dst_pos[2] = {dst_x, dst_y};
    uint16_t dimensions[2] = {w, h}; 

    renderer()->gl_renderer()->copy_rect(src_pos, dst_pos, dimensions);
}

void rsx_gl_push_line(int16_t p0x,
      int16_t p0y,
      int16_t p1x,
      int16_t p1y,
      uint32_t c0,
      uint32_t c1,
      bool dither,
      int blend_mode)
{
   CommandVertex v[2] = {
      {
          {p0x, p0y}, /* position */
          {(uint8_t) c0, (uint8_t) (c0 >> 8), (uint8_t) (c0 >> 16)}, /* color */
          {0, 0}, /* texture_coord */
          {0, 0}, /* texture_page */
          {0, 0}, /* clut */
          0,      /* texture_blend_mode */
          0,      /* depth_shift */
          (uint8_t) dither
      },
      {
          {p1x, p1y}, /* position */
          {(uint8_t) c1, (uint8_t) (c1 >> 8), (uint8_t) (c1 >> 16)}, /* color */
          {0, 0}, /* texture_coord */
          {0, 0}, /* texture_page */
          {0, 0}, /* clut */
          0,      /* texture_blend_mode */
          0,      /* depth_shift */
          (uint8_t) dither
      }
   };

   renderer()->gl_renderer()->push_line(v, SemiTransparencyMode::Add);
}

void rsx_gl_load_image(uint16_t x, uint16_t y,
      uint16_t w, uint16_t h,
      uint16_t *vram)
{
   uint16_t top_left[2]   = {x, y};
   uint16_t dimensions[2] = {w, h};

   /* TODO FIXME - upload_vram_window expects a 
  uint16_t[VRAM_HEIGHT*VRAM_WIDTH_PIXELS] array arg instead of a ptr */
   renderer()->gl_renderer()->upload_vram_window(top_left, dimensions, vram);
}

void rsx_gl_set_blend_mode(enum blending_modes mode)
{
}


