#ifndef RETROGL_FRAMEBUFFER_H
#define RETROGL_FRAMEBUFFER_H

#include "../glmsym.h"
#include "texture.h"

class Framebuffer {
public:
    GLuint id;
    Texture* _color_texture;

    Framebuffer(Texture* color_texture);
    Framebuffer(Texture* color_texture, Texture* depth_texture);
    ~Framebuffer();

    void bind();
    void drop();
};

#endif
