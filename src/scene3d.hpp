#pragma once
#include "settings.hpp"
#include <array>

struct FpsView {
    cv::Vec3d eye{1,-1,1};
    double yaw=2.356194490192345,pitch=-.5,speed=.5;
    bool initialized=false;
    void validate()const;
    cv::Vec3d forward()const;
    cv::Vec3d right()const;
    void look(double dx,double dy);
    void move(double forwardInput,double rightInput,double upInput,double seconds,bool fast);
    void aimAt(cv::Vec3d target);
    cv::Matx44d matrix(double aspect)const;
};
struct SceneCamera {
    std::string serial,model;
    cv::Vec3d center;
    cv::Matx33d cameraToWorld;
    std::array<cv::Vec3d,4> cornerRays; // Undistorted rays, camera z=1, transformed to world.
    cv::Vec3d forward()const{return cameraToWorld*cv::Vec3d(0,0,1);}
};
std::vector<SceneCamera> buildSceneCameras(const Settings& settings);
void frameScene(FpsView& view,const std::vector<SceneCamera>& cameras);
std::optional<cv::Vec3d> floorIntersection(cv::Vec3d point,cv::Vec3d ray,double limit=100);
std::optional<cv::Point2d> projectScenePoint(const cv::Matx44d& matrix,cv::Vec3d point);

struct SceneVertex {float x,y,z,r,g,b;};
struct SceneMesh {std::vector<SceneVertex> triangles,lines;};
struct SceneOptions {
    bool grid=true,axes=true,frusta=false,directions=true;
    double gridStep=.1,gridExtent=2,frustumLength=.3;
    void validate()const;
};
struct SceneMeasurement {
    int id=0;
    cv::Vec3d position;
    std::array<cv::Vec3d,2> origins{},closest{};
    double gapM=0;
};
SceneMesh makeSceneMesh(const std::vector<SceneCamera>& cameras,const SceneOptions& options,int selected=-1,
                        const std::vector<SceneMeasurement>& measurements={},int selectedPoint=-1);
