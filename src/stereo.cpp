#include "stereo.hpp"
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

bool usableStereoTiming(double a,double b,double now){
    return std::isfinite(a)&&std::isfinite(b)&&std::isfinite(now)&&a<=now&&b<=now&&
        now-a<=1.5&&now-b<=1.5&&std::abs(a-b)<=.1;
}
void StereoCalibration::validate() const{
    if(R.type()!=CV_64F||R.size()!=cv::Size(3,3)||T.type()!=CV_64F||T.size()!=cv::Size(1,3)||
       !cv::checkRange(R)||!cv::checkRange(T)||!std::isfinite(rms)||rms<0||cv::norm(T)<1e-8||
       cv::norm(R*R.t()-cv::Mat::eye(3,3,CV_64F))>1e-5||std::abs(cv::determinant(R)-1)>1e-5)
        throw std::runtime_error("Invalid stereo transform.");
    if(perPairErrors.size()!=secondRotations.size())throw std::runtime_error("Invalid stereo diagnostics.");
    for(double e:perPairErrors)if(!std::isfinite(e)||e<0)throw std::runtime_error("Invalid stereo error.");
    for(int r:secondRotations)if(r<0||r>3)throw std::runtime_error("Invalid board correspondence rotation.");
}
cv::Mat StereoCalibration::secondCenterInFirst()const{validate();return -R.t()*T;}
StereoPoints matchStereoPoints(const StereoPair& p,const BoardConfig& b,int rotation){
    b.validate();validateSample(p.first,{});validateSample(p.second,{});
    StereoPoints out;
    if(b.pattern==Pattern::Chessboard){
        if(p.first.imagePoints.size()!=size_t(b.columns*b.rows)||p.second.imagePoints.size()!=p.first.imagePoints.size()||
           rotation<0||rotation>3||(b.columns!=b.rows&&rotation%2))throw std::runtime_error("Invalid chessboard stereo pair.");
        out.object=p.first.objectPoints;out.first=p.first.imagePoints;
        for(int y=0;y<b.rows;++y)for(int x=0;x<b.columns;++x){
            int u=x,v=y;
            if(rotation==1){u=b.columns-1-y;v=x;}
            if(rotation==2){u=b.columns-1-x;v=b.rows-1-y;}
            if(rotation==3){u=y;v=b.rows-1-x;}
            out.second.push_back(p.second.imagePoints[v*b.columns+u]);
        }
    }else{
        if(rotation!=0||p.first.ids.size()!=p.first.imagePoints.size()||p.second.ids.size()!=p.second.imagePoints.size())throw std::runtime_error("Invalid ChArUco IDs.");
        std::map<int,size_t> right,left;
        for(size_t i=0;i<p.second.ids.size();++i)if(!right.emplace(p.second.ids[i],i).second)throw std::runtime_error("Duplicate ChArUco ID.");
        for(size_t i=0;i<p.first.ids.size();++i){
            int id=p.first.ids[i];if(!left.emplace(id,i).second)throw std::runtime_error("Duplicate ChArUco ID.");
            if(auto it=right.find(id);it!=right.end()){
                auto j=it->second;if(cv::norm(p.first.objectPoints[i]-p.second.objectPoints[j])>1e-6)throw std::runtime_error("Board definitions differ.");
                out.object.push_back(p.first.objectPoints[i]);out.first.push_back(p.first.imagePoints[i]);out.second.push_back(p.second.imagePoints[j]);
            }
        }
    }
    Observation common=p.first;common.objectPoints=out.object;common.imagePoints=out.first;validateSample(common,{});
    return out;
}
namespace {
struct Candidate {cv::Mat R,T;int rotation=0;};
void checkStop(std::stop_token s){if(s.stop_requested())throw std::runtime_error("Stereo calculation cancelled.");}
void validateIntrinsics(const Calibration& c){
    if(c.imageSize.width<=0||c.imageSize.height<=0||c.K.type()!=CV_64F||c.K.size()!=cv::Size(3,3)||
       c.distortion.type()!=CV_64F||c.distortion.total()!=5||!cv::checkRange(c.K)||!cv::checkRange(c.distortion)||
       c.K.at<double>(0,0)<=0||c.K.at<double>(1,1)<=0)throw std::runtime_error("Valid individual calibrations are required.");
}
Candidate candidate(const StereoPoints& p,const Calibration& a,const Calibration& b,int rotation){
    cv::Mat ra,ta,rb,tb,A,B;
    if(!cv::solvePnP(p.object,p.first,a.K,a.distortion,ra,ta)||!cv::solvePnP(p.object,p.second,b.K,b.distortion,rb,tb))throw std::runtime_error("Board pose estimation failed.");
    cv::Rodrigues(ra,A);cv::Rodrigues(rb,B);
    Candidate c;c.R=B*A.t();c.T=tb-c.R*ta;c.rotation=rotation;return c;
}
double distance(const Candidate& a,const Candidate& b){
    double cosine=std::clamp((cv::trace(a.R*b.R.t())[0]-1)*.5,-1.0,1.0);
    double angle=std::acos(cosine),shift=cv::norm(a.T-b.T)/.25;
    return angle*angle+shift*shift;
}
}
StereoCalibration calibrateStereo(const std::vector<StereoPair>& pairs,const BoardConfig& board,const Calibration& a,const Calibration& b,std::stop_token stop){
    board.validate();validateIntrinsics(a);validateIntrinsics(b);
    // With CALIB_FIX_INTRINSIC, imageSize is not used to initialize K. Keep
    // each camera's native pixels and saved K/D, including mixed resolutions.
    if(pairs.size()<10)throw std::runtime_error("상대 위치 계산은 최소 10쌍, 권장 20~30쌍이 필요합니다.");
    std::vector<std::vector<Candidate>> candidates;
    for(const auto& pair:pairs){
        checkStop(stop);
        if(pair.first.image.size()!=a.imageSize||pair.second.image.size()!=b.imageSize||!std::isfinite(pair.receiptSkewMs)||pair.receiptSkewMs<0||pair.receiptSkewMs>100)
            throw std::runtime_error("Stereo frame size or receipt timing differs from capture requirements.");
        std::vector<Candidate> choices;
        for(int r=0;r<4;++r){
            if(board.pattern==Pattern::Charuco&&r!=0)continue;
            if(board.pattern==Pattern::Chessboard&&board.columns!=board.rows&&r%2)continue;
            choices.push_back(candidate(matchStereoPoints(pair,board,r),a,b,r));
        }
        candidates.push_back(std::move(choices));
    }
    // An unmarked chessboard may be numbered in opposite directions in the two
    // views. Select the board rotations that share one rigid camera transform
    // across diverse poses, instead of assuming matching detector indices.
    Candidate reference;double best=std::numeric_limits<double>::infinity();
    for(const auto& choices:candidates)for(const auto& seed:choices){
        checkStop(stop);double score=0;
        for(const auto& view:candidates){double d=std::numeric_limits<double>::infinity();for(const auto& c:view)d=std::min(d,distance(seed,c));score+=std::min(d,4.0);}
        if(score<best){best=score;reference=seed;}
    }
    std::vector<std::vector<cv::Point3f>> objects;
    std::vector<std::vector<cv::Point2f>> first,second;
    StereoCalibration result;
    for(size_t i=0;i<pairs.size();++i){
        auto choice=std::min_element(candidates[i].begin(),candidates[i].end(),[&](const auto& x,const auto& y){return distance(reference,x)<distance(reference,y);});
        auto p=matchStereoPoints(pairs[i],board,choice->rotation);
        objects.push_back(std::move(p.object));first.push_back(std::move(p.first));second.push_back(std::move(p.second));result.secondRotations.push_back(choice->rotation);
    }
    checkStop(stop);cv::Mat ka=a.K.clone(),da=a.distortion.clone(),kb=b.K.clone(),db=b.distortion.clone(),E,F,errors;
    result.rms=cv::stereoCalibrate(objects,first,second,ka,da,kb,db,a.imageSize,result.R,result.T,E,F,errors,
        cv::CALIB_FIX_INTRINSIC,{cv::TermCriteria::COUNT|cv::TermCriteria::EPS,100,1e-9});
    checkStop(stop);
    for(int i=0;i<errors.rows;++i)result.perPairErrors.push_back(std::sqrt((std::pow(errors.at<double>(i,0),2)+std::pow(errors.at<double>(i,1),2))*.5));
    result.validate();return result;
}
StereoRectification makeStereoRectification(const Calibration& a,const Calibration& b,const StereoCalibration& relation){
    validateIntrinsics(a);validateIntrinsics(b);relation.validate();
    StereoRectification out;out.inputSize={a.imageSize,b.imageSize};
    out.outputSize={std::max(a.imageSize.width,b.imageSize.width),std::max(a.imageSize.height,b.imageSize.height)};
    // stereoRectify assumes one image size. Represent each native image in a
    // shared virtual canvas using uniform scaling and centred padding, then
    // build the sampling maps from the original, unchanged camera intrinsics.
    auto virtualK=[&](const Calibration& c){
        const double scale=std::min(double(out.outputSize.width)/c.imageSize.width,double(out.outputSize.height)/c.imageSize.height);
        cv::Mat pixelTransform=(cv::Mat_<double>(3,3)<<scale,0,(out.outputSize.width-scale*c.imageSize.width)*.5,
            0,scale,(out.outputSize.height-scale*c.imageSize.height)*.5,0,0,1);
        return cv::Mat(pixelTransform*c.K);
    };
    cv::Mat q;
    // Preserve the field of view: tight alpha=0 cropping can remove the useful
    // overlap for strongly diagonal camera baselines. Missing pixels stay black.
    cv::stereoRectify(virtualK(a),a.distortion,virtualK(b),b.distortion,out.outputSize,relation.R,relation.T,
        out.R[0],out.R[1],out.P[0],out.P[1],q,cv::CALIB_ZERO_DISPARITY,1,out.outputSize);
    for(int i=0;i<2;++i){
        const auto& c=i?b:a;
        cv::initUndistortRectifyMap(c.K,c.distortion,out.R[i],out.P[i],out.outputSize,CV_32FC1,out.mapX[i],out.mapY[i]);
        if(!cv::checkRange(out.mapX[i])||!cv::checkRange(out.mapY[i]))throw std::runtime_error("Rectification maps are not finite.");
    }
    out.rotated=std::abs(out.P[1].at<double>(1,3))>std::abs(out.P[1].at<double>(0,3));
    if(out.rotated){
        const cv::Mat rotatePixels=(cv::Mat_<double>(3,3)<<0,-1,out.outputSize.height-1,1,0,0,0,0,1);
        for(int i=0;i<2;++i){
            cv::rotate(out.mapX[i],out.mapX[i],cv::ROTATE_90_CLOCKWISE);
            cv::rotate(out.mapY[i],out.mapY[i],cv::ROTATE_90_CLOCKWISE);
            out.P[i]=rotatePixels*out.P[i];
        }
        out.outputSize={out.outputSize.height,out.outputSize.width};
    }
    return out;
}
cv::Mat rectifiedFrame(const cv::Mat& input,const StereoRectification& maps,int cameraIndex){
    if(cameraIndex<0||cameraIndex>1||input.empty())throw std::runtime_error("Invalid rectification frame.");
    if(input.size()!=maps.inputSize[cameraIndex])throw std::runtime_error("영상 크기가 개별 보정과 다릅니다.");
    if(maps.mapX[cameraIndex].type()!=CV_32FC1||maps.mapY[cameraIndex].type()!=CV_32FC1||
       maps.mapX[cameraIndex].size()!=maps.outputSize||maps.mapY[cameraIndex].size()!=maps.outputSize)
        throw std::runtime_error("Invalid rectification maps.");
    cv::Mat output;cv::remap(input,output,maps.mapX[cameraIndex],maps.mapY[cameraIndex],cv::INTER_LINEAR,cv::BORDER_CONSTANT);
    return output;
}
namespace {
// Clip an infinite epipolar line to the displayed image bounds.
bool clipGuide(cv::Point2d origin,cv::Point2d direction,const cv::Rect2d& bounds,
               double& lo,double& hi){
    const double positions[]{origin.x,origin.y},steps[]{direction.x,direction.y};
    const double minima[]{bounds.x,bounds.y},maxima[]{bounds.x+bounds.width,bounds.y+bounds.height};
    for(int axis=0;axis<2;++axis){
        if(std::abs(steps[axis])<1e-12){if(positions[axis]<minima[axis]||positions[axis]>maxima[axis])return false;}
        else {double a=(minima[axis]-positions[axis])/steps[axis],b=(maxima[axis]-positions[axis])/steps[axis];
            if(a>b)std::swap(a,b);lo=std::max(lo,a);hi=std::min(hi,b);if(lo>hi)return false;}
    }
    return std::isfinite(lo)&&std::isfinite(hi);
}

}
StereoEpipolarGuides makeEpipolarGuides(const Calibration& a,const Calibration& b,
                                       const StereoRectification& maps,int mode,int count){
    validateIntrinsics(a);validateIntrinsics(b);
    if(mode<1||mode>2||count<1||count>12||maps.outputSize.width<2||maps.outputSize.height<2)
        throw std::runtime_error("Invalid epipolar guide options.");
    StereoEpipolarGuides result;
    for(int camera=0;camera<2;++camera){
        const auto& c=camera?b:a;result[camera].resize(count);
        if(maps.inputSize[camera]!=c.imageSize||maps.R[camera].type()!=CV_64F||maps.R[camera].size()!=cv::Size(3,3)||
           maps.P[camera].type()!=CV_64F||maps.P[camera].size()!=cv::Size(4,3)||!cv::checkRange(maps.R[camera])||!cv::checkRange(maps.P[camera]))
            throw std::runtime_error("Invalid epipolar projection.");
        cv::Mat inverseK=c.K.inv(),homography=maps.P[camera].colRange(0,3)*maps.R[camera]*inverseK;
        cv::Rect2d imageBounds(0,0,c.imageSize.width-1,c.imageSize.height-1);
        const auto bounds=imageBounds;
        for(int n=0;n<count;++n){
            const double y=double(maps.outputSize.height-1)*(n+1)/(count+1);
            auto& guide=result[camera][n];
            if(mode==2){guide.push_back({{0,float(y)},{float(maps.outputSize.width-1),float(y)}});continue;}
            // H maps ideal native pixels to rectified pixels. H^T maps the
            // common horizontal epiline back into each ideal native image.
            cv::Mat line=homography.t()*(cv::Mat_<double>(3,1)<<0,1,-y);
            const double la=line.at<double>(0),lb=line.at<double>(1),lc=line.at<double>(2),norm=std::hypot(la,lb);
            if(!std::isfinite(norm)||norm<1e-12)continue;
            cv::Point2d origin(-la*lc/(norm*norm),-lb*lc/(norm*norm)),direction(-lb/norm,la/norm);
            double lo=-std::numeric_limits<double>::infinity(),hi=std::numeric_limits<double>::infinity();
            if(!clipGuide(origin,direction,bounds,lo,hi))continue;
            auto start=origin+direction*lo,end=origin+direction*hi;
            guide.push_back({cv::Point2f(start),cv::Point2f(end)});
        }
    }
    return result;
}
void saveStereoArchive(const std::string& directory,const std::string& firstId,const std::string& secondId,const BoardConfig& board,
                       const std::vector<StereoPair>& pairs,const Calibration& a,const Calibration& b,const StereoCalibration& result){
    result.validate();if(pairs.size()!=result.perPairErrors.size())throw std::runtime_error("Stereo diagnostics do not match samples.");
    namespace fs=std::filesystem;fs::path dir(std::u8string(directory.begin(),directory.end()));
    if(fs::exists(dir))throw std::runtime_error("Stereo archive already exists.");fs::create_directories(dir);
    std::vector<Observation> first,second;for(const auto& p:pairs){first.push_back(p.first);second.push_back(p.second);}
    auto ca=a,cb=b;ca.perViewErrors=result.perPairErrors;cb.perViewErrors=result.perPairErrors;
    saveCalibration(directory+"/A",firstId,board,first,ca);saveCalibration(directory+"/B",secondId,board,second,cb);
    cv::FileStorage f("stereo.yml",cv::FileStorage::WRITE|cv::FileStorage::MEMORY);
    f<<"schema_version"<<1<<"camera_A"<<firstId<<"camera_B"<<secondId<<"convention"<<"X_B = R * X_A + T; metres; x right, y down, z forward"
     <<"R_A_to_B"<<result.R<<"T_A_to_B_m"<<result.T<<"camera_B_center_in_A_m"<<result.secondCenterInFirst()<<"rms_px"<<result.rms
     <<"pair_errors_px"<<result.perPairErrors<<"B_corner_quarter_turns"<<result.secondRotations<<"receipt_skew_ms"<<"[";
    for(const auto& p:pairs)f<<p.receiptSkewMs;f<<"]";
    std::ofstream out(dir/"stereo.yml",std::ios::binary);out<<f.releaseAndGetString();if(!out)throw std::runtime_error("Stereo archive write failed.");
}
