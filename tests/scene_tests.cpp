#include "scene3d.hpp"
#include <opencv2/calib3d.hpp>
#include <iostream>
#include <cmath>
#include <functional>
static void require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
static void rejects(std::function<void()> f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"Expected rejection");}
int main(){try{
    FpsView v;v.eye={0,0,0};v.yaw=CV_PI/2;v.pitch=0;v.speed=1;
    v.move(1,0,0,.02,false);require(cv::norm(v.eye-cv::Vec3d(0,.02,0))<1e-9,"W moves along viewing direction");
    v.move(0,1,0,.02,false);require(cv::norm(v.eye-cv::Vec3d(.02,.02,0))<1e-9,"D strafes right in Z-up coordinates");
    v.move(-1,-1,0,.02,false);require(std::abs(cv::norm(v.eye)-(.02*std::sqrt(2.)-.02))<1e-9,"Diagonal movement is normalized");
    auto before=v.eye;v.move(0,0,1,.02,true);require(std::abs(v.eye[2]-before[2]-.08)<1e-9,"E raises viewpoint and Shift gives 4x speed");
    v.look(100,0);require(v.forward()[0]>0,"Rightward mouse look turns toward view right");v.look(0,1e8);v.validate();require(v.pitch<0,"Pitch clamps without flipping");
    v.eye={1,-1,1};v.aimAt({0,0,0});auto m=v.matrix(16./9);auto p=projectScenePoint(m,{0,0,0});
    require(p&&cv::norm(*p-cv::Point2d(.5,.5))<1e-8,"Look-at target projects to canvas center");
    require(!projectScenePoint(m,v.eye-v.forward()),"Points behind viewpoint are clipped");
    require(floorIntersection({0,0,1},{.2,.3,-1})&&cv::norm(*floorIntersection({0,0,1},{.2,.3,-1})-cv::Vec3d(.2,.3,0))<1e-9,"Optical axis intersects floor in forward direction");
    require(!floorIntersection({0,0,1},{0,0,1})&&!floorIntersection({0,0,1},{1,0,0}),"Backward and parallel floor rays do not create false hits");
    Settings s;Calibration k;k.imageSize={1280,960};k.K=(cv::Mat_<double>(3,3)<<900,0,630,0,910,475,0,0,1);k.distortion=(cv::Mat_<double>(1,5)<<-.05,.01,0,0,0);
    StereoCalibration edge;cv::Rodrigues(cv::Vec3d(.1,.2,.05),edge.R);edge.T=(cv::Mat_<double>(3,1)<<-.15,.03,.02);edge.perPairErrors={.1};edge.secondRotations={0};
    auto other=edge;cv::Rodrigues(cv::Vec3d(-.05,-.1,.08),other.R);other.T=(cv::Mat_<double>(3,1)<<.1,-.02,.01);
    BoardConfig board;Settings::SavedRig rig{{"A","B","C"},{"ia","ib","ic"},"rig",board,RigCalibration{{edge,other}}};s.rig=rig;
    for(size_t i=0;i<3;++i){SavedCamera saved;saved.identity=rig.serials[i];saved.archive=rig.intrinsicArchives[i];saved.sourceKind="live";saved.calibration=k;s.cameras[saved.identity]=saved;}
    WorldOrigin w;cv::Rodrigues(cv::Vec3d(2.8,.1,.2),w.R);w.T=(cv::Mat_<double>(3,1)<<.02,.1,1);w.errors={.1,.1,.1};s.origin=Settings::SavedOrigin{"rig","origin",w};
    auto cameras=buildSceneCameras(s);require(cameras.size()==3,"Every calibrated camera is included");
    cv::Vec3d worldPoint(.25,.5,.15);
    for(size_t i=0;i<3;++i){auto& c=cameras[i];cv::Mat center=(cv::Mat_<double>(3,1)<<c.center[0],c.center[1],c.center[2]);
        cv::Mat r=cv::Mat::eye(3,3,CV_64F),t=cv::Mat::zeros(3,1,CV_64F);if(i){r=rig.calibration.fromReference[i-1].R;t=rig.calibration.fromReference[i-1].T;}
        require(cv::norm(r*(w.R*center+w.T)+t)<1e-9,"Scene center maps to its physical camera origin");
        auto forward=c.forward();cv::Mat direction=(cv::Mat_<double>(3,1)<<forward[0],forward[1],forward[2]);cv::Mat expected=(cv::Mat_<double>(3,1)<<0,0,1);
        require(cv::norm(r*w.R*direction-expected)<1e-9,"Scene arrow is camera +Z, not inverse transform");
        cv::Mat localPoint=r*(w.R*cv::Mat(worldPoint)+w.T)+t;
        auto measured=cameraPointToWorld(s,c.serial,{localPoint.at<double>(0),localPoint.at<double>(1),localPoint.at<double>(2)});
        require(measured&&cv::norm(*measured-worldPoint)<1e-10,"Triangulated point transforms from any selected camera into saved world coordinates");
        std::vector<cv::Point3d> rays;for(auto ray:c.cornerRays){auto local=c.cameraToWorld.t()*ray;rays.emplace_back(local[0],local[1],local[2]);}
        std::vector<cv::Point2d> pixels;cv::projectPoints(rays,cv::Vec3d(0,0,0),cv::Vec3d(0,0,0),k.K,k.distortion,pixels);
        require(cv::norm(pixels[0]-cv::Point2d(0,0))<.01&&cv::norm(pixels[2]-cv::Point2d(1279,959))<.01,"Frustum edges reproject to calibrated image boundaries including distortion");
    }
    frameScene(v,cameras);v.validate();require(v.initialized,"Frame-all initializes persisted view");
    auto mesh=makeSceneMesh(cameras,{});require(!mesh.lines.empty()&&!mesh.triangles.empty(),"Grid and solid camera bodies rendered");
    SceneMeasurement first{1,worldPoint,{cameras[0].center,cameras[1].center},{worldPoint-cv::Vec3d(0,.001,0),worldPoint+cv::Vec3d(0,.001,0)},.002};
    auto second=first;second.id=2;second.position+=cv::Vec3d(.1,0,0);
    auto measuredMesh=makeSceneMesh(cameras,{},-1,{first,second});
    require(measuredMesh.triangles.size()==mesh.triangles.size()+48&&measuredMesh.lines.size()==mesh.lines.size(),"All points have solid markers without unselected rays");
    auto selectedMesh=makeSceneMesh(cameras,{},-1,{first,second},1);
    require(selectedMesh.lines.size()==mesh.lines.size()+14,"Only selected point has two rays with arrowheads and shortest segment");
    auto vertexPoint=[](const SceneVertex& p){return cv::Vec3d(p.x,p.y,p.z);};
    require(cv::norm(vertexPoint(selectedMesh.lines[mesh.lines.size()])-first.origins[0])<1e-6,"Left ray starts at the first camera optical center");
    require(cv::norm(vertexPoint(selectedMesh.lines[mesh.lines.size()+6])-first.origins[1])<1e-6,"Right ray starts at the second camera optical center");
    require(cv::norm(vertexPoint(selectedMesh.lines[selectedMesh.lines.size()-2])-first.closest[0])<1e-6&&cv::norm(vertexPoint(selectedMesh.lines.back())-first.closest[1])<1e-6,"Shortest segment uses actual endpoints without visual exaggeration");
    auto noOrigin=s;noOrigin.origin.reset();require(!cameraPointToWorld(noOrigin,"A",{0,0,1}),"Camera-relative triangulation does not invent a missing world origin");
    auto oldOrigin=s;oldOrigin.origin->rigArchive="previous-rig";require(!cameraPointToWorld(oldOrigin,"B",{0,0,1}),"Stale origin cannot produce misleading world coordinates");
    for(auto& x:mesh.lines)require(std::isfinite(x.x)&&std::isfinite(x.y)&&std::isfinite(x.z),"Scene geometry finite");
    SceneOptions options;options.gridExtent=50;options.gridStep=.01;auto large=makeSceneMesh(cameras,options);require(large.lines.size()<3000,"Grid density bounded for large extents");
    s.rig->archive="changed";rejects([&]{buildSceneCameras(s);});s.rig->archive="rig";s.cameras["C"].archive="changed";rejects([&]{buildSceneCameras(s);});
    std::cout<<"PASS: FPS movement, mouse look, projection, floor rays, camera centers/directions, calibrated frusta and stale-data rejection\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
