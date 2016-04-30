#include <stdlib.h> // exit()

#include "framebuffer.h"

Framebuffer::Framebuffer(Texture* color_texture)
{
    GLuint id = 0;
    glGenFramebuffers(1, &id);

    this->id = id;
    this->_color_texture = color_texture;

    this->bind();

    glFramebufferTexture(   GL_DRAW_FRAMEBUFFER,
                            GL_COLOR_ATTACHMENT0,
                            color_texture.id(),
                            0);

    glDrawBuffers(1, &GL_COLOR_ATTACHMENT0);
    glViewport( 0,
                0,
                (GLsizei) color_texture.width(),
                (GLsizei) color_texture.height());

    /* error_or(fb) */
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("GL error %d\n", (int) error);
        exit(EXIT_FAILURE);
    }
}

Framebuffer::Framebuffer(Texture* color_texture, Texture* depth_texture)
{
    /* C++11 delegating constructor */
    Framebuffer(color_texture);

    glFramebufferTexture(   GL_DRAW_FRAMEBUFFER,
                            GL_DEPTH_ATTACHMENT,
                            depth_texture.id(),
                            0);

    /* error_or(fb) */
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("GL error %d\n", (int) error);
        exit(EXIT_FAILURE);
    }

    /* Framebuffer owns this Texture*, make sure we clean it after being done */
    if (depth_texture != nullptr) delete depth_texture;
}

Framebuffer::~Framebuffer()
{
    if (this->_color_texture != nullptr) delete this->_color_texture;
}

void Framebuffer::bind()
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, this->id);
}

void Framebuffer::drop()
{
    glDeleteFramebuffers(1, &this->id);
}


