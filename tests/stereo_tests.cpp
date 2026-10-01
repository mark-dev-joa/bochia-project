#include "stereo.hpp"
#include "settings.hpp"
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <functional>
#include <numeric>
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void rejects(const std::function<void()>& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"Expected rejection");}
static std::vector<StereoPair> synthetic(const BoardConfig& board,const Calibration& a,const Calibration& b,const cv::Mat& R,const cv::Mat& T){
    std::vector<cv::Point3f> points;
    if(board.pattern==Pattern::Charuco)points=board.charucoBoard().getChessboardCorners();
    else for(int y=0;y<board.rows;++y)for(int x=0;x<board.columns;++x)points.emplace_back(float(x*board.squareMm/1000),float(y*board.squareMm/1000),0.f);
    std::vector<StereoPair> pairs;cv::Mat blank(a.imageSize,CV_8UC3,cv::Scalar(100,100,100));
    for(int i=0;i<18;++i){
        StereoPair p;p.first.image=blank;p.second.image=cv::Mat(b.imageSize,CV_8UC3,cv::Scalar(100,100,100));p.first.objectPoints=points;p.second.objectPoints=points;
        cv::Mat r=(cv::Mat_<double>(3,1)<<-.4+(i%5)*.17,-.32+(i%7)*.11,-.15+(i%3)*.15);
        cv::Mat t=(cv::Mat_<double>(3,1)<<-.12+(i%4)*.035,-.08+(i%6)*.017,.65+(i%5)*.07);
        cv::Mat boardR,otherR,otherRotation;cv::Rodrigues(r,boardR);otherR=R*boardR;cv::Rodrigues(otherR,otherRotation);
        cv::projectPoints(points,r,t,a.K,a.distortion,p.first.imagePoints);
        cv::projectPoints(points,otherRotation,cv::Mat(R*t+T),b.K,b.distortion,p.second.imagePoints);
        if(board.pattern==Pattern::Chessboard&&i%3==0)std::reverse(p.second.imagePoints.begin(),p.second.imagePoints.end());
        if(board.pattern==Pattern::Charuco){
            p.first.ids.resize(points.size());std::iota(p.first.ids.begin(),p.first.ids.end(),0);p.second.ids=p.first.ids;
            for(int n=0;n<2;++n){p.first.ids.erase(p.first.ids.begin());p.first.objectPoints.erase(p.first.objectPoints.begin());p.first.imagePoints.erase(p.first.imagePoints.begin());}
            for(int n=0;n<3;++n){p.second.ids.pop_back();p.second.objectPoints.pop_back();p.second.imagePoints.pop_back();}
        }
        p.first.frameId=i+1;p.second.frameId=i+501;p.receiptSkewMs=20;pairs.push_back(p);
    }
    return pairs;
}
static void checkRectification(const Calibration& a,const Calibration& b,const StereoCalibration& relation,bool vertical=false){
    auto ka=a.K.clone(),kb=b.K.clone(),da=a.distortion.clone(),db=b.distortion.clone();
    auto maps=makeStereoRectification(a,b,relation);
    require(maps.rotated==vertical,"Near-vertical baselines rotate both outputs into horizontal epipolar rows");
    for(int mode:{1,2}){auto guides=makeEpipolarGuides(a,b,maps,mode,6);int checkedGuides=0;
        for(int side=0;side<2;++side){require(guides[side].size()==6,"Corresponding line IDs persist on both images");const auto& c=side?b:a;
            cv::Mat H=maps.P[side](cv::Rect(0,0,3,3))*maps.R[side]*c.K.inv();
            for(size_t n=0;n<6;++n)for(const auto& path:guides[side][n])for(auto p:path){double y=p.y;
                if(mode==1){cv::Mat q=H*(cv::Mat_<double>(3,1)<<p.x,p.y,1);y=q.at<double>(1)/q.at<double>(2);}
                require(std::abs(y-(maps.outputSize.height-1)*double(n+1)/7)<.003,"Same color/number projects to the same epipolar row");++checkedGuides;}}
        require(checkedGuides>=12,"Epipolar guides have useful visible segments");}
    rejects([&]{makeEpipolarGuides(a,b,maps,0,4);});
    require(cv::norm(ka-a.K)==0&&cv::norm(kb-b.K)==0&&cv::norm(da-a.distortion)==0&&cv::norm(db-b.distortion)==0,"Rectification preserves native intrinsics");
    std::vector<cv::Point3d> objects;
    for(int y=-2;y<=2;++y)for(int x=-3;x<=3;++x)objects.emplace_back(x*.04,y*.04,.8+(x+3)*.07);
    std::array<std::vector<cv::Point2d>,2> pixels,rectified;
    cv::projectPoints(objects,cv::Vec3d(),cv::Vec3d(),a.K,a.distortion,pixels[0]);
    cv::projectPoints(objects,relation.R,relation.T,b.K,b.distortion,pixels[1]);
    for(int i=0;i<2;++i){
        const auto& c=i?b:a;
        cv::undistortPoints(pixels[i],rectified[i],c.K,c.distortion,maps.R[i],maps.P[i]);
    }
    int checked=0;
    for(size_t p=0;p<objects.size();++p){
        require(std::abs(rectified[0][p].y-rectified[1][p].y)<.002,"Corresponding points have matching rectified rows at varying depths");
        for(int i=0;i<2;++i){
            auto q=rectified[i][p];
            if(q.x<1||q.y<1||q.x>=maps.outputSize.width-2||q.y>=maps.outputSize.height-2)continue;
            cv::Mat sx,sy;cv::Point2f xy(float(q.x),float(q.y));
            cv::getRectSubPix(maps.mapX[i],{1,1},xy,sx);cv::getRectSubPix(maps.mapY[i],{1,1},xy,sy);
            require(cv::norm(cv::Point2d(sx.at<float>(0),sy.at<float>(0))-pixels[i][p])<.025,"Map sampling agrees with final projection matrices, including rotated output");
            ++checked;
        }
    }
    require(checked>20,"Useful central points remain visible after rectification");
    for(int i=0;i<2;++i){
        cv::Mat raw(maps.inputSize[i],CV_8UC3);cv::randu(raw,0,256);auto original=raw.clone();
        const auto corrected=rectifiedFrame(raw,maps,i);
        require(corrected.size()==maps.outputSize&&corrected.type()==raw.type()&&corrected.data!=raw.data&&cv::norm(original,raw,cv::NORM_INF)==0,"Rectification produces shared-size outputs without modifying camera frames");
    }
    rejects([&]{rectifiedFrame(cv::Mat(100,100,CV_8UC3),maps,0);});
    rejects([&]{rectifiedFrame(cv::Mat(),maps,0);});
    rejects([&]{rectifiedFrame(cv::Mat(a.imageSize,CV_8UC3),maps,2);});
}
int main(){try{
    Calibration a,b;a.imageSize=b.imageSize={1280,960};
    a.K=(cv::Mat_<double>(3,3)<<1000,0,640,0,980,480,0,0,1);b.K=(cv::Mat_<double>(3,3)<<1050,0,632,0,1020,475,0,0,1);
    a.distortion=(cv::Mat_<double>(1,5)<<-.06,.01,.001,-.0005,0);b.distortion=(cv::Mat_<double>(1,5)<<-.04,.02,-.0008,.0003,0);
    cv::Mat R,T=(cv::Mat_<double>(3,1)<<-.15,.008,.004);cv::Rodrigues(cv::Vec3d(.01,.05,-.02),R);
    BoardConfig board;board.columns=8;board.rows=6;
    auto pairs=synthetic(board,a,b,R,T);auto originalK=a.K.clone(),originalD=b.distortion.clone();
    auto result=calibrateStereo(pairs,board,a,b);
    require(result.rms<.001&&cv::norm(result.R-R)<1e-4&&cv::norm(result.T-T)<1e-5,"Recover known camera transform despite opposite chessboard numbering");
    require(result.perPairErrors.size()==pairs.size()&&result.secondRotations[0]==2&&result.secondRotations[1]==0,"Per-pair error and correspondence orientation reported");
    require(cv::norm(result.secondCenterInFirst()+R.t()*T)<1e-5,"B position is -R transpose T, not T");
    require(cv::norm(originalK-a.K)==0&&cv::norm(originalD-b.distortion)==0,"Fixed intrinsics remain untouched");
    auto noisy=pairs;cv::RNG random(104);
    for(auto& p:noisy)for(auto* o:{&p.first,&p.second})for(auto& xy:o->imagePoints){xy.x+=float(random.gaussian(.15));xy.y+=float(random.gaussian(.15));}
    auto noisyResult=calibrateStereo(noisy,board,a,b);
    require(noisyResult.rms<.4&&cv::norm(noisyResult.T-T)<.002&&cv::norm(noisyResult.R-R)<.005,"Subpixel corner noise preserves millimetre-scale synthetic baseline recovery");
    auto squareBoard=board;squareBoard.columns=squareBoard.rows=6;auto squarePairs=synthetic(squareBoard,a,b,R,T);
    for(auto& p:squarePairs){auto old=p.second.imagePoints;for(int y=0;y<6;++y)for(int x=0;x<6;++x)p.second.imagePoints[y*6+x]=old[x*6+5-y];}
    auto squareResult=calibrateStereo(squarePairs,squareBoard,a,b);
    require(cv::norm(squareResult.T-T)<1e-5,"Square chessboard quarter-turn ambiguity resolved across poses");
    require(usableStereoTiming(1,1.02,1.1)&&!usableStereoTiming(1,1.11,1.2)&&!usableStereoTiming(1,1.01,3)&&!usableStereoTiming(4,4,3),"Reject stale, mismatched, or future pair receipt times");
    auto wrong=pairs;wrong[0].receiptSkewMs=101;rejects([&]{calibrateStereo(wrong,board,a,b);});
    wrong=pairs;wrong[0].second.image=cv::Mat(100,100,CV_8UC3);rejects([&]{calibrateStereo(wrong,board,a,b);});
    rejects([&]{calibrateStereo({pairs[0]},board,a,b);});
    std::stop_source stop;stop.request_stop();rejects([&]{calibrateStereo(pairs,board,a,b,stop.get_token());});
    BoardConfig charuco;charuco.pattern=Pattern::Charuco;charuco.columns=6;charuco.rows=5;
    auto partial=synthetic(charuco,a,b,R,T);auto common=matchStereoPoints(partial[0],charuco);
    require(common.object.size()==15,"ChArUco pairs use intersection of visible marker corner IDs");
    auto charucoResult=calibrateStereo(partial,charuco,a,b);
    require(charucoResult.rms<.001&&cv::norm(charucoResult.T-T)<1e-5,"Recover transform from unequal ChArUco subsets");
    auto bad=partial[0];bad.second.ids[1]=bad.second.ids[0];rejects([&]{matchStereoPoints(bad,charuco);});
    const auto dir=std::filesystem::temp_directory_path()/("boccia-stereo-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    saveStereoArchive(dir.string(),"A","B",board,pairs,a,b,result);
    cv::FileStorage yaml((dir/"stereo.yml").string(),cv::FileStorage::READ);cv::Mat restored;yaml["T_A_to_B_m"]>>restored;
    require(cv::norm(restored-T)<1e-5&&std::filesystem::exists(dir/"A/view_0.png")&&std::filesystem::exists(dir/"B/view_0.png"),"Stereo archive includes transform and raw image pairs");
    Settings settings;SavedCamera sa;sa.identity="A";sa.sourceKind="live";sa.archive="intrinsic-A-v1";sa.calibration=a;sa.board=board;
    auto sb=sa;sb.identity="B";sb.archive="intrinsic-B-v1";sb.calibration=b;
    settings.cameras["A"]=sa;settings.cameras["B"]=sb;settings.selectedSerials={"A","B"};settings.stereoSerials={"A","B"};
    settings.stereo=SavedStereo{"A","B",sa.archive,sb.archive,dir.string(),board,result};settings.autoReconnect=true;
    writeSettings(dir/"settings.json",settings);auto loaded=loadSettings(dir/"settings.json");
    require(loaded.stereo&&loaded.stereo->matches(loaded.cameras)&&cv::norm(loaded.stereo->calibration.T-T)<1e-5,"Relative pose and intrinsic dependencies survive restart");
    require(loaded.autoReconnect&&loaded.selectedSerials==settings.selectedSerials&&loaded.stereoSerials==settings.stereoSerials,"Automatic reconnect and pair selection persist");
    loaded.cameras["A"].archive="intrinsic-A-v2";require(!loaded.stereo->matches(loaded.cameras),"Recalibrating a camera invalidates the saved relative pose");
    settings.autoReconnect=false;writeSettings(dir/"settings.json",settings);require(!loadSettings(dir/"settings.json").autoReconnect,"Automatic connection can be disabled persistently");
    auto invalid=settings;invalid.stereo->calibration.R=invalid.stereo->calibration.R.clone();invalid.stereo->calibration.R.setTo(0);rejects([&]{writeSettings(dir/"settings.json",invalid);});
    require(loadSettings(dir/"settings.json").stereo->matches(settings.cameras),"Invalid stereo save preserves existing settings");
    Calibration color=b;color.imageSize={1600,1200};color.K=(cv::Mat_<double>(3,3)<<1200,0,800,0,1190,600,0,0,1);
    cv::Mat Rc,Tc=(cv::Mat_<double>(3,1)<<.08,-.06,.025);cv::Rodrigues(cv::Vec3d(-.03,.02,.07),Rc);
    auto colorPairs=synthetic(board,a,color,Rc,Tc);std::vector<RigSample> groups;
    for(size_t i=0;i<pairs.size();++i)groups.push_back({{pairs[i].first,pairs[i].second,colorPairs[i].second},35});
    auto rig=calibrateRig(groups,board,{a,b,color});
    require(rig.fromReference.size()==2&&cv::norm(rig.fromReference[0].T-T)<1e-5&&cv::norm(rig.fromReference[1].T-Tc)<1e-5&&cv::norm(rig.fromReference[1].R-Rc)<1e-4,"Three-camera groups recover A-B and A-C with mixed native resolutions");
    require(usableRigTiming({1,1.02,1.06},1.1)&&!usableRigTiming({1,1.02,1.15},1.2)&&!usableRigTiming({1,1.02,3},3.1),"Full group receipt span includes third camera");
    auto missing=groups;missing[3].cameras.pop_back();rejects([&]{calibrateRig(missing,board,{a,b,color});});
    missing=groups;missing[3].cameras[2].imagePoints.clear();rejects([&]{calibrateRig(missing,board,{a,b,color});});
    auto brokenColor=color;brokenColor.K=cv::Mat();rejects([&]{calibrateRig(groups,board,{a,b,brokenColor});});
    rejects([&]{calibrateRig(groups,board,{a,b,color},stop.get_token());});
    RigCapture capture;AutoCaptureConfig config{10,1.0,1.5};capture.start(3,config);
    auto observe=[&](const RigSample& group,double t){return capture.observe(group,board,{}, {t,t+.01,t+.02},t+.03);};
    require(!observe(groups[0],10)&&!observe(groups[0],10.4),"All-camera collection waits for stable hold");
    auto invisible=groups[0];invisible.cameras[2].imagePoints.clear();
    require(!observe(invisible,10.8)&&capture.progress()==0,"Third-camera board loss resets all holds");
    require(!observe(groups[0],11.2)&&!observe(groups[0],11.6)&&!observe(groups[0],12.0)&&observe(groups[0],12.4),"Only three stable valid cameras produce one complete group");
    capture.captured(12.43,10);require(!capture.running(),"Group target stops all cameras together");
    capture.start(3,config);require(!capture.observe(groups[0],board,{}, {20,20.01,19.0},20.03),"Stale third camera cannot generate two-camera subset");
    capture.stop();require(!observe(groups[0],21),"Stop cannot append late group");
    auto rigDir=dir/"three-camera";saveRigArchive(rigDir.string(),{"A","B","C"},board,groups,{a,b,color},rig);
    require(std::filesystem::exists(rigDir/"A/view_17.png")&&std::filesystem::exists(rigDir/"B/view_17.png")&&std::filesystem::exists(rigDir/"C/view_17.png"),"All three originals archived in matching group slots");
    cv::FileStorage rigYaml((rigDir/"rig.yml").string(),cv::FileStorage::READ);rigYaml["from_reference"][1]["T_m"]>>restored;
    require(cv::norm(restored-Tc)<1e-5,"Third-camera transform archived");
    auto sc=sb;sc.identity="C";sc.archive="intrinsic-C-v1";sc.calibration=color;settings.cameras["C"]=sc;
    settings.rig=Settings::SavedRig{{"A","B","C"},{sa.archive,sb.archive,sc.archive},rigDir.string(),board,rig};settings.relativeCameraCount=3;
    settings.stereoSerials={"A","B","C"};writeSettings(dir/"settings.json",settings);loaded=loadSettings(dir/"settings.json");
    require(loaded.rig&&loaded.rig->matches(loaded.cameras)&&loaded.rig->serials.size()==3&&loaded.relativeCameraCount==3&&loaded.stereo.has_value(),"Three-camera rig round trip preserves earlier two-camera result");
    loaded.cameras["C"].archive="intrinsic-C-v2";require(!loaded.rig->matches(loaded.cameras),"Third-camera intrinsic change invalidates rig");
    auto corrupt=settings;corrupt.rig->serials[2]="A";rejects([&]{writeSettings(dir/"settings.json",corrupt);});
    require(loadSettings(dir/"settings.json").rig->serials[2]=="C","Invalid rig update preserves saved group");
    require(calibrationState(settings,"A")==RecordState::Saved&&calibrationState(settings,"unknown")==RecordState::Missing,"Cal distinguishes saved and missing records without a live device");
    std::map<std::string,std::string> changedGeometry{{"Width","999"}};
    require(calibrationState(settings,"A",&changedGeometry)==RecordState::Stale,"Cal marks mismatched live geometry stale");
    auto reference=savedPoseView(settings,"A"),poseB=savedPoseView(settings,"B"),poseC=savedPoseView(settings,"C");
    require(reference.state==RecordState::Saved&&!reference.rms&&cv::norm(reference.center)==0,"Reference camera has identity pose without fabricated RMS");
    require(poseB.state==RecordState::Saved&&poseC.state==RecordState::Saved&&poseB.reference=="A"&&
        cv::norm(poseB.center-result.secondCenterInFirst())<1e-4&&cv::norm(poseC.rotation-rig.fromReference[1].R.t())<1e-8,"Pose popup reports camera center and camera-to-reference rotation");
    require(savedPoseView(settings,"unknown").state==RecordState::Missing,"Unrelated camera has no pose");
    require(savedPoseView(loaded,"A").state==RecordState::Stale&&savedPoseView(loaded,"B").state==RecordState::Stale,"Changed C intrinsics invalidate all dependent rig poses including reference");
    auto legacy=settings;legacy.rig.reset();require(savedPoseView(legacy,"B").state==RecordState::Saved&&savedPoseView(legacy,"C").state==RecordState::Missing,"Legacy pair poses remain inspectable");
    std::array<cv::Mat,3> nativeR{cv::Mat::eye(3,3,CV_64F),rig.fromReference[0].R,rig.fromReference[1].R};
    std::array<cv::Mat,3> nativeT{cv::Mat::zeros(3,1,CV_64F),rig.fromReference[0].T,rig.fromReference[1].T};
    const std::array<std::string,3> serials{"A","B","C"};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)if(i!=j){
        auto relation=savedStereoTransform(settings,serials[i],serials[j]);
        cv::Mat expectedR=nativeR[j]*nativeR[i].t(),expectedT=nativeT[j]-expectedR*nativeT[i];
        require(cv::norm(relation.R-expectedR)<1e-10&&cv::norm(relation.T-expectedT)<1e-10,"All rig pairs and reversed orders compose from camera extrinsics");
        checkRectification(settings.cameras.at(serials[i]).calibration,settings.cameras.at(serials[j]).calibration,relation);
    }
    auto reverse=savedStereoTransform(legacy,"B","A");
    require(cv::norm(reverse.R*result.R-cv::Mat::eye(3,3,CV_64F))<1e-8&&cv::norm(reverse.T+reverse.R*result.T)<1e-8,"Legacy reverse pair inverts both rotation and translation");
    require(cv::norm(savedStereoTransform(legacy,"A","B").T-result.T)<1e-10,"Valid legacy pair remains usable without a rig or world origin");
    rejects([&]{savedStereoTransform(settings,"A","A");});
    rejects([&]{savedStereoTransform(settings,"A","unknown");});
    rejects([&]{savedStereoTransform(legacy,"B","C");});
    rejects([&]{savedStereoTransform(loaded,"A","B");}); // C changed: cannot silently use older legacy pair.
    auto stale=legacy;stale.cameras["B"].archive="new-intrinsics";
    rejects([&]{savedStereoTransform(stale,"A","B");});
    stale=legacy;stale.cameras["B"].sourceKind="offline";
    rejects([&]{savedStereoTransform(stale,"A","B");});
    auto wide=color;wide.imageSize={1600,900};wide.K=wide.K.clone();wide.K.at<double>(1,2)=450;
    StereoCalibration vertical;vertical.R=R.clone();vertical.T=(cv::Mat_<double>(3,1)<<.007,-.17,.002);
    checkRectification(a,wide,vertical,true);
    StereoCalibration horizontal;horizontal.R=R.clone();horizontal.T=T.clone();checkRectification(a,wide,horizontal);
    std::cout<<"PASS: stereo recovery, correspondence, timing, cancellation, archives, all rig pairs, mixed-resolution horizontal rectification, native frame preservation and stale dependency checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
