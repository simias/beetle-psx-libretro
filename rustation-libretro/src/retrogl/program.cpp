#include "program.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

Program::Program(Shader* vertex_shader, Shader* fragment_shader)
{
    GLuint id = glCreateProgram();
    printf("PROGRAM ID: %d\n", (int) id);

    vertex_shader->attach_to(id);
    fragment_shader->attach_to(id);

    glLinkProgram(id);

    vertex_shader->detach_from(id);
    fragment_shader->detach_from(id);

    /* Program owns the two pointers, so we clean them up now */
    if (vertex_shader != nullptr) delete vertex_shader;
    if (fragment_shader != nullptr) delete fragment_shader;

    // Check if the program linking was successful
    GLint status = (GLint) GL_FALSE;
    glGetProgramiv(id, GL_LINK_STATUS, &status);
    printf("GL LINK STATUS: %d\n", (int) status);

    if (status == (GLint) GL_TRUE) {
        /* Rust code has a try statement here, perhaps we should fail fast with
        exit(EXIT_FAILURE) ? */
        UniformMap uniforms = load_program_uniforms(id);

        // There shouldn't be anything in glGetError but let's
        // check to make sure.
        assert( !glGetError() );

        this->id = id;
        this->uniforms = uniforms;
    } else {
        puts("OpenGL program linking failed\n");
        puts("Program info log:\n");
        puts( get_program_info_log(id) );

        exit(EXIT_FAILURE);
    }
}

GLint Program::find_attribute(const char* attr)
{
    printf("FIND ATTRIBUTE: %s\n", attr);
    GLint index = glGetAttribLocation(this->id, attr);

    if (index < 0) {
        printf("Couldn't find attribute \"%s\" in program\n", attr);
        printf("GL error %d\n", (int) glGetError());
        /* exit(EXIT_FAILURE); */
    }

    return index;
}

void Program::bind()
{
    glUseProgram(this->id);
}

GLint Program::uniform(const char* name)
{
    bool found = this->uniforms.find(name) != this->uniforms.end();
    if (!found) {
        printf("Attempted to access unknown uniform %s\n", name);
        exit(EXIT_FAILURE);
    }

    return this->uniforms[name];
}

void Program::uniform1i(const char* name, GLint i)
{
    this->bind();

    GLint u = this->uniform(name);
    glUniform1i(u, i);
}

void Program::uniform1ui(const char* name, GLuint i)
{
    this->bind();

    GLint u = this->uniform(name);
    glUniform1ui(u, i);
}

void Program::uniform2i(const char* name, GLint a, GLint b)
{
    this->bind();

    GLint u = this->uniform(name);
    glUniform2i(u, a, b); 
}

void Program::drop()
{
    glDeleteProgram(this->id);
}

const char* get_program_info_log(GLuint id)
{
    GLint log_len = 0;

    glGetProgramiv(id, GL_INFO_LOG_LENGTH, &log_len);

    if (log_len <= 0) {
        return " ";
    }

    char log[(size_t) log_len];
    GLsizei len = (GLsizei) log_len;
    glGetProgramInfoLog(id,
                        len,
                        &log_len,
                        (char*) log);

    if (log_len <= 0) {
        return " ";
    }

    // The length returned by GetShaderInfoLog *excludes*
    // the ending \0 unlike the call to GetShaderiv above
    // so we can get rid of it by truncating here.
    /* log.truncate(log_len as usize); */
    /* Don't want to spend time thinking about the above, I'll just put a \0
    in the last index */
    log[log_len - 1] = '\0';

    return (const char*) log;
}

UniformMap load_program_uniforms(GLuint program)
{
    printf("load_program_uniforms()\n");
    GLint n_uniforms = 0;

    glGetProgramiv( program,
                    GL_ACTIVE_UNIFORMS,
                    &n_uniforms );

    UniformMap uniforms;

    // Figure out how long a uniform name can be
    GLint max_name_len = 0;

    glGetProgramiv( program,
                    GL_ACTIVE_UNIFORM_MAX_LENGTH,
                    &max_name_len);

    assert( !glGetError() );

    size_t u;
    printf("Uniform names:\n");
    for (u = 0; u < n_uniforms; ++u) {
        // Retrieve the name of this uniform
        char name[max_name_len];
        size_t name_len = max_name_len;
        GLsizei len = 0;
        // XXX we might want to validate those at some point
        GLint size = 0;
        GLenum ty = 0;

        glGetActiveUniform( program,
                            (GLuint) u,
                            (GLsizei) name_len,
                            &len,
                            &size,
                            &ty,
                            (char*) name);
        if (len <= 0) {
            printf("Ignoring uniform name with size %d\n", len);
            continue;
        }

        // Retrieve the location of this uniform
        GLint location = glGetUniformLocation(program, (const char*) name);

        /* name.truncate(len as usize); */
        /* name[len - 1] = '\0'; */

        if (location < 0) {
            printf("Uniform \"%s\" doesn't have a location", name);
            continue;
        }

        uniforms[name] = location;
        printf("\t%s - location %d\n", name, location);
    }

    assert( !glGetError() );

    return uniforms;
}
