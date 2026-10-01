#include "world_origin.hpp"
#include "settings.hpp"
#include <opencv2/calib3d.hpp>
#include <iostream>
#include <numeric>
#include <chrono>
#include <functional>
static void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
static void rejects(std::function<void()> f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"Expected rejection");}
int main(){try{
    BoardConfig board;board.columns=8;board.rows=6;
    Calibration a;a.imageSize={1440,1080};a.K=(cv::Mat_<double>(3,3)<<1200,0,720,0,1200,540,0,0,1);a.distortion=cv::Mat::zeros(1,5,CV_64F);
    Calibration b=a,c=a;c.imageSize={1600,1200};c.K=(cv::Mat_<double>(3,3)<<1300,0,800,0,1300,600,0,0,1);
    std::vector<Calibration> intrinsics{a,b,c};StereoCalibration ab,ac;
    cv::Rodrigues(cv::Vec3d(.01,.08,-.02),ab.R);ab.T=(cv::Mat_<double>(3,1)<<-.12,.004,.01);
    cv::Rodrigues(cv::Vec3d(-.02,.03,.01),ac.R);ac.T=(cv::Mat_<double>(3,1)<<.07,-.01,.02);
    ab.perPairErrors=ac.perPairErrors={.1};ab.secondRotations=ac.secondRotations={0};RigCalibration rig{{ab,ac}};
    cv::Mat rb,tb=(cv::Mat_<double>(3,1)<<-.09,-.06,.85);cv::Rodrigues(cv::Vec3d(.2,-.1,.3),rb);
    auto synth=[&](const BoardConfig& config){RigSample sample;sample.receiptSkewMs=20;
        std::vector<cv::Point3f> objects;if(config.pattern==Pattern::Charuco)objects=config.charucoBoard().getChessboardCorners();
        else for(int y=0;y<config.rows;++y)for(int x=0;x<config.columns;++x)objects.emplace_back(float(x*config.squareMm/1000),float(y*config.squareMm/1000),0.f);
        for(size_t i=0;i<3;++i){Observation o;o.image=cv::Mat(intrinsics[i].imageSize,CV_8UC3,cv::Scalar(90,90,90));o.objectPoints=objects;
            cv::Mat r=rb.clone(),t=tb.clone(),rv;if(i){r=rig.fromReference[i-1].R*rb;t=rig.fromReference[i-1].R*tb+rig.fromReference[i-1].T;}cv::Rodrigues(r,rv);
            cv::projectPoints(objects,rv,t,intrinsics[i].K,intrinsics[i].distortion,o.imagePoints);
            if(config.pattern==Pattern::Chessboard&&i==1)std::reverse(o.imagePoints.begin(),o.imagePoints.end());
            if(config.pattern==Pattern::Charuco){o.ids.resize(objects.size());std::iota(o.ids.begin(),o.ids.end(),0);
                for(size_t n=0;n<i;++n){o.ids.erase(o.ids.begin());o.objectPoints.erase(o.objectPoints.begin());o.imagePoints.erase(o.imagePoints.begin());}}
            sample.cameras.push_back(std::move(o));}return sample;};
    auto sample=synth(board);auto pose=locateBoard(sample,board,intrinsics,rig);
    require(cv::norm(pose.R-rb)<1e-5&&cv::norm(pose.T-tb)<1e-6,"Joint pose recovers ground truth with reversed B corners and mixed resolution");
    require(pose.cornerRotations[1]==2&&pose.errors.size()==3,"Every camera contributes diagnostics");
    cv::Mat center=(cv::Mat_<double>(3,1)<<(board.columns-1)*board.squareMm/2000,(board.rows-1)*board.squareMm/2000,0);
    auto w=makeWorldOrigin(pose,board,rig);cv::Mat centerA=rb*center+tb;
    require(cv::norm(w.R.t()*(centerA-w.T))<1e-6,"Board center is world origin");
    require(w.cameraCenter(rig,0).at<double>(2)>0&&cv::determinant(w.R)>.999,"Z faces cameras and basis stays right handed");
    for(size_t i=0;i<3;++i){cv::Mat ca=cv::Mat::zeros(3,1,CV_64F);if(i)ca=rig.fromReference[i-1].secondCenterInFirst();
        require(cv::norm(w.R*w.cameraCenter(rig,i)+w.T-ca)<1e-8,"Camera center composes into reference camera coordinates");}
    auto rotated=makeWorldOrigin(pose,board,rig,1,12);cv::Mat centered=rotated.R.t()*(centerA-rotated.T);
    require(std::abs(centered.at<double>(2)-.012)<1e-6&&std::abs(centered.at<double>(0))+std::abs(centered.at<double>(1))<1e-6,"Board height offsets floor without horizontal drift");
    require(cv::norm(rotated.R.col(0)-w.R.col(1))<1e-8,"Quarter turn rotates world X onto prior Y");
    auto originalImage=sample.cameras[0].image.clone();auto drawn=drawWorldAxes(sample.cameras[0].image,a,rig,w,0,.05);
    require(cv::norm(drawn-originalImage)>0&&cv::norm(originalImage-sample.cameras[0].image)==0,"Axis overlay preserves captured raw image");
    auto noisy=sample;cv::RNG random(192);for(auto& camera:noisy.cameras)for(auto& xy:camera.imagePoints){xy.x+=float(random.gaussian(.15));xy.y+=float(random.gaussian(.15));}
    auto fit=locateBoard(noisy,board,intrinsics,rig);require(cv::norm(fit.T-tb)<.002&&*std::max_element(fit.errors.begin(),fit.errors.end())<.5,"Joint pose tolerates subpixel noise");
    auto wrong=sample;for(auto& p:wrong.cameras[2].imagePoints)p.x+=80;rejects([&]{locateBoard(wrong,board,intrinsics,rig);});
    std::stop_source stop;stop.request_stop();rejects([&]{locateBoard(sample,board,intrinsics,rig,stop.get_token());});
    rejects([&]{makeWorldOrigin(pose,board,rig,0,-1);});
    BoardConfig ch;ch.pattern=Pattern::Charuco;ch.columns=6;ch.rows=5;auto cp=locateBoard(synth(ch),ch,intrinsics,rig);auto cw=makeWorldOrigin(cp,ch,rig);
    cv::Mat cc=(cv::Mat_<double>(3,1)<<ch.columns*ch.squareMm/2000,ch.rows*ch.squareMm/2000,0);
    require(cv::norm(cw.T-cv::Mat(rb*cc+tb))<1e-6,"ChArUco partial views preserve physical grid center");
    Settings settings;for(size_t i=0;i<3;++i){SavedCamera camera;camera.identity=std::string(1,char('A'+i));camera.sourceKind="live";camera.archive="intrinsics-"+camera.identity;camera.board=board;camera.calibration=intrinsics[i];settings.cameras[camera.identity]=camera;}
    settings.rig=Settings::SavedRig{{"A","B","C"},{"intrinsics-A","intrinsics-B","intrinsics-C"},"rig-v1",board,rig};
    settings.origin=Settings::SavedOrigin{"rig-v1","origin-v1",rotated};
    settings.cameraNames={{"A","Left camera"},{"B","컬러 카메라"}};
    auto dir=std::filesystem::temp_directory_path()/("boccia-origin-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    writeSettings(dir/"settings.json",settings);auto restored=loadSettings(dir/"settings.json");
    require(restored.originCurrent()&&cv::norm(restored.origin->transform.R-rotated.R)<1e-10&&restored.origin->transform.surfaceHeightMm==12,"Origin survives restart with correct rig dependency");
    require(restored.cameraNames==settings.cameraNames,"Camera names survive restart without changing calibration identities");
    for(size_t i=0;i<3;++i){auto view=savedWorldPoseView(restored,std::string(1,char('A'+i)));
        require(view.state==RecordState::Saved&&cv::norm(view.center-rotated.cameraCenter(rig,i))<1e-10,"World popup uses saved world camera center");
        cv::Mat worldToCamera=rotated.R.clone();if(i)worldToCamera=rig.fromReference[i-1].R*worldToCamera;
        require(cv::norm(worldToCamera*view.rotation-cv::Mat::eye(3,3,CV_64F))<1e-10,"World orientation maps camera axes into floor coordinates");
        require(view.rms&&*view.rms==rotated.errors[i],"World RMS belongs to origin observation");
    }
    require(cv::norm(savedWorldPoseView(restored,"A").center)>0.1,"Reference camera is not the world origin");
    require(savedWorldPoseView(restored,"unknown").state==RecordState::Missing,"Unrelated camera has no world pose");
    auto missing=restored;missing.origin.reset();require(savedWorldPoseView(missing,"A").state==RecordState::Missing,"No origin means no world pose");
    auto stale=restored;stale.rig->archive="new-rig";auto staleView=savedWorldPoseView(stale,"A");
    require(staleView.state==RecordState::Stale&&staleView.center.empty()&&staleView.rotation.empty(),"Never compose old origin with new rig");
    stale=restored;stale.cameras["C"].archive="new-intrinsics";
    require(savedWorldPoseView(stale,"A").state==RecordState::Stale,"Any changed intrinsic invalidates world pose");
    restored.rig->archive="rig-v2";require(!restored.originCurrent(),"New rig invalidates old origin");restored=settings;restored.cameras["C"].archive="intrinsics-new";require(!restored.originCurrent(),"Any intrinsic change invalidates origin");
    auto bad=settings;bad.origin->transform.R=bad.origin->transform.R.clone();bad.origin->transform.R.setTo(0);rejects([&]{writeSettings(dir/"settings.json",bad);});
    require(loadSettings(dir/"settings.json").originCurrent(),"Rejected save preserves prior valid profile");
    RectangleOriginConfig rectangle;std::vector<cv::Point3f> rectangleObjects{{0,0,0},{.297f,0,0},{.297f,.210f,0},{0,.210f,0}};
    std::vector<cv::Point2f> savedCorners;WorldOrigin rectangleWorld;
    for(size_t camera=0;camera<3;++camera){
        cv::Mat rotation=rb.clone(),translation=tb.clone();if(camera){rotation=rig.fromReference[camera-1].R*rb;translation=rig.fromReference[camera-1].R*tb+rig.fromReference[camera-1].T;}
        std::vector<cv::Point2f> corners;cv::projectPoints(rectangleObjects,rotation,translation,intrinsics[camera].K,intrinsics[camera].distortion,corners);
        auto paperPose=locateRectangle(corners,rectangle,intrinsics[camera],rig,camera);
        require(cv::norm(paperPose.R-rb)<2e-5&&cv::norm(paperPose.T-tb)<2e-6,"Four rectangle corners from any rig camera recover reference-A pose");
        auto paperWorld=makeRectangleWorldOrigin(paperPose,rectangle,rig,1,3);
        cv::Mat paperCenter=rb*(cv::Mat_<double>(3,1)<<.297/2,.210/2,0)+tb;
        cv::Mat inWorld=paperWorld.R.t()*(paperCenter-paperWorld.T);
        require(cv::norm(inWorld-(cv::Mat_<double>(3,1)<<0,0,.003))<2e-6,"Rectangle origin uses physical center, height and quarter turns");
        for(size_t i=0;i<3;++i)require(paperWorld.cameraCenter(rig,i).at<double>(2)>0&&paperWorld.measured(i)==(i==camera),"All cameras remain above floor but only clicked camera has measured RMS");
        if(camera==1){savedCorners=corners;rectangleWorld=paperWorld;}
        auto invalidCorners=corners;std::swap(invalidCorners[1],invalidCorners[2]);rejects([&]{locateRectangle(invalidCorners,rectangle,intrinsics[camera],rig,camera);});
        invalidCorners=corners;invalidCorners[1]=invalidCorners[0];rejects([&]{locateRectangle(invalidCorners,rectangle,intrinsics[camera],rig,camera);});
        invalidCorners=corners;invalidCorners[0].x=-1;rejects([&]{locateRectangle(invalidCorners,rectangle,intrinsics[camera],rig,camera);});
    }
    auto badRectangle=rectangle;badRectangle.widthMm=0;rejects([&]{badRectangle.validate();});
    auto paperSettings=settings;paperSettings.origin=Settings::SavedOrigin{"rig-v1","paper-origin",rectangleWorld};
    paperSettings.origin->method="rectangle";paperSettings.origin->rectangle=rectangle;paperSettings.origin->referenceCamera="B";paperSettings.origin->points=savedCorners;
    writeSettings(dir/"rectangle-settings.json",paperSettings);auto paperRestored=loadSettings(dir/"rectangle-settings.json");
    require(paperRestored.originCurrent()&&paperRestored.origin->method=="rectangle"&&paperRestored.origin->referenceCamera=="B"&&paperRestored.origin->points==savedCorners,"Rectangle size, selected camera and original pixels survive restart");
    require(!savedWorldPoseView(paperRestored,"A").rms&&savedWorldPoseView(paperRestored,"B").rms&&!savedWorldPoseView(paperRestored,"C").rms,"Unobserved cameras never display fabricated RMS values");
    auto inconsistent=paperSettings;inconsistent.origin->referenceCamera="C";rejects([&]{writeSettings(dir/"rectangle-settings.json",inconsistent);});
    inconsistent=paperSettings;inconsistent.origin->transform.errors[0]=.1;rejects([&]{writeSettings(dir/"rectangle-settings.json",inconsistent);});
    auto oldPaper=paperSettings;oldPaper.rig->archive="rig-v2";writeSettings(dir/"stale-rectangle.json",oldPaper);
    require(!loadSettings(dir/"stale-rectangle.json").originCurrent(),"Rectangle origin is invalidated when relative calibration changes");
    std::cout<<"PASS: board and four-corner floor poses, any reference camera, measured-only RMS, axes, height, raw preservation, validation and persistence\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
