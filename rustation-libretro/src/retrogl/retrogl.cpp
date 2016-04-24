#include "retrogl.h"

#include <stdio.h>
#include <stdlib.h>

RetroGl::RetroGl(VideoClock video_clock)
{
	// TODO: Is bool set_pixel_format() declared by including libretro.h?
	if ( !set_pixel_format(RETRO_PIXEL_FORMAT_XRGB8888) ) {
		puts("Can't set pixel format");
		exit(EXIT_FAILURE);
	}

	// TODO: Is bool hw_context_init() declared by including libretro.h?
	if ( !hw_context_init() ) {
		puts("Failed to init hardware context");
		exit(EXIT_FAILURE);
	}

	// The VRAM's bootup contents are undefined
	int16_t vram[VRAM_PIXELS];
	size_t i;
	for (i = 0; i < VRAM_PIXELS; ++i)
	{
		vram[i] = 0xdead;
	}

	DrawConfig config = {
		(0, 0),			// display_top_left
        (1024, 512), 	// display_resolution
        false, 			// display_24bpp
        (0, 0), 		// draw_area_top_left
        (0, 0), 		// draw_area_dimensions
        (0, 0), 		// draw_offset
        vram
	};

	this->state(config);
	this->video_clock = video_clock;
}

RetroGl::~RetroGl() {

}

void RetroGl::context_reset() {
	puts("OpenGL context reset");

	// Should I call this at every reset? Does it matter?
	//// TODO: I don't know how to translate this into C++
	/*
	gl::load_with(|s| {
            libretro::hw_context::get_proc_address(s) as *const _
    });
	*/

    //// r5 - I'm not sure what the match self.state is doing or how to
    //// replicate it in C++

    /*
	DrawConfig* config = nullptr;
	let config =
            match self.state {
                GlState::Valid(ref r) => r.draw_config().clone(),
                GlState::Invalid(ref c) => c.clone(),
            };

        match GlRenderer::from_config(config) {
            Ok(r) => self.state = GlState::Valid(r),
            Err(e) => panic!("Couldn't create RetroGL state: {:?}", e),
        }
     */   
}