#include "texture.h"

#include <stdlib.h>
#include <stdio.h>

Texture::Texture(uint32_t width, uint32_t height, GLenum internal_format)
{
    GLuint id = 0;

    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexStorage2D( GL_TEXTURE_2D,
                    1,
                    internal_format,
                    (GLsizei) width,
                    (GLsizei) height);

    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("GL error %d", (int) error);
        exit(EXIT_FAILURE);
    }

    this->id = id;
    this->width = width;
    this->height = height;
}

void Texture::bind(GLenum texture_unit)
{
    glActiveTexture(texture_unit);
    glBindTexture(GL_TEXTURE_2D, this->id);
}
