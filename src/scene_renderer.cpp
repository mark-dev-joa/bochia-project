#include "scene_renderer.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <cstddef>

namespace {
// Rendering occurs during UI construction; leave the host and ImGui GL state intact.
struct GlState {
    GLint draw,read,renderbuffer,texture,program,vao,buffer,viewport[4],depthFunc,polygon[2];
    GLboolean blend,cull,scissor,depth,srgb,depthMask,colorMask[4];GLfloat clearColor[4];GLdouble clearDepth;
    GlState(){
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);
        glGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
        glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&buffer);
        glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_DEPTH_FUNC,&depthFunc);glGetIntegerv(GL_POLYGON_MODE,polygon);
        blend=glIsEnabled(GL_BLEND);cull=glIsEnabled(GL_CULL_FACE);scissor=glIsEnabled(GL_SCISSOR_TEST);depth=glIsEnabled(GL_DEPTH_TEST);srgb=glIsEnabled(GL_FRAMEBUFFER_SRGB);
        glGetBooleanv(GL_DEPTH_WRITEMASK,&depthMask);glGetBooleanv(GL_COLOR_WRITEMASK,colorMask);glGetFloatv(GL_COLOR_CLEAR_VALUE,clearColor);glGetDoublev(GL_DEPTH_CLEAR_VALUE,&clearDepth);
    }
    ~GlState(){
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
        glBindTexture(GL_TEXTURE_2D,texture);glUseProgram(program);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,buffer);
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glDepthFunc(depthFunc);glPolygonMode(GL_FRONT_AND_BACK,polygon[0]);
        auto enabled=[](GLenum flag,bool value){if(value)glEnable(flag);else glDisable(flag);};
        enabled(GL_BLEND,blend);enabled(GL_CULL_FACE,cull);enabled(GL_SCISSOR_TEST,scissor);enabled(GL_DEPTH_TEST,depth);enabled(GL_FRAMEBUFFER_SRGB,srgb);
        glDepthMask(depthMask);glColorMask(colorMask[0],colorMask[1],colorMask[2],colorMask[3]);glClearColor(clearColor[0],clearColor[1],clearColor[2],clearColor[3]);glClearDepth(clearDepth);
    }
};
GLuint shader(GLenum type,const char* source){
    GLuint s=glCreateShader(type);glShaderSource(s,1,&source,nullptr);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048]{};glGetShaderInfoLog(s,sizeof(log),nullptr,log);glDeleteShader(s);throw std::runtime_error(std::string("3D shader: ")+log);}return s;
}
}
void SceneRenderer::initialize(){
    if(program)return;
    GLuint vs=0,fs=0,p=0;
    try{
        vs=shader(GL_VERTEX_SHADER,"#version 330 core\nlayout(location=0) in vec3 position; layout(location=1) in vec3 color; uniform mat4 mvp; out vec3 rgb; void main(){gl_Position=mvp*vec4(position,1);rgb=color;}");
        fs=shader(GL_FRAGMENT_SHADER,"#version 330 core\nin vec3 rgb;out vec4 outColor;void main(){outColor=vec4(rgb,1);}");
        p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glLinkProgram(p);GLint ok;glGetProgramiv(p,GL_LINK_STATUS,&ok);
        if(!ok)throw std::runtime_error("3D shader link failed.");
        glDeleteShader(vs);vs=0;glDeleteShader(fs);fs=0;program=p;
        glGenFramebuffers(1,&framebuffer);glGenTextures(1,&color);glGenRenderbuffers(1,&depth);glGenVertexArrays(1,&vao);glGenBuffers(1,&vbo);
    }catch(...){if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);if(p)glDeleteProgram(p);program=0;throw;}
}
void SceneRenderer::release(){
    if(vbo)glDeleteBuffers(1,&vbo);if(vao)glDeleteVertexArrays(1,&vao);if(depth)glDeleteRenderbuffers(1,&depth);
    if(color)glDeleteTextures(1,&color);if(framebuffer)glDeleteFramebuffers(1,&framebuffer);if(program)glDeleteProgram(program);
    vbo=vao=depth=color=framebuffer=program=0;width=height=0;
}
GLuint SceneRenderer::render(const SceneMesh& mesh,const cv::Matx44d& matrix,int w,int h){
    GlState saved;initialize();GLint maxSize;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maxSize);maxSize=std::min(4096,maxSize);
    w=std::clamp(w,1,maxSize);h=std::clamp(h,1,maxSize);
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    if(w!=width||h!=height){
        glBindTexture(GL_TEXTURE_2D,color);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
        glBindRenderbuffer(GL_RENDERBUFFER,depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,w,h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("3D framebuffer allocation failed.");width=w;height=h;
    }
    glViewport(0,0,width,height);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_TRUE);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
    glClearColor(.032f,.045f,.067f,1);glClearDepth(1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glUseProgram(program);float m[16];for(int i=0;i<16;++i)m[i]=float(matrix.val[i]);glUniformMatrix4fv(glGetUniformLocation(program,"mvp"),1,GL_TRUE,m);
    glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(SceneVertex),nullptr);
    glEnableVertexAttribArray(1);glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(SceneVertex),reinterpret_cast<void*>(offsetof(SceneVertex,r)));
    auto draw=[&](const std::vector<SceneVertex>& vertices,GLenum mode){if(vertices.empty())return;
        glBufferData(GL_ARRAY_BUFFER,GLsizeiptr(vertices.size()*sizeof(SceneVertex)),vertices.data(),GL_STREAM_DRAW);glDrawArrays(mode,0,GLsizei(vertices.size()));};
    draw(mesh.triangles,GL_TRIANGLES);draw(mesh.lines,GL_LINES);return color;
}
