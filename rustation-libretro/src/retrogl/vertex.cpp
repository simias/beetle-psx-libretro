#include "vertex.h"

#include <string.h> // strcpy()
#include <assert.h>

VertexArrayObject::VertexArrayObject()
{
    GLuint id = 0;
    rglGenVertexArrays(1, &id);

    assert( !rglGetError() );

    this->id = id;
}

void VertexArrayObject::bind()
{
    rglBindVertexArray(this->id);
}

void VertexArrayObject::drop()
{
    rglDeleteBuffers(1, &this->id);
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
