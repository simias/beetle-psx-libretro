#include "shader.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

Shader::Shader(const char** source, GLenum shader_type)
{
    GLuint id = glCreateShader(shader_type);

    size_t src_size = ARRAY_SIZE(source);

    glShaderSource( id,
                    1,
                    source,
                    (const GLint*) &src_size);
    glCompileShader(id);

    GLint status = (GLint) GL_FALSE;
    glGetShaderiv(id, GL_COMPILE_STATUS, &status);

    if (status == (GLint) GL_TRUE) {
        // There shouldn't be anything in glGetError but let's
        // check to make sure.
        assert( !glGetError() );
    } else {
        puts("Shader compilation failed:\n");

        /* print shader source */
        size_t i;
        for (i = 0; i < src_size; ++i) {
            puts( source[i] );
        }

        puts("Shader info log:\n");
        puts( get_shader_info_log(id) );

        exit(EXIT_FAILURE);
    }
}

void Shader::attach_to(GLuint program)
{
    glAttachShader(program, this->id);
}

void Shader::detach_from(GLuint program)
{
    glDetachShader(program, this->id);
}

void Shader::drop()
{
    glDeleteShader(this->id);
}

const char* get_shader_info_log(GLuint id)
{
    GLint log_len = 0;

    glGetShaderiv(id, GL_INFO_LOG_LENGTH, &log_len);

    if (log_len <= 0) {
        return " ";
    }

    char log[(size_t) log_len];
    GLsizei len = (GLsizei) log_len;
    glGetShaderInfoLog(id,
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
