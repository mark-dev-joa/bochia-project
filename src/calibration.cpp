#include "calibration.hpp"
#include "brightness.hpp"
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <stdexcept>
#include <set>

void BoardConfig::validate() const {
    if(columns < 3 || rows < 3 || columns > 40 || rows > 40 || !std::isfinite(squareMm) || squareMm <= 0)
        throw std::runtime_error("Invalid board dimensions / square length.");
    if(pattern == Pattern::Charuco && (!std::isfinite(markerMm) || markerMm <= 0 || markerMm >= squareMm || columns*rows/2 > 50))
        throw std::runtime_error("Charuco: 0 < marker < square; DICT_4X4_50 supports at most 50 markers.");
}
cv::aruco::CharucoBoard BoardConfig::charucoBoard() const {
    validate();
    cv::aruco::CharucoBoard b({columns,rows}, float(squareMm/1000), float(markerMm/1000),
                             cv::aruco::getPredefinedDictionary(cv::aruco::DICT_4X4_50));
    b.setLegacyPattern(legacy);
    return b;
}
Observation detectBoard(const cv::Mat& image, const BoardConfig& config, const BrightnessOptions& brightness) {
    config.validate();
    if(image.empty()) throw std::runtime_error("Empty image.");
    Observation o; o.image = image;o.softwareBrightness=brightness; // immutable original, never the processed preview
    cv::Mat detectionImage=applySoftwareBrightness(image,brightness);
    cv::Mat gray;
    if(detectionImage.channels() == 3) cv::cvtColor(detectionImage,gray,cv::COLOR_BGR2GRAY); else gray=detectionImage;
    if(config.pattern == Pattern::Chessboard) {
        bool found = cv::findChessboardCorners(gray,{config.columns,config.rows},o.imagePoints,
            cv::CALIB_CB_ADAPTIVE_THRESH | cv::CALIB_CB_NORMALIZE_IMAGE | cv::CALIB_CB_FAST_CHECK);
        if(!found) { o.imagePoints.clear(); return o; }
        cv::cornerSubPix(gray,o.imagePoints,{5,5},{-1,-1},{cv::TermCriteria::EPS|cv::TermCriteria::COUNT,40,0.001});
        for(int y=0;y<config.rows;++y) for(int x=0;x<config.columns;++x)
            o.objectPoints.emplace_back(float(x*config.squareMm/1000),float(y*config.squareMm/1000),0.f);
    } else {
        auto b=config.charucoBoard();
        cv::aruco::CharucoDetector detector(b);
        detector.detectBoard(gray,o.imagePoints,o.ids);
        if(o.ids.size()<6 || b.checkCharucoCornersCollinear(o.ids)) {
            o.imagePoints.clear(); o.ids.clear(); return o;
        }
        const auto& points=b.getChessboardCorners();
        for(int id:o.ids) o.objectPoints.push_back(points.at(id));
    }
    return o;
}
void validateSample(const Observation& s,const std::vector<Observation>& existing) {
    if(s.image.empty() || s.imagePoints.size()<6 || s.imagePoints.size()!=s.objectPoints.size())
        throw std::runtime_error("Board not detected: at least 6 non-collinear corners required.");
    if(!existing.empty() && existing.front().image.size()!=s.image.size())
        throw std::runtime_error("Image resolution differs. Start a separate calibration session.");
    std::vector<cv::Point2f> plane;
    for(size_t i=0;i<s.objectPoints.size();++i) {
        auto p=s.objectPoints[i]; auto q=s.imagePoints[i];
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(q.x)||!std::isfinite(q.y))
            throw std::runtime_error("Non-finite calibration point.");
        plane.emplace_back(p.x,p.y);
    }
    std::vector<cv::Point2f> hull;
    cv::convexHull(plane,hull);
    if(std::abs(cv::contourArea(hull))<1e-12) throw std::runtime_error("Collinear board points.");
}
Calibration calibrate(const std::vector<Observation>& samples) {
    if(samples.size()<10) throw std::runtime_error("Collect at least 10 diverse views (20-30 recommended).");
    std::vector<std::vector<cv::Point2f>> imagePoints;
    std::vector<std::vector<cv::Point3f>> objectPoints;
    for(const auto& s:samples) {validateSample(s,samples);imagePoints.push_back(s.imagePoints);objectPoints.push_back(s.objectPoints);}
    Calibration c;c.imageSize=samples.front().image.size();
    std::vector<cv::Mat> rotations,translations;
    c.rms=cv::calibrateCamera(objectPoints,imagePoints,c.imageSize,c.K,c.distortion,rotations,translations,0,
        {cv::TermCriteria::COUNT|cv::TermCriteria::EPS,100,1e-9});
    if(!cv::checkRange(c.K)||!cv::checkRange(c.distortion)||!std::isfinite(c.rms)||c.K.at<double>(0,0)<=0||c.K.at<double>(1,1)<=0)
        throw std::runtime_error("Invalid calibration. Collect more varied board poses.");
    for(size_t i=0;i<samples.size();++i) {
        std::vector<cv::Point2f> projected;
        cv::projectPoints(objectPoints[i],rotations[i],translations[i],c.K,c.distortion,projected);
        c.perViewErrors.push_back(cv::norm(imagePoints[i],projected,cv::NORM_L2)/std::sqrt(double(projected.size())));
    }
    return c;
}
cv::Mat undistortFrame(const cv::Mat& image,const Calibration& calibration){
    if(image.empty()||image.size()!=calibration.imageSize)throw std::runtime_error("Calibration image size does not match.");
    // cv::Mat assignment shares storage. undistort explicitly forbids in-place
    // output, so always give it a fresh destination rather than an input alias.
    cv::Mat corrected;
    cv::undistort(image,corrected,calibration.K,calibration.distortion);
    return corrected;
}
void saveCalibration(const std::string& directory,const std::string& identity,const BoardConfig& b,
                     const std::vector<Observation>& samples,const Calibration& c) {
    namespace fs=std::filesystem;
    if(c.K.empty() || samples.size()!=c.perViewErrors.size()) throw std::runtime_error("Run calibration before saving.");
    fs::path dir(std::u8string(directory.begin(),directory.end()));
    // Each export uses a new session folder, so raw images and results cannot overwrite another session.
    if(fs::exists(dir)) throw std::runtime_error("Export folder already exists. Choose a new session folder.");
    fs::create_directories(dir);
    for(size_t i=0;i<samples.size();++i) {
        std::vector<uchar> png;
        if(!cv::imencode(".png",samples[i].image,png)) throw std::runtime_error("PNG encoding failed.");
        std::ofstream out(dir/("view_"+std::to_string(i)+".png"),std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()),std::streamsize(png.size()));
        if(!out) throw std::runtime_error("Could not save image.");
    }
    cv::FileStorage f("calibration.yml",cv::FileStorage::WRITE|cv::FileStorage::MEMORY);
    f<<"schema_version"<<1<<"camera_identity"<<identity<<"opencv_version"<<CV_VERSION;
    f<<"pattern"<<(b.pattern==Pattern::Chessboard?"chessboard":"charuco")
     <<"columns"<<b.columns<<"rows"<<b.rows<<"square_length_m"<<b.squareMm/1000
     <<"marker_length_m"<<b.markerMm/1000<<"dictionary"<<"DICT_4X4_50"<<"legacy_pattern"<<int(b.legacy);
    f<<"image_width"<<c.imageSize.width<<"image_height"<<c.imageSize.height
     <<"camera_matrix"<<c.K<<"distortion_coefficients"<<c.distortion<<"rms_px"<<c.rms;
    f<<"views"<<"[";
    for(size_t i=0;i<samples.size();++i) {
        auto& s=samples[i];
        f<<"{"<<"image"<<("view_"+std::to_string(i)+".png")<<"source"<<s.source
         <<"frame_id"<<std::to_string(s.frameId)<<"camera_timestamp_raw"<<std::to_string(s.timestamp)
         <<"software_brightness_enabled"<<s.softwareBrightness.enabled<<"software_black_level"<<s.softwareBrightness.blackLevel<<"software_white_level"<<s.softwareBrightness.whiteLevel
         <<"rms_px"<<c.perViewErrors[i]<<"image_points"<<s.imagePoints<<"object_points_m"<<s.objectPoints<<"ids"<<s.ids<<"}";
    }
    f<<"]";
    std::ofstream out(dir/"calibration.yml",std::ios::binary);
    out<<f.releaseAndGetString();
    if(!out) throw std::runtime_error("Could not save calibration YAML.");
}
