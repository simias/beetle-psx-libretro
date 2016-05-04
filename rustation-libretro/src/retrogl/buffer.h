#ifndef RETROGL_BUFFER_H
#define RETROGL_BUFFER_H

#include <stdlib.h> // size_t
#include <stdint.h>
#include <glsm/glsmsym.h>

#include "vertex.h"
#include "program.h"
/* #include "types.h" */

template<typename T>
class DrawBuffer 
{
public:
    /// OpenGL name for this buffer
    GLuint id;
    /// Vertex Array Object containing the bindings for this
    /// buffer. I'm assuming that each VAO will only use a single
    /// buffer for simplicity.
    VertexArrayObject* vao;
    /// Program used to draw this buffer
    Program* program;
    /// Number of elements T that the vertex buffer can hold
    size_t capacity;
    /// Marker for the type of our buffer's contents
    /* PhantomData<T> contains; */
    T* contains;
    /// Current number of entries in the buffer
    size_t len;
    /// If true newer items are added *before* older ones
    /// (i.e. they'll be drawn first)
    bool lifo;

    /* 
    pub fn new(capacity: usize,
               program: Program,
               lifo: bool) -> Result<DrawBuffer<T>, Error> {
    */
    DrawBuffer(size_t capacity, Program* program, bool lifo);
    ~DrawBuffer();

    /* fn bind_attributes(&self)-> Result<(), Error> { */
    GLenum bind_attributes(); 
    GLenum enable_attribute(const char* attr);
    GLenum disable_attribute(const char* attr);
    bool empty();

    /* impl<T> DrawBuffer<T> { */
    GLenum clear();
    /// Bind the buffer to the current VAO
    void bind();
    GLenum push_slice(T slice[], size_t n);
    void draw(GLenum mode);
    size_t remaining_capacity();

    /* impl<T> Drop for DrawBuffer<T> { */
    void drop();
};

#endif
