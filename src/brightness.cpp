#include "brightness.hpp"
#include "calibration.hpp"
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

void BrightnessOptions::validate() const {
    if(blackLevel<0||blackLevel>255||whiteLevel<0||whiteLevel>255||whiteLevel-blackLevel<16||(enabled&&!fitted))
        throw std::runtime_error("Invalid fixed software brightness levels.");
}
cv::Mat adjustDisplay(const cv::Mat& image,double brightness,double contrast){
    if(!std::isfinite(brightness)||brightness<-100||brightness>100||
       !std::isfinite(contrast)||contrast<.25||contrast>3)
        throw std::runtime_error("Invalid display brightness or contrast.");
    if(image.empty()||image.depth()!=CV_8U||(image.channels()!=1&&image.channels()!=3))
        throw std::runtime_error("Unsupported display image.");
    if(brightness==0&&contrast==1)return image;
    cv::Mat output;
    image.convertTo(output,image.type(),contrast,128*(1-contrast)+brightness);
    return output;
}
cv::Mat applySoftwareBrightness(const cv::Mat& image,const BrightnessOptions& options){
    options.validate();
    if(image.empty()||image.depth()!=CV_8U||(image.channels()!=1&&image.channels()!=3))
        throw std::runtime_error("Unsupported software brightness image.");
    if(!options.enabled)return image;
    std::array<uchar,256> values;
    for(int i=0;i<256;++i)values[i]=cv::saturate_cast<uchar>(16.0+224.0*(i-options.blackLevel)/(options.whiteLevel-options.blackLevel));
    cv::Mat lookup(1,256,CV_8U,values.data()),output;
    cv::LUT(image,lookup,output);return output;
}
BrightnessOptions estimateBoardBrightness(const Observation& o){
    validateSample(o,{});
    if(o.image.depth()!=CV_8U||(o.image.channels()!=1&&o.image.channels()!=3))throw std::runtime_error("Unsupported board image.");
    for(auto p:o.imagePoints)if(p.x<0||p.y<0||p.x>=o.image.cols||p.y>=o.image.rows)throw std::runtime_error("Board corners outside image.");
    std::vector<cv::Point2f> hull;cv::convexHull(o.imagePoints,hull);
    if(std::abs(cv::contourArea(hull))<100)throw std::runtime_error("보드를 조금 더 가까이 대고 다시 맞춰주세요.");
    auto bounds=cv::boundingRect(hull)&cv::Rect(0,0,o.image.cols,o.image.rows);
    std::vector<cv::Point> polygon;for(auto p:hull)polygon.emplace_back(cvRound(p.x)-bounds.x,cvRound(p.y)-bounds.y);
    cv::Mat mask=cv::Mat::zeros(bounds.size(),CV_8U),gray;
    cv::fillConvexPoly(mask,polygon,cv::Scalar(255));
    if(o.image.channels()==3)cv::cvtColor(o.image(bounds),gray,cv::COLOR_BGR2GRAY);else gray=o.image(bounds);
    // Only the detected board interior contributes. The resulting LUT is fixed;
    // later frames and changing backgrounds never refit it.
    std::array<int,256> histogram{};int count=0;
    for(int y=0;y<gray.rows;++y)for(int x=0;x<gray.cols;++x)if(mask.at<uchar>(y,x)){++histogram[gray.at<uchar>(y,x)];++count;}
    auto percentile=[&](double q){int cumulative=0;for(int i=0;i<256;++i){cumulative+=histogram[i];if(cumulative>=count*q)return i;}return 255;};
    BrightnessOptions options{true,true,percentile(.15),percentile(.85)};
    if(options.whiteLevel-options.blackLevel<16)throw std::runtime_error("보드의 흰 칸과 검은 칸 대비가 부족합니다. 보드가 선명하게 보이도록 한 뒤 다시 맞춰주세요.");
    options.validate();return options;
}
