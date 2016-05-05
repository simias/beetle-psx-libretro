#include "vertex.h"

#include <stdio.h>
#include <string.h> // strcpy()

VertexArrayObject::VertexArrayObject()
{
    GLuint id = 0;
    glGenVertexArrays(1, &id);

    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("GL error %d\n", (int) error);
        exit(EXIT_FAILURE);
    }

    this->id = id;
}

void VertexArrayObject::bind()
{
    glBindVertexArray(this->id);
}

void VertexArrayObject::drop()
{
    glDeleteBuffers(1, &this->id);
}

Attribute::Attribute(const char* name, size_t offset, GLenum ty, GLint components)
{
    /* Not sure if safe */
    strcpy(this->name, name);
    
    this->offset = offset;
    this->ty = ty;
    this->components = components;
}

const GLvoid* Attribute::gl_offset()
{
    /// For some reason VertexAttribXPointer takes the offset as a
    /// pointer...
    return (void*) &this->offset;
}
