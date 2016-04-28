#ifndef RETROGL_BUFFER_H
#define RETROGL_BUFFER_H

#include <gl>
#include <stdlib.h> // size_t

#include "error.h"
#include "vertex.h"
#include "program.h"
#include "types.h"

template<typename T>
class DrawBuffer<T> 
{
public:
    /// OpenGL name for this buffer
    GLuint id;
    /// Vertex Array Object containing the bindings for this
    /// buffer. I'm assuming that each VAO will only use a single
    /// buffer for simplicity.
    VertexArrayObject vao;
    /// Program used to draw this buffer
    Program program;
    // Number of elements T that the vertex buffer can hold
    size_t capacity;
    /// Marker for the type of our buffer's contents
    /* How do I simulate this in C++? Maybe an array of size 1? */
    PhantomData<T> contains;

};


#endif
