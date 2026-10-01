#include "ball_detection.hpp"
#include "brightness.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <optional>
#include <array>
#include <stdexcept>

namespace {
void checkStop(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Ball detection cancelled.");}
bool inRange(double v,double lo,double hi){return std::isfinite(v)&&v>=lo&&v<=hi;}
bool inside(const std::vector<cv::Point2f>& polygon,cv::Point2f point){return polygon.empty()||cv::pointPolygonTest(polygon,point,false)>=0;}
std::optional<BallCircle> refit(const BallCircle& seed,const cv::Mat& edges,const BallOptions& o,double scale,std::stop_token stop){
    std::vector<cv::Point2d> pixels;const double reach=seed.radius*o.reach,tol=std::max(.4,o.tolerance*scale);
    cv::Rect box(int(std::floor(seed.center.x-reach)),int(std::floor(seed.center.y-reach)),int(std::ceil(2*reach+2)),int(std::ceil(2*reach+2)));
    box&=cv::Rect(0,0,edges.cols,edges.rows);
    for(int y=box.y;y<box.y+box.height;++y)for(int x=box.x;x<box.x+box.width;++x)
        if(edges.at<uchar>(y,x)&&cv::norm(cv::Point2f(float(x),float(y))-seed.center)<=reach)pixels.emplace_back(x,y);
    if(pixels.size()<12)return {};
    std::mt19937 random(0);std::uniform_int_distribution<size_t> pick(0,pixels.size()-1);BallCircle best;size_t bestCount=0;
    for(int it=0;it<o.iterations;++it){checkStop(stop);auto a=pixels[pick(random)],b=pixels[pick(random)],c=pixels[pick(random)];
        double d=2*(a.x*(b.y-c.y)+b.x*(c.y-a.y)+c.x*(a.y-b.y));if(std::abs(d)<1e-8)continue;
        double a2=a.dot(a),b2=b.dot(b),c2=c.dot(c);
        cv::Point2d center((a2*(b.y-c.y)+b2*(c.y-a.y)+c2*(a.y-b.y))/d,(a2*(c.x-b.x)+b2*(a.x-c.x)+c2*(b.x-a.x))/d);
        double radius=cv::norm(a-center);if(radius<seed.radius*.5||radius>seed.radius*1.8||cv::norm(center-cv::Point2d(seed.center))>reach)continue;
        size_t count=0;for(auto p:pixels)if(std::abs(cv::norm(p-center)-radius)<tol)++count;
        if(count>bestCount){bestCount=count;best.center=cv::Point2f(center);best.radius=float(radius);}
    }
    if(bestCount<10)return {};
    std::vector<cv::Point2d> inliers;for(auto p:pixels)if(std::abs(cv::norm(p-cv::Point2d(best.center))-best.radius)<tol)inliers.push_back(p);
    cv::Mat A(int(inliers.size()),3,CV_64F),B(int(inliers.size()),1,CV_64F),solution;
    for(int i=0;i<A.rows;++i){auto p=inliers[i]-cv::Point2d(best.center);A.at<double>(i,0)=p.x;A.at<double>(i,1)=p.y;A.at<double>(i,2)=1;B.at<double>(i)=p.dot(p);}
    if(!cv::solve(A,B,solution,cv::DECOMP_SVD))return {};
    cv::Point2d shift(solution.at<double>(0)/2,solution.at<double>(1)/2);double r2=solution.at<double>(2)+shift.dot(shift);if(r2<=0)return {};
    best.center+=cv::Point2f(shift);best.radius=float(std::sqrt(r2));
    if(best.radius<o.minRadius*scale||best.radius>o.maxRadius*scale||cv::norm(best.center-seed.center)>reach)return {};
    size_t count=0;for(auto p:pixels)if(std::abs(cv::norm(p-cv::Point2d(best.center))-best.radius)<tol)++count;
    best.coverage=std::min(1.,count/std::max(1.,2*CV_PI*best.radius));if(best.coverage<o.minCoverage)return {};return best;
}
}
void BallOptions::validate()const{
    if(stage<0||stage>4||blur<0||blur>4||kernel<1||kernel>31||kernel%2==0||minRadius<1||maxRadius<minRadius||maxRadius>2000||minDistance<1||minDistance>4000||
       cannyLow<0||cannyHigh<=cannyLow||cannyHigh>500||houghEdge<1||houghEdge>500||houghVotes<1||houghVotes>300||iterations<8||iterations>200||
       !inRange(brightness,-100,100)||!inRange(contrast,.25,3)||!inRange(scale,.25,1)||!inRange(dp,1,4)||!inRange(reach,1,4)||!inRange(tolerance,.5,10)||!inRange(minCoverage,0,1)||
       redWidth<1||redWidth>40||blueLow<0||blueHigh<=blueLow||blueHigh>179||minSaturation<0||minSaturation>255||minValue<0||minValue>255||whiteSaturation<0||whiteSaturation>255||whiteValue<0||whiteValue>255||!inRange(colorFraction,.1,1))
        throw std::runtime_error("Invalid ball detection settings.");
    if(!roi.empty()&&roi.size()!=4)throw std::runtime_error("ROI requires four corners.");
    for(auto p:roi)if(!inRange(p.x,0,1)||!inRange(p.y,0,1))throw std::runtime_error("ROI point is outside the image.");
    if(!roi.empty()&&(!cv::isContourConvex(roi)||std::abs(cv::contourArea(roi))<1e-5))throw std::runtime_error("ROI corners must form a non-crossing quadrilateral.");
}
void BallSettings::validate()const{if(cameras.size()>64)throw std::runtime_error("Too many ball detection profiles.");for(const auto& [id,o]:cameras){if(id.empty()||id.size()>256)throw std::runtime_error("Invalid ball camera identity.");o.validate();}}
void to_json(nlohmann::json& j,const BallOptions& o){
    j={{"color",o.color},{"detect",o.detect},{"overlay",o.overlay},{"roi_enabled",o.roiEnabled},{"ransac",o.ransac},{"show_candidates",o.showCandidates},{"show_center",o.showCenter},{"show_roi",o.showRoi},
        {"stage",o.stage},{"blur",o.blur},{"kernel",o.kernel},{"min_radius",o.minRadius},{"max_radius",o.maxRadius},{"min_distance",o.minDistance},{"canny_low",o.cannyLow},{"canny_high",o.cannyHigh},
        {"hough_edge",o.houghEdge},{"hough_votes",o.houghVotes},{"iterations",o.iterations},{"brightness",o.brightness},{"contrast",o.contrast},{"scale",o.scale},{"dp",o.dp},{"reach",o.reach},{"tolerance",o.tolerance},{"min_coverage",o.minCoverage},
        {"red_width",o.redWidth},{"blue_low",o.blueLow},{"blue_high",o.blueHigh},{"min_saturation",o.minSaturation},{"min_value",o.minValue},{"white_saturation",o.whiteSaturation},{"white_value",o.whiteValue},{"color_fraction",o.colorFraction},{"roi",nlohmann::json::array()}};
    for(auto p:o.roi)j["roi"].push_back({p.x,p.y});
}
void from_json(const nlohmann::json& j,BallOptions& o){
    o=BallOptions{};
#define READ(member,key) o.member=j.value(key,o.member)
    READ(color,"color");READ(detect,"detect");READ(overlay,"overlay");READ(roiEnabled,"roi_enabled");READ(ransac,"ransac");READ(showCandidates,"show_candidates");READ(showCenter,"show_center");READ(showRoi,"show_roi");
    READ(stage,"stage");READ(blur,"blur");READ(kernel,"kernel");READ(minRadius,"min_radius");READ(maxRadius,"max_radius");READ(minDistance,"min_distance");READ(cannyLow,"canny_low");READ(cannyHigh,"canny_high");READ(houghEdge,"hough_edge");READ(houghVotes,"hough_votes");READ(iterations,"iterations");
    READ(brightness,"brightness");READ(contrast,"contrast");READ(scale,"scale");READ(dp,"dp");READ(reach,"reach");READ(tolerance,"tolerance");READ(minCoverage,"min_coverage");
    READ(redWidth,"red_width");READ(blueLow,"blue_low");READ(blueHigh,"blue_high");READ(minSaturation,"min_saturation");READ(minValue,"min_value");READ(whiteSaturation,"white_saturation");READ(whiteValue,"white_value");READ(colorFraction,"color_fraction");
#undef READ
    if(j.contains("roi"))for(const auto& p:j.at("roi")){if(p.size()!=2)throw std::runtime_error("Invalid ROI coordinate.");o.roi.emplace_back(p[0].get<float>(),p[1].get<float>());}o.validate();
}
void to_json(nlohmann::json& j,const BallSettings& o){o.validate();j={{"run",o.run},{"overlay",o.overlay},{"cameras",o.cameras}};}
void from_json(const nlohmann::json& j,BallSettings& o){o.run=j.value("run",false);o.overlay=j.value("overlay",true);o.cameras=j.value("cameras",std::map<std::string,BallOptions>{});o.validate();}
std::string classifyBallColor(const cv::Mat& bgr,const BallCircle& circle,const BallOptions& o){
    cv::Rect box(int(circle.center.x-circle.radius),int(circle.center.y-circle.radius),int(2*circle.radius+2),int(2*circle.radius+2));box&=cv::Rect(0,0,bgr.cols,bgr.rows);if(box.empty())return "Unknown";
    cv::Mat hsv;cv::cvtColor(bgr(box),hsv,cv::COLOR_BGR2HSV);int counts[3]{},total=0,step=std::max(1,int(circle.radius/20));
    for(int y=0;y<hsv.rows;y+=step)for(int x=0;x<hsv.cols;x+=step){if(cv::norm(cv::Point2f(float(x+box.x),float(y+box.y))-circle.center)>circle.radius*.65)continue;
        auto p=hsv.at<cv::Vec3b>(y,x);++total;
        if(p[1]<=o.whiteSaturation&&p[2]>=o.whiteValue)++counts[2];
        else if(p[1]>=o.minSaturation&&p[2]>=o.minValue){if(p[0]<=o.redWidth||p[0]>=180-o.redWidth)++counts[0];if(p[0]>=o.blueLow&&p[0]<=o.blueHigh)++counts[1];}}
    int best=int(std::max_element(counts,counts+3)-counts);return total>0&&counts[best]>=total*o.colorFraction?std::array<const char*,3>{"Red","Blue","White"}[best]:"Unknown";
}
BallDetection detectBalls(const cv::Mat& bgr,const BallOptions& o,std::stop_token stop){
    o.validate();checkStop(stop);if(bgr.empty()||bgr.type()!=CV_8UC3)throw std::runtime_error("Ball detection requires an 8-bit BGR source.");
    auto start=std::chrono::steady_clock::now();BallDetection result;cv::Mat processed=adjustDisplay(bgr,o.brightness,o.contrast),gray,blurred,edges;
    cv::cvtColor(processed,result.gray,cv::COLOR_BGR2GRAY);cv::resize(result.gray,gray,{},o.scale,o.scale,cv::INTER_AREA);
    switch(o.blur){case 0:blurred=gray;break;case 1:cv::medianBlur(gray,blurred,o.kernel);break;case 2:cv::GaussianBlur(gray,blurred,{o.kernel,o.kernel},0);break;case 3:cv::bilateralFilter(gray,blurred,o.kernel,75,75);break;default:cv::blur(gray,blurred,{o.kernel,o.kernel});}
    cv::Canny(blurred,edges,o.cannyLow,o.cannyHigh);checkStop(stop);std::vector<cv::Vec3f> circles;
    cv::HoughCircles(blurred,circles,cv::HOUGH_GRADIENT,o.dp,std::max(1.,o.minDistance*o.scale),o.houghEdge,o.houghVotes,std::max(1,int(std::round(o.minRadius*o.scale))),std::max(2,int(std::round(o.maxRadius*o.scale))));
    const double sx=double(gray.cols)/bgr.cols,sy=double(gray.rows)/bgr.rows;
    auto nativeCircle=[&](BallCircle circle){circle.center={float((circle.center.x+.5)/sx-.5),float((circle.center.y+.5)/sy-.5)};circle.radius/=float(std::sqrt(sx*sy));return circle;};
    std::vector<cv::Point2f> polygon;if(o.roiEnabled)for(auto p:o.roi)polygon.emplace_back(float(p.x*gray.cols+.5*sx-.5),float(p.y*gray.rows+.5*sy-.5));
    for(auto c:circles){checkStop(stop);BallCircle seed{{c[0],c[1]},c[2]};if(!inside(polygon,seed.center))continue;
        if(result.candidates.size()>=64){result.candidateLimit=true;break;}
        result.candidates.push_back(nativeCircle(seed));
        std::optional<BallCircle> fitted=o.ransac?refit(seed,edges,o,o.scale,stop):std::optional<BallCircle>(seed);if(!fitted||!inside(polygon,fitted->center))continue;
        *fitted=nativeCircle(*fitted);if(o.color)fitted->color=classifyBallColor(bgr,*fitted,o);result.balls.push_back(*fitted);
    }
    cv::resize(blurred,result.blurred,bgr.size(),0,0,cv::INTER_LINEAR);cv::resize(edges,result.edges,bgr.size(),0,0,cv::INTER_NEAREST);
    result.milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return result;
}
