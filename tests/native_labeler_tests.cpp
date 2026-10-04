#include "native_labeler_model.hpp"
#include "native_labeler_client.hpp"
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <stdexcept>

using namespace boccia::labeler;
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
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
