#include "auto_capture.hpp"
#include <opencv2/calib3d.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

void AutoCaptureConfig::validate() const {
    if(target<10||target>80||!std::isfinite(holdSeconds)||holdSeconds<0.3||holdSeconds>3||
       !std::isfinite(intervalSeconds)||intervalSeconds<0.5||intervalSeconds>10)
        throw std::runtime_error("Invalid automatic capture settings.");
}
void AutoCapture::resetHold(){anchor.reset();stableFrames=0;holdProgress=0;}
void AutoCapture::start(const AutoCaptureConfig& config){
    config.validate();options=config;enabled=true;state=CaptureStatus::Waiting;
    resetHold();lastFrame=-1;lastCapture=-1e9;
}
void AutoCapture::stop(){enabled=false;resetHold();state=CaptureStatus::Stopped;}
void AutoCapture::tick(double now){
    if(enabled&&lastFrame>=0&&now-lastFrame>1.5){resetHold();state=CaptureStatus::Stale;}
}
std::optional<AutoCapture::Pose> AutoCapture::pose(const Observation& f,const BoardConfig& b){
    if(f.image.empty()||f.imagePoints.size()<6||f.imagePoints.size()!=f.objectPoints.size())return {};
    if(b.pattern==Pattern::Chessboard&&f.imagePoints.size()!=size_t(b.columns*b.rows))return {};
    std::vector<cv::Point2f> plane;
    for(size_t i=0;i<f.objectPoints.size();++i){
        auto p=f.objectPoints[i];auto q=f.imagePoints[i];
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(q.x)||!std::isfinite(q.y))return {};
        plane.emplace_back(p.x,p.y);
    }
    auto H=cv::findHomography(plane,f.imagePoints);
    if(H.empty()||!cv::checkRange(H))return {};
    const float square=float(b.squareMm/1000);
    const float x=b.pattern==Pattern::Chessboard?0.f:square;
    const float y=x;
    const float right=(b.columns-1)*square;
    const float bottom=(b.rows-1)*square;
    std::vector<cv::Point2f> corners{{x,y},{right,y},{right,bottom},{x,bottom}},projected;
    cv::perspectiveTransform(corners,projected,H);
    const float diagonal=float(std::hypot(f.image.cols,f.image.rows));
    Pose result;
    for(size_t i=0;i<4;++i){
        if(!std::isfinite(projected[i].x)||!std::isfinite(projected[i].y))return {};
        result[i]=projected[i]/diagonal;
    }
    return result;
}
double AutoCapture::distance(const Pose& a,const Pose& b){
    // Unmarked chessboards can reverse corner numbering; compare footprints
    // under both windings so re-numbering does not create a new sample.
    double best=std::numeric_limits<double>::max();
    for(int winding:{-1,1})for(int offset=0;offset<4;++offset){
        double sum=0;
        for(int i=0;i<4;++i){auto d=a[i]-b[(offset+winding*i+8)%4];sum+=d.dot(d);}
        best=std::min(best,std::sqrt(sum/4));
    }
    return best;
}
bool AutoCapture::observe(const Observation& f,const BoardConfig& b,const std::vector<Observation>& samples,double at,double now){
    if(!enabled)return false;
    if(samples.size()>=size_t(options.target)){enabled=false;resetHold();state=CaptureStatus::Complete;return false;}
    if(now-at>1.5||at>now||at<=lastFrame){tick(now);return false;}
    const bool gap=lastFrame>=0&&at-lastFrame>1.0;
    lastFrame=at;if(gap)resetHold();
    auto current=pose(f,b);
    if(!current){resetHold();state=CaptureStatus::Waiting;return false;}
    for(const auto& sample:samples){
        if(sample.image.size()!=f.image.size()){resetHold();state=CaptureStatus::Waiting;return false;}
        auto previous=pose(sample,b);
        if(previous&&distance(*current,*previous)<0.025){resetHold();state=CaptureStatus::MoveBoard;return false;}
    }
    if(now-lastCapture<options.intervalSeconds){resetHold();state=CaptureStatus::Cooldown;return false;}
    if(!anchor||distance(*current,*anchor)>0.0015){anchor=current;holdStart=at;stableFrames=0;}
    ++stableFrames;
    holdProgress=float(std::clamp((at-holdStart)/options.holdSeconds,0.0,1.0));
    state=CaptureStatus::Hold;
    return holdProgress>=1&&stableFrames>=3;
}
void AutoCapture::captured(double now,size_t count){
    lastCapture=now;resetHold();
    if(enabled&&count>=size_t(options.target)){enabled=false;state=CaptureStatus::Complete;}
    else if(enabled)state=CaptureStatus::Cooldown;
}
