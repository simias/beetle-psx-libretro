// PlayStation OpenGL 3.3 renderer playing nice with libretro
#ifndef RETROGL_H
#define RETROGL_H

#include "../libretro.h"
#include <gl>

#include "../renderer/GlRenderer.h"

class RetroGl {
public:
	GlState state;
	VideoClock clock;

};

#endif