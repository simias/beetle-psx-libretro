#ifndef RETROGL_TEXTURE_H
#define RETROGL_TEXTURE_H

#include <glsm/glsmsym.h>
#include <stdint.h>

class Texture {
public:
    GLuint id;
    uint32_t width;
    uint32_t height;

    Texture(uint32_t width, uint32_t height, GLenum internal_format);
    void bind(GLenum texture_unit);
    GLenum set_sub_image(   uint16_t top_left[2],
                            uint16_t resolution[2],
                            GLenum format,
                            GLenum ty,
                            T** data);
};

#endif
