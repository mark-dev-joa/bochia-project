#include "scene3d.hpp"
#include <opencv2/calib3d.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
constexpr double pi=3.14159265358979323846;
bool finite(cv::Vec3d v){return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);}
cv::Vec3d vec(const cv::Mat& m){return {m.at<double>(0),m.at<double>(1),m.at<double>(2)};}
cv::Matx33d mat(const cv::Mat& m){cv::Matx33d r;for(int y=0;y<3;++y)for(int x=0;x<3;++x)r(y,x)=m.at<double>(y,x);return r;}
}
void FpsView::validate()const{
    if(!finite(eye)||cv::norm(eye)>10000||!std::isfinite(yaw)||!std::isfinite(pitch)||std::abs(pitch)>1.554||
        !std::isfinite(speed)||speed<.01||speed>20)throw std::runtime_error("Invalid 3D view settings.");
}
cv::Vec3d FpsView::forward()const{return {std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),std::sin(pitch)};}
cv::Vec3d FpsView::right()const{return {std::sin(yaw),-std::cos(yaw),0};}
void FpsView::look(double dx,double dy){yaw=std::remainder(yaw-dx*.003,2*pi);pitch=std::clamp(pitch-dy*.003,-1.553,1.553);}
void FpsView::move(double f,double r,double u,double seconds,bool fast){
    auto d=forward()*f+right()*r+cv::Vec3d(0,0,u);double n=cv::norm(d);
    if(n>1e-8){d/=std::max(1.,n);auto next=eye+d*(speed*(fast?4:1)*std::clamp(seconds,0.,.05));if(cv::norm(next)<10000)eye=next;}
}
void FpsView::aimAt(cv::Vec3d target){auto f=target-eye;double n=cv::norm(f);if(n<1e-8)return;yaw=std::atan2(f[1],f[0]);pitch=std::clamp(std::asin(std::clamp(f[2]/n,-1.,1.)),-1.553,1.553);initialized=true;}
cv::Matx44d FpsView::matrix(double aspect)const{
    validate();if(!std::isfinite(aspect)||aspect<=0)throw std::runtime_error("Invalid 3D aspect ratio.");
    auto f=forward(),r=right(),u=r.cross(f);
    cv::Matx44d v(r[0],r[1],r[2],-r.dot(eye),u[0],u[1],u[2],-u.dot(eye),-f[0],-f[1],-f[2],f.dot(eye),0,0,0,1);
    double s=1/std::tan(55*pi/360),near=.001,far=1000;
    cv::Matx44d p(s/aspect,0,0,0,0,s,0,0,0,0,-(far+near)/(far-near),-2*far*near/(far-near),0,0,-1,0);return p*v;
}
std::vector<SceneCamera> buildSceneCameras(const Settings& s){
    if(!s.originCurrent())throw std::runtime_error("유효한 바닥 원점과 다중 카메라 보정값을 먼저 저장하세요.");
    s.rig->validate();s.origin->transform.validate();const auto& world=s.origin->transform;
    std::vector<SceneCamera> result;
    for(size_t i=0;i<s.rig->serials.size();++i){const auto& saved=s.cameras.at(s.rig->serials[i]);const auto& k=saved.calibration;
        cv::Mat relative=cv::Mat::eye(3,3,CV_64F);if(i)relative=s.rig->calibration.fromReference[i-1].R;
        SceneCamera c;c.serial=saved.identity;c.model=saved.model;c.center=vec(world.cameraCenter(s.rig->calibration,i));
        c.cameraToWorld=mat(cv::Mat(world.R.t()*relative.t()));
        std::vector<cv::Point2d> pixels{{0,0},{double(k.imageSize.width-1),0},{double(k.imageSize.width-1),double(k.imageSize.height-1)},{0,double(k.imageSize.height-1)}},rays;
        cv::undistortPoints(pixels,rays,k.K,k.distortion);
        for(size_t j=0;j<4;++j){c.cornerRays[j]=c.cameraToWorld*cv::Vec3d(rays[j].x,rays[j].y,1);if(!finite(c.cornerRays[j]))throw std::runtime_error("Invalid camera field of view.");}
        result.push_back(c);
    }return result;
}
void frameScene(FpsView& v,const std::vector<SceneCamera>& cameras){
    cv::Vec3d lo(0,0,0),hi(0,0,0);for(auto& c:cameras)for(int j=0;j<3;++j){lo[j]=std::min(lo[j],c.center[j]);hi[j]=std::max(hi[j],c.center[j]);}
    auto center=(lo+hi)*.5;double radius=std::max(.2,cv::norm(hi-lo)*.6);
    v.eye=center+cv::Vec3d(1,-1,.75)*(radius*2.4);v.aimAt(center);v.speed=std::clamp(radius,.05,5.);
}
std::optional<cv::Vec3d> floorIntersection(cv::Vec3d p,cv::Vec3d ray,double limit){
    if(!finite(p)||!finite(ray)||std::abs(ray[2])<1e-8)return {};
    double t=-p[2]/ray[2];if(t<=0)return {};auto hit=p+ray*t;
    if(cv::norm(hit-p)>limit||!finite(hit))return {};hit[2]=0;return hit;
}
std::optional<cv::Point2d> projectScenePoint(const cv::Matx44d& m,cv::Vec3d p){
    auto clip=m*cv::Vec4d(p[0],p[1],p[2],1);if(clip[3]<=0||std::abs(clip[2])>clip[3])return {};
    double x=clip[0]/clip[3],y=clip[1]/clip[3];if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>1||std::abs(y)>1)return {};
    return cv::Point2d((x+1)*.5,(1-y)*.5);
}
void SceneOptions::validate()const{
    if(!std::isfinite(gridStep)||gridStep<.01||gridStep>2||!std::isfinite(gridExtent)||gridExtent<.2||gridExtent>50||
       !std::isfinite(frustumLength)||frustumLength<.01||frustumLength>10)throw std::runtime_error("Invalid 3D display settings.");
}
SceneMesh makeSceneMesh(const std::vector<SceneCamera>& cameras,const SceneOptions& o,int selected,const std::vector<SceneMeasurement>& measurements,int selectedPoint){
    o.validate();SceneMesh mesh;
    auto vertex=[](cv::Vec3d p,cv::Vec3d c){return SceneVertex{float(p[0]),float(p[1]),float(p[2]),float(c[0]),float(c[1]),float(c[2])};};
    auto line=[&](cv::Vec3d a,cv::Vec3d b,cv::Vec3d c){mesh.lines.push_back(vertex(a,c));mesh.lines.push_back(vertex(b,c));};
    auto triangle=[&](cv::Vec3d a,cv::Vec3d b,cv::Vec3d c,cv::Vec3d color){for(auto p:{a,b,c})mesh.triangles.push_back(vertex(p,color));};
    auto quad=[&](cv::Vec3d a,cv::Vec3d b,cv::Vec3d c,cv::Vec3d d,cv::Vec3d color){triangle(a,b,c,color);triangle(a,c,d,color);};
    auto arrow=[&](cv::Vec3d a,cv::Vec3d b,cv::Vec3d color){line(a,b,color);auto v=b-a;double n=cv::norm(v);if(n<1e-7)return;v/=n;
        auto side=v.cross(std::abs(v[2])>.9?cv::Vec3d(1,0,0):cv::Vec3d(0,0,1));side/=cv::norm(side);double h=std::min(.035,n*.15);
        line(b,b-v*h+side*h*.45,color);line(b,b-v*h-side*h*.45,color);};
    double e=o.gridExtent*.5;
    if(o.grid){
        // Grid has no opaque floor, so camera positions below the estimated plane remain visible.
        int count=std::min(250,int(std::ceil(e/o.gridStep)));double step=std::max(o.gridStep,e/250);
        for(int i=-count;i<=count;++i){double p=i*step;auto color=i%5==0?cv::Vec3d(.22,.29,.36):cv::Vec3d(.13,.18,.24);
            line({-e,p,0},{e,p,0},color);line({p,-e,0},{p,e,0},color);}
    }
    if(o.axes){double length=std::clamp(o.gridExtent*.2,.15,1.);
        arrow({0,0,0},{length,0,0},{1,.25,.28});arrow({0,0,0},{0,length,0},{.3,.95,.5});arrow({0,0,0},{0,0,length},{.25,.6,1});
        double r=.02;line({-r,0,.001},{r,0,.001},{1,1,1});line({0,-r,.001},{0,r,.001},{1,1,1});}
    const cv::Vec3d colors[]={{.2,.8,1},{1,.65,.25},{.8,.45,1}};
    for(size_t i=0;i<cameras.size();++i){const auto& c=cameras[i];auto color=colors[i%3];if(int(i)==selected)color={1,1,.5};
        auto world=[&](cv::Vec3d p){return c.center+c.cameraToWorld*p;};
        // Compact marker; optical center and direction are calibrated, casing dimensions are not.
        std::array<cv::Vec3d,8> p;for(int j=0;j<8;++j)p[j]=world({j&1?.009:-.009,j&2?.0065:-.0065,j&4?-.003:-.022});
        quad(p[0],p[1],p[3],p[2],color*.35);quad(p[4],p[5],p[7],p[6],color*.8);
        quad(p[0],p[1],p[5],p[4],color*.55);quad(p[2],p[3],p[7],p[6],color*.55);
        quad(p[0],p[2],p[6],p[4],color*.5);quad(p[1],p[3],p[7],p[5],color*.65);
        for(int j=0;j<8;++j)for(int bit:{1,2,4})if(!(j&bit))line(p[j],p[j|bit],color);
        for(int j=0;j<16;++j){double a=j*2*pi/16,b=(j+1)*2*pi/16;line(world({.005*std::cos(a),.005*std::sin(a),0}),world({.005*std::cos(b),.005*std::sin(b),0}),color);}
        if(o.frusta){for(int j=0;j<4;++j){auto end=c.center+c.cornerRays[j]*o.frustumLength;
            line(c.center,end,color*.6);line(end,c.center+c.cornerRays[(j+1)%4]*o.frustumLength,color*.8);}}
        if(o.directions)arrow(c.center,c.center+c.forward()*.08,color);
    }
    for(const auto& measurement:measurements){
        const auto p=measurement.position;if(!finite(p))throw std::runtime_error("Invalid measured 3D point.");
        bool active=measurement.id==selectedPoint;
        const double radius=active?.006:.004;std::array<cv::Vec3d,4> ring{p+cv::Vec3d(radius,0,0),p+cv::Vec3d(0,radius,0),p+cv::Vec3d(-radius,0,0),p+cv::Vec3d(0,-radius,0)};
        auto color=active?cv::Vec3d(1,.9,.25):cv::Vec3d(.3,1,.65);
        for(int i=0;i<4;++i){triangle(p+cv::Vec3d(0,0,radius),ring[i],ring[(i+1)%4],color);triangle(p-cv::Vec3d(0,0,radius),ring[(i+1)%4],ring[i],color*.6);}
        if(active){
            for(int side=0;side<2;++side){auto origin=measurement.origins[side],closest=measurement.closest[side];
                if(!finite(origin)||!finite(closest))throw std::runtime_error("Invalid measurement ray.");
                // Extend slightly past the closest point without moving either endpoint of the measured gap.
                arrow(origin,origin+(closest-origin)*1.05,colors[side]);}
            line(measurement.closest[0],measurement.closest[1],{1,.35,.85});
        }
    }
    return mesh;
}
