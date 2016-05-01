#ifndef RETROGL_SHADER_H
#define RETROGL_SHADER_H

#include <glsm/glsmsym.h>

class Shader {
    GLuint id;

    Shader(const char** source, GLenum shader_type);
    void attach_to(GLuint program);
    void detach_from(GLuint program);
    void drop();
};

const char* get_shader_info_log(GLuint id);

#endif
