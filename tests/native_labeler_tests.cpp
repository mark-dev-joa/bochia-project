#include "native_labeler_model.hpp"
#include "native_labeler_client.hpp"
#include "native_labeler_view.hpp"
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace boccia::labeler;
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
bool about(double a, double b, double tolerance = .01) { return std::abs(a - b) <= tolerance; }
void viewTests() {
    auto equal = tileSpans(-1, 1252);
    check(about(equal[0].width, 410.667) && about(equal[1].x, 420.667) && about(equal[2].x + equal[2].width, 1252), "equal tiles wrong");
    auto first = tileSpans(0, 1252), middle = tileSpans(1, 1252), last = tileSpans(2, 1252);
    check(about(first[0].width, 864) && about(first[1].x, 874) && about(first[1].width, 184) && about(first[2].x, 1068), "focused first tile wrong");
    check(about(middle[0].width, 184) && about(middle[1].x, 194) && about(middle[1].width, 864) && about(middle[2].x, 1068), "focus must keep camera order");
    check(about(last[2].x, 388) && about(last[2].width, 864), "focused last tile wrong");
    auto narrow = tileSpans(0, 700);
    check(about(narrow[1].width, 170) && about(narrow[0].width, 340), "narrow window must shrink side tiles");
    auto fit = fitView(1440, 1080, 864, 858);
    check(about(fit.scale, .6) && about(fit.x, 0) && about(fit.y, 105), "fit view wrong");
    cv::Rect2d ball(1174, 296, 38, 34);
    auto zoom = ballView(ball, 1440, 1080, 864, 858);
    check(about(zoom.scale, 858 * .34 / 38) && about(1193 * zoom.scale + zoom.x, 432, .05) && about(313 * zoom.scale + zoom.y, 429, .05), "selected ball must be centered at a third of the height");
    check(about(ballView({100, 100, 900, 900}, 1440, 1080, 864, 858).scale, .6), "large ball must not zoom below fit");
    check(about(ballView({100, 100, 4, 4}, 1440, 1080, 864, 858).scale, 14), "tiny ball must cap at 14x");
    auto crop = cropRegion({816, 465, 28, 28}, 1440, 1080);
    check(about(crop.width, 58.8) && about(crop.x, 800.6) && about(crop.y, 449.6), "crop is not centered");
    auto corner = cropRegion({2, 3, 20, 20}, 1440, 1080);
    check(about(corner.x, 0) && about(corner.y, 0) && about(corner.width, 42), "crop must shift inside the image");
}
int main(int argc,char** argv){try{
    viewTests();
    cv::Mat mask(9,13,CV_8UC1,cv::Scalar(0));mask(cv::Rect(4,2,5,5)).setTo(1);mask.at<uchar>(4,6)=0;
    auto rle=encodeMask(mask);auto decoded=decodeMask(rle,13,9);check(cv::countNonZero(mask!=decoded)==0,"RLE hole/column order lost");
    Json box={{"bbox",{4,2,5,5}},{"segmentation",rle},{"occluded",false}};
    for(int edge=0;edge<4;++edge)check(nudgeEdge(box,edge,true,13,9),"edge expansion failed");
    auto bounds=detectionBox(box);check(bounds==cv::Rect2d(3,1,7,7),"original pixel edge coordinates wrong");check(box["segmentation"]==rle,"box correction altered mask");check(box["bbox"]==Json::array({4,2,5,5}),"mask bounds altered");
    for(int edge=0;edge<4;++edge)check(nudgeEdge(box,edge,false,13,9),"edge shrink failed");check(detectionBox(box)==cv::Rect2d(4,2,5,5),"inverse edge corrections wrong");
    Json boundary={{"bbox",{0,0,1,1}}};for(int edge=0;edge<4;++edge)check(!nudgeEdge(boundary,edge,false,13,9),"subpixel box accepted");check(!nudgeEdge(boundary,0,true,13,9),"top image edge overflow");check(!nudgeEdge(boundary,2,true,13,9),"left image edge overflow");
    mask.at<uchar>(1,4)=1;auto changed=maskAnnotation(mask,box,"manual_brush");check(!changed.contains("detection_bbox"),"mask edit retained stale correction");check(changed["area"]==25,"brush mask area wrong");
    if(argc>1){
        Client client(std::stoi(argv[1]));auto catalog=client.get("/api/native/catalog");check(catalog["api_version"]==1,"native catalog incompatible");
        auto found=std::find_if(catalog["scenes"].begin(),catalog["scenes"].end(),[](const Json& scene){return scene.value("recording_number",0)==5&&scene.value("index",-1)==0;});check(found!=catalog["scenes"].end(),"test scene absent");
        auto member=(*found)["members"][0];auto query=Client::frameQuery(member,(*found)["recording"].get<std::string>());auto frame=client.get("/api/frame"+query);auto original=frame;
        check(!frame["boxes"].empty(),"test ball absent");check(!client.image(member,(*found)["recording"].get<std::string>()).empty(),"native image decode failed");
        auto oldMask=frame["boxes"][0]["segmentation"];check(nudgeEdge(frame["boxes"][0],0,true,frame["width"],frame["height"]),"test correction failed");frame["reviewed"]=false;frame["review_status"]="editing";
        auto saved=client.post("/api/annotation",annotationPayload(frame,catalog["summary"]["revision"]));auto reread=client.get("/api/frame"+query);check(reread["boxes"][0]["segmentation"]==oldMask,"HTTP save lost mask");check(reread["boxes"][0].contains("detection_bbox"),"HTTP save lost edge correction");
        auto rectangle=detectionBox(original["boxes"][0]);Json prompt=annotationPayload(original,saved["summary"]["revision"]);prompt["points"]={{{"x",rectangle.x+rectangle.width/2},{"y",rectangle.y+rectangle.height/2},{"label",1}}};prompt["radius"]=18;
        auto proposed=client.post("/api/assist/segment",prompt);auto& inferred=proposed["proposals"][0];check(inferred.contains("segmentation")&&inferred.value("source","")=="efficientsam","real SAM inference failed");
        original["review_status"]=original.value("reviewed",false)?"reviewed":"editing";client.post("/api/annotation",annotationPayload(original,saved["summary"]["revision"]));
        std::cout<<"PASS: native HTTP image, mask-preserving save/reload and actual EfficientSAM inference\n";
    }
    std::cout<<"PASS: RLE, original-pixel edge corrections, bounds, mask preservation and brush invalidation\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
