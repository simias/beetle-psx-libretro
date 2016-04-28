#include "buffer.h"

DrawBuffer::DrawBuffer(size_t capacity, Program* program, bool lifo)
{
    VertexArrayObject vao = new VertexArrayObject();

    GLuint id = 0;
    // Generate the buffer object
    glGenBuffers(1, &id);

    this->vao = vao;
    this->program = program;
    this->capacity = capacity;
    this->id = id;
    /* TODO: Are all 'T's classes or structs? */
    this->contains = new T;

    this->clear();
    this->bind_attributes();

    /* 
    if ( error_or(this) != this) {
        exit(EXIT_FAILURE);
    } 
    */
}

DrawBuffer::~DrawBuffer()
{
    if (this->program != nullptr) delete program;
    if (this->T != nullptr) delete T;
}

void DrawBuffer::bind_attributes()
{
    this->vao.bind();

    //ARRAY_BUFFER is captured by VertexAttribPointer
    this->bind();

    /* What is this? Rust stuff? */
    auto attributes = T::attributes();
    GLint element_size = (GLint) sizeof( *(this->contains) );

    /* 
    let index =
                match self.program.find_attribute(attr.name) {
                    Ok(i) => i,
                    // Don't error out if the shader doesn't use this
                    // attribute, it could be caused by shader
                    // optimization if the attribute is unused for
                    // some reason.
                    Err(Error::InvalidValue) => continue,
                    Err(e) => return Err(e),
                };

    */
    for (attr : attributes) {
        /* TODO - I'm not doing any error checking here unlike the code above */
        GLuint index = this->find_attribute(attr.name.c_str());
        glEnableVertexAttribArray(index);

        // This captures the buffer so that we don't have to bind it
        // when we draw later on, we'll just have to bind the vao.
        switch (Kind::from_type(atrib.ty)) {
        /* TODO - Make sure KindEnum exists, Rust allows enums to have funcs  */
        case KindEnum::Integer:
            glEnableVertexAttribIPointer(   index,
                                            attr.components,
                                            attr.ty,
                                            element_size,
                                            attr.gl_offset());
            break;
        case KindEnum::Float:
            glEnableVertexAttribPointer(    index,
                                            attr.components,
                                            attr.ty,
                                            GL_FALSE,
                                            element_size,
                                            attr.gl_offset());
            break;
        case KindEnum::Double:
            glEnableVertexAttribLPointer(   index,
                                            attr.components,
                                            attr.ty,
                                            element_size,
                                            attr.gl_offset());
            break;
        }

    }

    /* get_error() */
}

void DrawBuffer::enable_attribute(const char* attr)
{
    GLuint index = this->find_attribute(attr);
    this->vao.bind();

    glEnableVertexAttribArray(index);

    /* get_error() */
}


void DrawBuffer::disable_attribute(const char* attr)
{
    GLuint index = this->find_attribute(attr);
    this->vao.bind();

    glDisableVertexAttribArray(index);

    /* get_error() */
}

bool DrawBuffer::empty()
{
    return this->len == 0;
}

Program* DrawBuffer::program()
{
    return this->program;
}

/// Orphan the buffer (to avoid synchronization) and allocate a
/// new one.
///
/// https://www.opengl.org/wiki/Buffer_Object_Streaming
void DrawBuffer::clear()
{
    this->bind();

    size_t element_size = sizeof( *(this->contains) );
    GLsizeiptr storage_size = (GLsizeiptr) (this->capacity * element_size);
    glBufferData(   GL_ARRAY_BUFFER,
                    storage_size,
                    NULL,
                    GL_DYNAMIC_DRAW);

    this->len = 0;

    /* get_error() */
}

void DrawBuffer::bind()
{
    glBindBuffer(GL_ARRAY_BUFFER, this->id);
}

