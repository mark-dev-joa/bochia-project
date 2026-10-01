#pragma once
#include "scene3d.hpp"
#include <glad/gl.h>

class SceneRenderer {
    GLuint framebuffer=0,color=0,depth=0,program=0,vao=0,vbo=0;
    int width=0,height=0;
    void initialize();
public:
    SceneRenderer()=default;
    SceneRenderer(const SceneRenderer&)=delete;
    SceneRenderer& operator=(const SceneRenderer&)=delete;
    void release(); // Main GL context must be current, before ImGui/GLFW shutdown.
    GLuint render(const SceneMesh& mesh,const cv::Matx44d& matrix,int width,int height);
};
