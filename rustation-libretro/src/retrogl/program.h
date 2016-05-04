#ifndef RETROGL_PROGRAM_H
#define RETROGL_PROGRAM_H

#include <map>
#include <glsm/glsmsym.h>

#include "shader.h"

typedef std::map<const char*, GLint> UniformMap;

class Program {
public:
    GLuint id;
    /// Hash map of all the active uniforms in this program
    UniformMap uniforms;

    Program(Shader* vertex_shader, Shader* fragment_shader);
    GLuint find_attribute(const char* attr);
    void bind();
    Glint uniform(const char* name);
    void uniform1i(const char* name, GLint i);
    void uniform1ui(const char* name, GLuint i);
    void uniform2i(const char* name, GLint a, GLint b);
    void drop();

};

const char* get_program_info_log(GLuint id);

// Return a hashmap of all uniform names contained in `program` with
// their corresponding location.
UniformMap load_program_uniforms(GLuint program);

#endif
