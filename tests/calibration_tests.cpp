#include "calibration.hpp"
#include "settings.hpp"
#include "background_task.hpp"
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <iostream>
#include <chrono>
#include <functional>
#include <limits>
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static void rejects(const std::function<void()>& f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,"Expected validation failure");}
int main(){try{
    // Cancellation must return control before the worker completes, and a late
    // result (or exception) must never update the replacement UI session.
    BackgroundTask<int> task;
    std::promise<void> release;
    auto gate=release.get_future().share();
    task.start([gate](std::stop_token){gate.wait();return 42;});
    task.cancel();
    require(!task.active()&&task.pending(),"Cancellation releases UI while worker drains");
    rejects([&]{task.start([](std::stop_token){return 99;});});
    release.set_value();task.wait();
    require(!task.poll().has_value()&&!task.pending(),"Cancelled late result discarded");
    task.start([](std::stop_token)->int{throw std::runtime_error("obsolete error");});
    task.cancel();task.wait();require(!task.poll().has_value(),"Cancelled error discarded");
    task.start([](std::stop_token){return 7;});task.wait();
    require(task.poll()==7,"Task reusable after cancellation");
    BoardConfig board;board.validate();
    cv::Mat chess(560,800,CV_8UC3,cv::Scalar(220,220,220));
    for(int y=0;y<7;++y)for(int x=0;x<10;++x)cv::rectangle(chess,{100+x*55,75+y*55,55,55},(x+y)%2?cv::Scalar(255,255,255):cv::Scalar(0,0,0),cv::FILLED);
    auto observation=detectBoard(chess,board);require(observation.imagePoints.size()==54,"Chessboard detection");
    auto chessOriginal=chess.clone();auto overlay=drawBoardOverlay(chess,observation,board);
    const auto horizontal=(observation.imagePoints[0]+observation.imagePoints[1])*.5f;
    const auto vertical=(observation.imagePoints[0]+observation.imagePoints[board.columns])*.5f;
    auto gridVisible=[&](cv::Point2f p){auto pixel=overlay.at<cv::Vec3b>(cvRound(p.y),cvRound(p.x));return pixel[1]>pixel[0]+50&&pixel[1]>pixel[2]+50;};
    require(gridVisible(horizontal)&&gridVisible(vertical),"Detected chessboard preview includes both horizontal and vertical grid edges");
    require(overlay.data!=chess.data&&cv::norm(chess,chessOriginal,cv::NORM_INF)==0,"Board overlay preserves calibration and acquisition originals");
    auto incomplete=observation;incomplete.imagePoints.pop_back();auto partialOverlay=drawBoardOverlay(chess,incomplete,board);
    require(partialOverlay.at<cv::Vec3b>(cvRound(horizontal.y),cvRound(horizontal.x))==chess.at<cv::Vec3b>(cvRound(horizontal.y),cvRound(horizontal.x)),"Incomplete board cannot draw a misleading connected grid");
    cv::Mat dim;chess.convertTo(dim,CV_8UC3,80.0/255,30);
    auto dimOriginal=dim.clone();auto dimBoard=detectBoard(dim,board);
    auto brightness=estimateBoardBrightness(dimBoard);
    require(brightness.enabled&&brightness.fitted&&brightness.blackLevel==30&&brightness.whiteLevel==110,"Fit black and white from dim board interior");
    auto enhanced=applySoftwareBrightness(dim,brightness);
    require(enhanced.at<cv::Vec3b>(100,125)==cv::Vec3b(16,16,16)&&enhanced.at<cv::Vec3b>(100,180)==cv::Vec3b(240,240,240),"Fixed contrast mapping lifts white squares");
    require(cv::norm(dim,dimOriginal,cv::NORM_INF)==0&&enhanced.data!=dim.data,"Brightness leaves original untouched");
    auto otherBackground=dimBoard;otherBackground.image=dim.clone();otherBackground.image.rowRange(0,65).setTo(cv::Scalar(255,255,255));
    auto otherFit=estimateBoardBrightness(otherBackground);
    require(otherFit.blackLevel==brightness.blackLevel&&otherFit.whiteLevel==brightness.whiteLevel,"Background excluded from fit");
    auto fixedAfter=applySoftwareBrightness(otherBackground.image,brightness);
    require(cv::norm(fixedAfter(cv::Rect(100,75,550,385)),enhanced(cv::Rect(100,75,550,385)),cv::NORM_INF)==0,"Later background changes never adapt fixed transform");
    auto correctedBoard=detectBoard(dim,board,brightness);
    require(correctedBoard.imagePoints.size()==54&&correctedBoard.softwareBrightness.blackLevel==30&&cv::norm(correctedBoard.image,dimOriginal,cv::NORM_INF)==0,"Detector uses processed image and retains original with settings snapshot");
    auto disabled=brightness;disabled.enabled=false;
    require(cv::norm(applySoftwareBrightness(dim,disabled),dim,cv::NORM_INF)==0,"Software correction can be disabled");
    auto flat=dimBoard;flat.image=cv::Mat(dim.size(),dim.type(),cv::Scalar(70,70,70));rejects([&]{estimateBoardBrightness(flat);});
    auto invalidBrightness=brightness;invalidBrightness.fitted=false;rejects([&]{invalidBrightness.validate();});
    cv::Mat displayPixels=(cv::Mat_<cv::Vec3b>(1,4)<<cv::Vec3b(0,32,64),cv::Vec3b(96,128,160),
        cv::Vec3b(192,224,255),cv::Vec3b(127,128,129));
    const auto displaySource=displayPixels.clone();
    auto identityDisplay=adjustDisplay(displayPixels,0,1);
    require(identityDisplay.type()==CV_8UC3&&cv::norm(identityDisplay,displaySource,cv::NORM_INF)==0,
        "Neutral display settings preserve BGR8 values exactly");
    auto brightDisplay=adjustDisplay(displayPixels,20,1);
    require(brightDisplay.at<cv::Vec3b>(0,0)==cv::Vec3b(20,52,84)&&brightDisplay.at<cv::Vec3b>(0,2)==cv::Vec3b(212,244,255),
        "Display brightness is additive per channel and saturates at white");
    auto contrastDisplay=adjustDisplay(displayPixels,0,2);
    require(contrastDisplay.at<cv::Vec3b>(0,0)==cv::Vec3b(0,0,0)&&contrastDisplay.at<cv::Vec3b>(0,1)==cv::Vec3b(64,128,192)&&
        contrastDisplay.at<cv::Vec3b>(0,2)==cv::Vec3b(255,255,255)&&contrastDisplay.at<cv::Vec3b>(0,3)==cv::Vec3b(126,128,130),
        "Display contrast pivots at 128 and clips highlights and shadows without wrapping");
    require(brightDisplay.data!=displayPixels.data&&contrastDisplay.data!=displayPixels.data&&
        cv::norm(displayPixels,displaySource,cv::NORM_INF)==0,"Display changes never mutate raw calibration or acquisition pixels");
    cv::Mat grayDisplay(1,1,CV_8UC1,cv::Scalar(128));
    require(adjustDisplay(grayDisplay,-20,.25).at<uchar>(0,0)==108,"Display adjustment also supports mono input");
    rejects([&]{adjustDisplay(displayPixels,101,1);});rejects([&]{adjustDisplay(displayPixels,0,0);});
    rejects([&]{adjustDisplay(displayPixels,std::numeric_limits<double>::quiet_NaN(),1);});
    rejects([&]{adjustDisplay(cv::Mat(2,2,CV_16UC1),0,1);});
    cv::Mat colored(chess.size(),CV_8UC3,cv::Scalar(160,170,180));
    for(int y=0;y<7;++y)for(int x=0;x<10;++x)cv::rectangle(colored,{100+x*55,75+y*55,55,55},(x+y)%2?cv::Scalar(210,235,255):cv::Scalar(10,20,30),cv::FILLED);
    auto colorObservation=detectBoard(colored,board);
    require(colorObservation.imagePoints.size()==54,"Color camera chessboard detection");
    require(colorObservation.image.channels()==3&&cv::norm(colorObservation.image,colored,cv::NORM_INF)==0,"Color source preserved while detection uses grayscale");
    auto colorFit=estimateBoardBrightness(colorObservation);auto enhancedColor=applySoftwareBrightness(colored,colorFit);
    require(enhancedColor.channels()==3&&enhancedColor.at<cv::Vec3b>(100,180)[0]<enhancedColor.at<cv::Vec3b>(100,180)[2],"Color correction retains separate BGR channels");
    AutoCapture capture;AutoCaptureConfig autoConfig;autoConfig.target=10;
    capture.start(autoConfig);
    std::vector<Observation> captured;
    require(!capture.observe(observation,board,captured,0,0),"Auto capture waits for stable pose");
    require(!capture.observe(observation,board,captured,.35,.35),"Two frames alone cannot capture");
    require(capture.observe(observation,board,captured,.75,.75),"Stable detected board is captured");
    captured.push_back(observation);capture.captured(.75,captured.size());
    require(!capture.observe(observation,board,captured,2.5,2.5)&&capture.status()==CaptureStatus::MoveBoard,"Existing pose rejected");
    auto reversed=observation;std::reverse(reversed.imagePoints.begin(),reversed.imagePoints.end());
    require(!capture.observe(reversed,board,captured,2.6,2.6)&&capture.status()==CaptureStatus::MoveBoard,"Chessboard numbering reversal is not a new pose");
    auto moved=observation;for(auto& p:moved.imagePoints)p.x+=70;
    require(!capture.observe(moved,board,captured,2.7,2.7),"New pose starts settling");
    require(!capture.observe(moved,board,captured,3.05,3.05),"New pose still settling");
    require(capture.observe(moved,board,captured,3.45,3.45),"Different stable pose accepted");
    captured.push_back(moved);capture.captured(3.45,captured.size());
    auto third=moved;for(auto& p:third.imagePoints)p.y+=70;
    require(!capture.observe(third,board,captured,3.6,3.6)&&capture.status()==CaptureStatus::Cooldown,"Minimum interval enforced");
    capture.stop();require(!capture.observe(third,board,captured,10,10),"Stop prevents late capture");
    capture.start(autoConfig);
    require(!capture.observe(observation,board,{},20,22),"Stale worker result cannot capture");
    capture.observe(observation,board,{},23,23);
    require(!capture.observe(observation,board,{},23,23.8),"Repeated frame cannot count as stable frames");
    capture.tick(25);require(capture.status()==CaptureStatus::Stale&&capture.progress()==0,"Stalled camera resets settling");
    capture.start(autoConfig);
    for(int i=0;i<15;++i){auto drift=observation;for(auto& p:drift.imagePoints)p.x+=float(i)*.9f;
        require(!capture.observe(drift,board,{},30+i*.2,30+i*.2),"Slow drift must not accumulate a stationary hold");}
    capture.start(autoConfig);
    capture.observe(observation,board,{},40,40);capture.observe(observation,board,{},40.4,40.4);
    auto missing=observation;missing.imagePoints.clear();missing.objectPoints.clear();
    require(!capture.observe(missing,board,{},40.5,40.5)&&capture.progress()==0,"Lost board resets settling");
    require(!capture.observe(observation,board,{},40.8,40.8),"Reappearing board starts new hold");
    capture.captured(41,10);require(!capture.running()&&capture.status()==CaptureStatus::Complete,"Target count stops capture");
    require(!capture.observe(third,board,{},42,42),"Completed session cannot add extra frames");
    BoardConfig cb;cb.pattern=Pattern::Charuco;cb.columns=6;cb.rows=5;cb.squareMm=250;cb.markerMm=175;
    cv::Mat ci;cb.charucoBoard().generateImage({1200,1000},ci,40);auto co=detectBoard(ci,cb);require(co.ids.size()==20,"Charuco detection / board definition compatibility");
    auto charucoBrightness=estimateBoardBrightness(co);require(charucoBrightness.blackLevel==0&&charucoBrightness.whiteLevel==255,"One-shot brightness also supports ChArUco");
    auto partial=co;partial.imagePoints.erase(partial.imagePoints.begin(),partial.imagePoints.begin()+5);
    partial.objectPoints.erase(partial.objectPoints.begin(),partial.objectPoints.begin()+5);partial.ids.erase(partial.ids.begin(),partial.ids.begin()+5);
    capture.start(autoConfig);
    require(!capture.observe(partial,cb,{co},0,0)&&capture.status()==CaptureStatus::MoveBoard,"Changing ChArUco visibility does not create duplicate poses");
    auto invalid=board;invalid.squareMm=0;rejects([&]{invalid.validate();});
    auto mixed=observation;mixed.image=cv::Mat(500,700,CV_8UC3);rejects([&]{validateSample(mixed,{observation});});
    rejects([&]{calibrate({observation});});
    // Recover a known camera from perspective-diverse synthetic observations; no detector circularity.
    cv::Mat K=(cv::Mat_<double>(3,3)<<1000,0,640,0,980,480,0,0,1);
    cv::Mat D=(cv::Mat_<double>(1,5)<<-.08,.015,.001,-.0005,0);
    std::vector<Observation> samples;
    for(int i=0;i<22;++i){Observation s;s.image=cv::Mat(960,1280,CV_8UC3,cv::Scalar(180,180,180));
        s.objectPoints=observation.objectPoints;
        cv::Vec3d r(-.4+(i%5)*.17,-.32+(i%7)*.11,-.12+(i%3)*.1),t(-.16+(i%4)*.025,-.10+(i%6)*.016,.65+(i%5)*.065);
        cv::projectPoints(s.objectPoints,r,t,K,D,s.imagePoints);s.source="synthetic-validation";samples.push_back(s);}
    auto c=calibrate(samples);require(c.rms<.001,"Known-camera RMS");require(cv::norm(c.K,K)<.1,"Recovered camera intrinsics");
    require(c.perViewErrors.size()==samples.size(),"Per-view RMS");
    Calibration previewCalibration=c;previewCalibration.imageSize=observation.image.size();
    auto original=observation.image.clone();cv::Mat sharedPreview=observation.image;
    sharedPreview=undistortFrame(observation.image,previewCalibration);
    require(sharedPreview.data!=observation.image.data&&sharedPreview.size()==observation.image.size(),"Undistort uses separate output even when preview previously aliased source");
    require(cv::norm(original,observation.image,cv::NORM_INF)==0,"Correction does not mutate captured source");
    rejects([&]{undistortFrame(cv::Mat(10,10,CV_8UC3),previewCalibration);});
    auto dir=std::filesystem::temp_directory_path()/("boccia-tests-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    samples[0].softwareBrightness=brightness;
    std::filesystem::create_directories(dir);saveCalibration((dir/"capture").string(),"camera-A",board,samples,c);
    cv::FileStorage yaml((dir/"capture/calibration.yml").string(),cv::FileStorage::READ);cv::Mat restored;yaml["camera_matrix"]>>restored;
    require(cv::norm(restored,c.K)<1e-8,"YAML calibration export");
    require(int(yaml["views"][0]["software_brightness_enabled"])==1&&int(yaml["views"][0]["software_black_level"])==30&&int(yaml["views"][0]["software_white_level"])==110,"Archive records per-sample fixed processing levels");
    require(cv::norm(cv::imread((dir/"capture/view_0.png").string()),samples[0].image,cv::NORM_INF)==0,"Archived PNG remains original despite enabled processing");
    Settings settings;settings.board=board;settings.boardConfirmed=true;settings.autoCapture={30,1.1,2.0};
    settings.softwareBrightness["camera-A"]=brightness;settings.softwareBrightness["camera-color"]=colorFit;
    SavedCamera camera;camera.identity="camera-A";camera.sourceKind="live";camera.board=board;camera.calibration=c;camera.archive=(dir/"capture").string();
    settings.cameras[camera.identity]=camera;auto profile=dir/"settings.json";writeSettings(profile,settings);
    camera.identity="camera-B";settings.cameras[camera.identity]=camera;writeSettings(profile,settings);
    camera.identity="camera-color";camera.model="synthetic-color-camera";camera.calibration.K=c.K.clone();camera.calibration.K.at<double>(0,0)+=100;
    settings.cameras[camera.identity]=camera;settings.selectedSerials={"camera-A","camera-B","camera-color"};writeSettings(profile,settings);
    auto loaded=loadSettings(profile);require(loaded.cameras.size()==3&&loaded.selectedSerials.size()==3,"Single profile retains three cameras and selection");
    require(loaded.softwareBrightness.size()==2&&loaded.softwareBrightness.at("camera-A").blackLevel==30&&loaded.softwareBrightness.at("camera-A").whiteLevel==110&&loaded.softwareBrightness.at("camera-color").enabled,"Per-camera fixed brightness survives restart");
    auto invalidFit=settings;invalidFit.softwareBrightness["camera-A"].whiteLevel=31;rejects([&]{writeSettings(profile,invalidFit);});
    require(loadSettings(profile).softwareBrightness.at("camera-A").whiteLevel==110,"Invalid contrast save preserves previous profile");
    require(cv::norm(loaded.cameras.at("camera-B").calibration.K-c.K)<1e-8&&loaded.cameras.at("camera-color").calibration.K.at<double>(0,0)>c.K.at<double>(0,0)+99,"Color calibration does not replace mono calibration");
    require(loaded.autoCapture.target==30&&loaded.autoCapture.holdSeconds==1.1&&loaded.autoCapture.intervalSeconds==2.0,"Automatic capture settings round-trip");
    auto invalidAuto=settings;invalidAuto.autoCapture.target=0;rejects([&]{writeSettings(profile,invalidAuto);});
    require(loadSettings(profile).autoCapture.target==30,"Invalid automatic settings preserve profile");
    {std::ifstream in(profile);nlohmann::json old;in>>old;old.erase("auto_capture");old.erase("software_brightness");std::ofstream out(dir/"legacy.json");out<<old;}
    require(loadSettings(dir/"legacy.json").softwareBrightness.empty(),"Old profiles default to no software processing");
    require(loadSettings(dir/"legacy.json").autoCapture.target==25,"Existing settings remain compatible");
    require(loaded.boardConfirmed&&cv::norm(loaded.cameras.at("camera-A").calibration.K-c.K)<1e-8,"Settings restart round-trip");
    auto bad=settings;bad.cameras["camera-A"].calibration.K=cv::Mat();rejects([&]{writeSettings(profile,bad);});
    require(loadSettings(profile).cameras.size()==3,"Invalid save must preserve existing settings");
    {std::ofstream out(dir/"broken.json");out<<"{broken";}rejects([&]{loadSettings(dir/"broken.json");});
    std::cout<<"PASS: chessboard, ChArUco, invalid inputs, mixed resolution, known intrinsics, YAML, two-camera settings, failed-write preservation, corrupt settings\n";
    std::cout<<"Test artifacts: "<<dir.string()<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
