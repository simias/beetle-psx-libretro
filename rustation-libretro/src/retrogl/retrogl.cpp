#include "retrogl.h"

#include <stdio.h>
#include <stdlib.h>

RetroGl::RetroGl(VideoClock video_clock)
{
    // TODO: Is bool set_pixel_format() declared by including libretro.h?
    if ( !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, RETRO_PIXEL_FORMAT_XRGB8888) ) {
        puts("Can't set pixel format\n");
        exit(EXIT_FAILURE);
    }

    // TODO: Is bool hw_context_init() declared by including libretro.h?
    if ( !hw_context_init() ) {
        puts("Failed to init hardware context\n");
        exit(EXIT_FAILURE);
    }

    // The VRAM's bootup contents are undefined
    uint16_t vram[VRAM_PIXELS];
    size_t i;
    for (i = 0; i < VRAM_PIXELS; ++i)
    {
        vram[i] = 0xdead;
    }

    DrawConfig config = {
        {0, 0},         // display_top_left
        {1024, 512},    // display_resolution
        false,          // display_24bpp
        {0, 0},         // draw_area_top_left
        {0, 0},         // draw_area_dimensions
        {0, 0},         // draw_offset
        vram
    };

    this->state = GlState::Invalid;
    this->state_data.c = config;
    this->video_clock = video_clock;
}

RetroGl::~RetroGl() {
    if (this->state_data.r != nullptr) delete this->state_data.r;
}

void RetroGl::context_reset() {
    puts("OpenGL context reset\n");

    // Should I call this at every reset? Does it matter?
    //// TODO: I don't know how to translate this into C++
    /*
    gl::load_with(|s| {
            libretro::hw_context::get_proc_address(s) as *const _
    });
    */

    /* Save this on the stack, I'm unsure if saving a ptr would
    would cause trouble because of the 'delete' below  */
    DrawConfig config;

    switch (this->state)
    {
    case GlState::Valid:
        config = *( this->state_data.r.draw_config() );
        break;
    case GlState::Invalid:
        config = *( this->state_data.c );
        break;
    }

    /* TODO - Not checking Ok() or Err() */
    delete this->state_data.r;
    this->state_data.r = new GlRenderer(&config);
    this->state = GlState::Valid;
}

GlRenderer* RetroGl::gl_renderer() 
{
    switch (this->state)
    {
    case GlState::Valid:
        return this->state_data.r;
    case GlState::Invalid:
        puts("Attempted to get GL state without GL context!\n");
        exit(EXIT_FAILURE);
    }
}

void GlRenderer::context_destroy()
{
    puts("OpenGL context destroy\n");

    DrawConfig config;

    switch (this->state)
    {
    case GlState::Valid:
        config = *( this->state_data.r.draw_config() );
        break;
    case GlState::Invalid:
        return;
    }

    this->state = GlState::Invalid;
    this->state_data.c = config;
}

void GlRenderer::prepare_render() 
{
    GlRenderer* renderer = nullptr;
    switch (this->state)
    {
    case GlState::Valid:
        renderer = this->state_data.r;
        break;
    case GlState::Invalid:
        puts("Attempted to render a frame without GL context\n");
        exit(EXIT_FAILURE);
    }

    renderer->prepare_render();
}

void GlRenderer::finalize_frame()
{
    GlRenderer* renderer = nullptr;
    switch (this->state)
    {
    case GlState::Valid:
        renderer = this->state_data.r;
        break;
    case GlState::Invalid:
        puts("Attempted to render a frame without GL context\n");
        exit(EXIT_FAILURE);
    }

    renderer->finalize_frame();
}

void refresh_variables()
{
    GlRenderer* renderer = nullptr;
    switch (this->state)
    {
    case GlState::Valid:
        renderer = this->state_data.r;
        break;
    case GlState::Invalid:
        // Nothing to be done if we don't have a GL context
        return;
    }

    bool reconfigure_frontend = renderer->refresh_variables();
    if (reconfigure_frontend) {
        // The resolution has changed, we must tell the frontend
        // to change its format
        struct retro_variable var = {0};
    
        var.key = "beetle_psx_internal_resolution";
        uint8_t upscaling = 1;
        if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
            /* Same limitations as libretro.cpp */
            upscaling = var.value[0] -'0';
        }

        /* TODO: Is get_av_info declared by libretro.h? 
        Also, check what the namespace operator is doing here,
        what get_av_info is this refering to? CoreVariables?
        */
        struct retro_system_av_info av_info = get_av_info(this->video_clock, upscaling);

        // This call can potentially (but not necessarily) call
        // `context_destroy` and `context_reset` to reinitialize
        // the entire OpenGL context, so beware.
        
        /* The above comment may not be applicable anymore since we're
        calling environ_cb directly.
        TODO - This callback can only be used in retro_run(), what should
        be done instead? 
         */
        bool ok = environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);

        if (!ok)
        {
            puts("Couldn't change frontend resolution\n");
            puts("Try resetting to enable the new configuration\n");
        }
    }
}

struct retro_system_av_info RetroGl::get_system_av_info()
{
    struct retro_variable var = {0};
    
    var.key = "beetle_psx_internal_resolution";
    uint8_t upscaling = 1;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        /* Same limitations as libretro.cpp */
        upscaling = var.value[0] -'0';
    }

    /* What's with the namespace operator behind get_av_info()? */
    struct retro_system_av_info av_info = ::get_av_info(this->video_clock, upscaling);

    return av_info;
}
