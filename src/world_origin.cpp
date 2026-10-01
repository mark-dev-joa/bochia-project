#include "world_origin.hpp"
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <algorithm>
#include <set>

namespace {
void checkStop(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("원점 측정을 취소했습니다.");}
void cameraTransform(const RigCalibration& rig,size_t i,cv::Mat& r,cv::Mat& t){
    if(i>rig.fromReference.size())throw std::runtime_error("Invalid origin camera index.");
    if(i==0){r=cv::Mat::eye(3,3,CV_64F);t=cv::Mat::zeros(3,1,CV_64F);}
    else {r=rig.fromReference[i-1].R;t=rig.fromReference[i-1].T;}
}
struct Fit:cv::LMSolver::Callback {
    std::vector<std::vector<cv::Point3d>> objects;
    std::vector<std::vector<cv::Point2f>> images;
    std::vector<Calibration> intrinsics;RigCalibration rig;std::stop_token stop;
    cv::Mat residual(const cv::Mat& p)const {
        checkStop(stop);cv::Mat rb;cv::Rodrigues(p.rowRange(0,3),rb);
        size_t n=0;for(auto& v:images)n+=v.size();cv::Mat e(int(n*2),1,CV_64F);int k=0;
        for(size_t i=0;i<images.size();++i){
            cv::Mat r,t,rv;cameraTransform(rig,i,r,t);cv::Mat rotation=r*rb,translation=r*p.rowRange(3,6)+t;cv::Rodrigues(rotation,rv);
            std::vector<cv::Point2d> projected;cv::projectPoints(objects[i],rv,translation,intrinsics[i].K,intrinsics[i].distortion,projected);
            for(size_t j=0;j<projected.size();++j){e.at<double>(k++)=projected[j].x-images[i][j].x;e.at<double>(k++)=projected[j].y-images[i][j].y;}
        }return e;
    }
    bool compute(cv::InputArray param,cv::OutputArray err,cv::OutputArray jac)const override {
        cv::Mat p=param.getMat(),e=residual(p);e.copyTo(err);
        if(jac.needed()){
            jac.create(e.rows,6,CV_64F);cv::Mat j=jac.getMat();
            for(int c=0;c<6;++c){auto plus=p.clone(),minus=p.clone();double h=1e-6;
                plus.at<double>(c)+=h;minus.at<double>(c)-=h;cv::Mat d=(residual(plus)-residual(minus))/(2*h);d.copyTo(j.col(c));}
        }return true;
    }
};
}
void RectangleOriginConfig::validate()const{
    if(!std::isfinite(widthMm)||!std::isfinite(heightMm)||widthMm<10||heightMm<10||widthMm>100000||heightMm>100000)
        throw std::runtime_error("가로·세로 크기는 10~100000 mm로 입력하세요.");
}
bool WorldOrigin::measured(size_t camera)const{
    return camera<errors.size()&&(measuredCameras.empty()||std::find(measuredCameras.begin(),measuredCameras.end(),int(camera))!=measuredCameras.end());
}
void WorldOrigin::validate()const {
    if(R.type()!=CV_64F||R.rows!=3||R.cols!=3||T.type()!=CV_64F||T.rows!=3||T.cols!=1||!cv::checkRange(R)||!cv::checkRange(T)||
       std::abs(cv::determinant(R)-1)>1e-6||cv::norm(R.t()*R-cv::Mat::eye(3,3,CV_64F))>1e-6||
       quarterTurns<0||quarterTurns>3||!std::isfinite(surfaceHeightMm)||surfaceHeightMm<0||surfaceHeightMm>1000||errors.size()<2||errors.size()>3)
        throw std::runtime_error("Invalid world origin transform.");
    for(double e:errors)if(!std::isfinite(e)||e<0)throw std::runtime_error("Invalid world origin error.");
    std::set<int> observed;for(int i:measuredCameras)if(i<0||size_t(i)>=errors.size()||!observed.insert(i).second)throw std::runtime_error("Invalid origin measured camera index.");
    for(size_t i=0;i<errors.size();++i)if(!measured(i)&&errors[i]!=0)throw std::runtime_error("Unobserved camera cannot have an origin measurement error.");
}
cv::Mat WorldOrigin::cameraCenter(const RigCalibration& rig,size_t camera)const {
    validate();cv::Mat r,t;cameraTransform(rig,camera,r,t);cv::Mat center=-r.t()*t;return R.t()*(center-T);
}
BoardPose locateBoard(const RigSample& sample,const BoardConfig& board,const std::vector<Calibration>& intrinsics,const RigCalibration& rig,std::stop_token stop){
    checkStop(stop);board.validate();rig.validate();validateRigSample(sample,board,intrinsics.size());
    if(intrinsics.size()!=rig.fromReference.size()+1)throw std::runtime_error("Origin requires every saved rig camera.");
    auto fit=cv::makePtr<Fit>();fit->intrinsics=intrinsics;fit->rig=rig;fit->stop=stop;
    for(size_t i=0;i<intrinsics.size();++i)if(sample.cameras[i].image.size()!=intrinsics[i].imageSize)throw std::runtime_error("Origin image resolution differs from calibration.");
    const auto& a=sample.cameras[0];cv::Mat rv,t,rb;
    if(!cv::solvePnP(a.objectPoints,a.imagePoints,intrinsics[0].K,intrinsics[0].distortion,rv,t))throw std::runtime_error("보드 자세를 구하지 못했습니다.");
    cv::Rodrigues(rv,rb);fit->objects.emplace_back(a.objectPoints.begin(),a.objectPoints.end());fit->images.push_back(a.imagePoints);
    BoardPose result;result.cornerRotations.push_back(0);
    for(size_t i=1;i<intrinsics.size();++i){
        checkStop(stop);double best=std::numeric_limits<double>::infinity();StereoPoints selected;int chosen=0;
        auto& edge=rig.fromReference[i-1];cv::Mat rotation=edge.R*rb,translation=edge.R*t+edge.T,otherRv;cv::Rodrigues(rotation,otherRv);
        for(int turn=0;turn<4;++turn){
            if(board.pattern==Pattern::Charuco&&turn>0)continue;
            if(board.pattern==Pattern::Chessboard&&board.columns!=board.rows&&turn%2)continue;
            auto points=matchStereoPoints({a,sample.cameras[i],sample.receiptSkewMs},board,turn);
            std::vector<cv::Point2f> projected;cv::projectPoints(points.object,otherRv,translation,intrinsics[i].K,intrinsics[i].distortion,projected);
            double error=cv::norm(projected,points.second,cv::NORM_L2SQR)/points.second.size();
            if(error<best){best=error;selected=std::move(points);chosen=turn;}
        }
        fit->objects.emplace_back(selected.object.begin(),selected.object.end());fit->images.push_back(selected.second);result.cornerRotations.push_back(chosen);
    }
    cv::Mat param(6,1,CV_64F);rv.copyTo(param.rowRange(0,3));t.copyTo(param.rowRange(3,6));
    double before=cv::norm(fit->residual(param));cv::LMSolver::create(fit,60,1e-10)->run(param);checkStop(stop);
    auto residual=fit->residual(param);if(!cv::checkRange(param)||cv::norm(residual)>before+1e-6)throw std::runtime_error("원점 최적화가 수렴하지 않았습니다.");
    cv::Rodrigues(param.rowRange(0,3),result.R);result.T=param.rowRange(3,6).clone();int offset=0;
    for(size_t i=0;i<intrinsics.size();++i){
        cv::Mat r,offsetT;cameraTransform(rig,i,r,offsetT);cv::Mat rotation=r*result.R,translation=r*result.T+offsetT;
        for(const auto& p:fit->objects[i])if(rotation.at<double>(2,0)*p.x+rotation.at<double>(2,1)*p.y+rotation.at<double>(2,2)*p.z+translation.at<double>(2)<=0)
            throw std::runtime_error("보드가 카메라 뒤쪽으로 계산되었습니다.");
        int rows=int(fit->images[i].size()*2);double error=cv::norm(residual.rowRange(offset,offset+rows))/std::sqrt(double(fit->images[i].size()));offset+=rows;
        if(error>5)throw std::runtime_error("카메라 간 보드 위치 오차가 5px를 넘습니다. 보드를 고정하고 개별·상대 보정 상태를 확인하세요.");
        result.errors.push_back(error);
    }return result;
}
namespace {
WorldOrigin originFromPlane(const BoardPose& pose,const cv::Mat& center,const RigCalibration& rig,int turns,double height){
    rig.validate();
    cv::Mat origin=pose.R*center+pose.T,cameraMean=cv::Mat::zeros(3,1,CV_64F);
    for(const auto& edge:rig.fromReference)cameraMean+=edge.secondCenterInFirst();cameraMean/=double(rig.fromReference.size()+1);
    cv::Mat basis=pose.R.clone();cv::Mat toward=cameraMean-origin;
    if(basis.col(2).dot(toward)<0){basis.col(1)*=-1;basis.col(2)*=-1;}
    double angle=turns*CV_PI/2;cv::Mat rotate=(cv::Mat_<double>(3,3)<<std::cos(angle),-std::sin(angle),0,std::sin(angle),std::cos(angle),0,0,0,1);
    WorldOrigin result;result.R=basis*rotate;result.T=origin-basis.col(2)*(height/1000);result.errors=pose.errors;result.measuredCameras=pose.measuredCameras;
    result.quarterTurns=turns;result.surfaceHeightMm=height;result.validate();return result;
}
}
WorldOrigin makeWorldOrigin(const BoardPose& pose,const BoardConfig& board,const RigCalibration& rig,int turns,double height){
    board.validate();
    double dx=board.pattern==Pattern::Chessboard?board.columns-1:board.columns,dy=board.pattern==Pattern::Chessboard?board.rows-1:board.rows;
    return originFromPlane(pose,(cv::Mat_<double>(3,1)<<dx*board.squareMm/2000,dy*board.squareMm/2000,0),rig,turns,height);
}
BoardPose locateRectangle(const std::vector<cv::Point2f>& corners,const RectangleOriginConfig& rectangle,const Calibration& intrinsics,const RigCalibration& rig,size_t camera){
    rectangle.validate();rig.validate();
    if(corners.size()!=4)throw std::runtime_error("종이의 모서리 4개를 순서대로 선택하세요.");
    for(size_t i=0;i<corners.size();++i){const auto p=corners[i];
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.y<0||p.x>=intrinsics.imageSize.width||p.y>=intrinsics.imageSize.height)
            throw std::runtime_error("모서리는 영상 안에서 선택하세요.");
        for(size_t j=0;j<i;++j)if(cv::norm(p-corners[j])<2)throw std::runtime_error("서로 다른 모서리를 선택하세요.");
    }
    if(!cv::isContourConvex(corners)||std::abs(cv::contourArea(corners))<100)throw std::runtime_error("교차하지 않는 사각형 모서리를 순서대로 선택하세요.");
    if(intrinsics.K.type()!=CV_64F||intrinsics.K.size()!=cv::Size(3,3)||!cv::checkRange(intrinsics.K)||intrinsics.K.at<double>(0,0)<=0||intrinsics.K.at<double>(1,1)<=0||
       intrinsics.distortion.type()!=CV_64F||intrinsics.distortion.total()!=5||!cv::checkRange(intrinsics.distortion))throw std::runtime_error("유효한 개별 카메라 보정이 필요합니다.");
    const float width=float(rectangle.widthMm/1000),height=float(rectangle.heightMm/1000);
    std::vector<cv::Point3f> objects{{0,0,0},{width,0,0},{width,height,0},{0,height,0}};
    std::vector<cv::Mat> rotations,translations;
    cv::solvePnPGeneric(objects,corners,intrinsics.K,intrinsics.distortion,rotations,translations,false,cv::SOLVEPNP_IPPE);
    cv::Mat cameraR,cameraT;cameraTransform(rig,camera,cameraR,cameraT);BoardPose best;double lowest=std::numeric_limits<double>::infinity();
    for(size_t i=0;i<rotations.size();++i){
        cv::solvePnPRefineLM(objects,corners,intrinsics.K,intrinsics.distortion,rotations[i],translations[i]);
        cv::Mat rotation;cv::Rodrigues(rotations[i],rotation);bool front=true;
        for(const auto& p:objects){cv::Mat point=(cv::Mat_<double>(3,1)<<p.x,p.y,p.z);cv::Mat position=rotation*point+translations[i];if(position.at<double>(2)<=1e-6)front=false;}
        if(!front)continue;
        std::vector<cv::Point2f> projected;cv::projectPoints(objects,rotations[i],translations[i],intrinsics.K,intrinsics.distortion,projected);
        double error=cv::norm(projected,corners)/2;if(!std::isfinite(error)||error>=lowest)continue;
        BoardPose candidate;candidate.R=cameraR.t()*rotation;candidate.T=cameraR.t()*(translations[i]-cameraT);
        candidate.errors.assign(rig.fromReference.size()+1,0);candidate.errors[camera]=error;candidate.measuredCameras={int(camera)};
        try{makeRectangleWorldOrigin(candidate,rectangle,rig);}catch(const std::exception&){continue;}
        best=std::move(candidate);lowest=error;
    }
    if(best.R.empty()||lowest>5)throw std::runtime_error("위치를 구하지 못했습니다. 종이 크기와 모서리 순서를 확인하세요.");
    return best;
}
WorldOrigin makeRectangleWorldOrigin(const BoardPose& pose,const RectangleOriginConfig& rectangle,const RigCalibration& rig,int turns,double height){
    rectangle.validate();
    auto result=originFromPlane(pose,(cv::Mat_<double>(3,1)<<rectangle.widthMm/2000,rectangle.heightMm/2000,0),rig,turns,height);
    for(size_t i=0;i<=rig.fromReference.size();++i)if(result.cameraCenter(rig,i).at<double>(2)<=1e-5)
        throw std::runtime_error("모든 카메라가 바닥 위에 오도록 모서리와 보정값을 확인하세요.");
    return result;
}
cv::Mat drawWorldAxes(const cv::Mat& image,const Calibration& intrinsics,const RigCalibration& rig,const WorldOrigin& origin,size_t camera,double length){
    origin.validate();cv::Mat display=image.clone(),r,t,rv;cameraTransform(rig,camera,r,t);cv::Mat rotation=r*origin.R,translation=r*origin.T+t;cv::Rodrigues(rotation,rv);
    std::vector<cv::Point3d> points{{0,0,0},{length,0,0},{0,length,0},{0,0,length}};
    for(auto p:points)if(rotation.at<double>(2,0)*p.x+rotation.at<double>(2,1)*p.y+rotation.at<double>(2,2)*p.z+translation.at<double>(2)<=0)return display;
    std::vector<cv::Point2d> pixels;cv::projectPoints(points,rv,translation,intrinsics.K,intrinsics.distortion,pixels);
    for(auto p:pixels)if(!std::isfinite(p.x)||!std::isfinite(p.y)||std::abs(p.x)>1e6||std::abs(p.y)>1e6)return display;
    const cv::Scalar colors[]={{0,0,255},{0,220,0},{255,120,0}};
    for(int i=1;i<=3;++i){cv::arrowedLine(display,pixels[0],pixels[i],colors[i-1],3,cv::LINE_AA,0,.12);cv::putText(display,std::string(1,char('X'+i-1)),pixels[i],cv::FONT_HERSHEY_SIMPLEX,.8,colors[i-1],2,cv::LINE_AA);}
    cv::circle(display,pixels[0],5,{255,255,255},-1,cv::LINE_AA);cv::putText(display,"O",pixels[0]+cv::Point2d(8,-8),cv::FONT_HERSHEY_SIMPLEX,.7,{255,255,255},2,cv::LINE_AA);return display;
}
