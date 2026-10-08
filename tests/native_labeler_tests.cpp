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
    check(memberState(Json::parse(R"({"selected":false,"status":"reviewed"})")) == MemberState::Duplicate, "unselected frame is a duplicate");
    check(memberState(Json::parse(R"({"status":"reviewed"})")) == MemberState::Done && memberState(Json::parse(R"({"status":"hold"})")) == MemberState::Hold
        && memberState(Json::parse(R"({"status":"editing"})")) == MemberState::Editing && memberState(Json::object()) == MemberState::Todo, "member state mapping wrong");
    auto scene = Json::parse(R"({"members":[{"count":2},{"count":2},{"count":3,"selected":false}]})");
    check(!countsDiffer(scene), "duplicates must not trigger a count warning");
    scene["members"][1]["count"] = 1;
    check(countsDiffer(scene), "different ball counts must warn");
    std::vector<Json> annotations = {Json::parse(R"({"boxes":[{"bbox":[50,0,5,5]},{"bbox":[10,0,5,5]}]})"), Json::parse(R"({"boxes":[{"bbox":[5,0,5,5]}]})")};
    auto order = ballOrder(annotations);
    check(order.size() == 3 && order[0].camera == 0 && order[0].index == 1 && order[1].index == 0 && order[2].camera == 1, "ball order must follow camera then x");
    auto begin = nextBall(order, {}, 1), wrapped = nextBall(order, order[2], 1), back = nextBall(order, order[0], -1);
    check(begin.camera == 0 && begin.index == 1 && wrapped.camera == 0 && wrapped.index == 1 && back.camera == 1, "ball cycling wrong");
    check(nextBall({}, {}, 1).camera == -1, "empty scene has no ball to select");
    auto stats = Json::parse(R"({"a":{"number":1,"day":"2026-10-02","images":420,"balls":563,"empty":0},
                                 "b":{"number":2,"day":"2026-10-02","images":492,"balls":935,"empty":39},
                                 "c":{"number":3,"day":"2026-10-02","images":629,"balls":1322,"empty":0}})");
    auto splits = Json::parse(R"({"a":"test","b":"train","c":"val"})"), groups = Json::parse(R"({"a":"a","b":"b","c":"c"})");
    auto totals = splitTotals(stats, splits);
    check(totals["train"].images == 492 && totals["train"].empty == 39 && totals["test"].recordings == 1 && totals["val"].balls == 1322, "split totals wrong");
    auto checks = splitChecks(stats, splits, groups);
    check(checks.size() == 5 && checks[0].ok && checks[1].ok && !checks[2].ok && !checks[3].ok && !checks[4].ok, "split checks wrong");
    check(checks[2].detail.find("02") != std::string::npos, "empty-image check must name the recordings that have them");
    groups["b"] = "a";
    check(!splitChecks(stats, splits, groups)[0].ok, "a group across two splits must fail");
    splits["a"] = "unassigned";
    check(!splitChecks(stats, splits, groups)[1].ok, "a split without images must fail");

    auto batches = Json::parse(R"({"a":"a","b":"b","c":"c","d":"d"})");
    mergeGroups(batches, {"c", "b"});
    check(batches["b"] == "b" && batches["c"] == "b" && batches["a"] == "a" && batches["d"] == "d", "a merged group must be named after its first member");
    mergeGroups(batches, {"d", "c"});
    check(batches["b"] == "b" && batches["c"] == "b" && batches["d"] == "b", "merging with one member of a group must take the whole group");
    leaveGroup(batches, "b");
    check(batches["b"] == "b" && batches["c"] == "c" && batches["d"] == "c", "the rest of a group must not keep the id of the recording that left");
    leaveGroup(batches, "d");
    check(batches["c"] == "c" && batches["d"] == "d", "leaving a pair must leave two single recordings");

    const std::vector<cv::Rect2d> balls = {{100, 100, 40, 40}, {130, 100, 40, 40}};   // two touching balls; their boxes overlap by 10 pixels
    check(replaceTarget(balls, {110, 120}, -1) == 0 && replaceTarget(balls, {160, 120}, -1) == 1, "a point inside one box must target that ball");
    check(replaceTarget(balls, {135, 120}, -1) == 1 && replaceTarget(balls, {135, 120}, 0) == 0, "where boxes overlap the selected ball wins, otherwise the later one");
    check(replaceTarget(balls, {60, 60}, 0) == -1, "a point outside every box is a new ball even with a ball selected");
    const auto outlineCenter = boundsCenter({{90, 95}, {150, 95}, {150, 145}, {90, 145}});
    check(about(float(outlineCenter.x), 120) && about(float(outlineCenter.y), 120), "the center of an outline is the middle of its bounds");
    check(replaceTarget(balls, boundsCenter({{96, 96}, {144, 96}, {144, 144}, {96, 144}}), -1) == 0, "re-tracing a ball from outside its box must still replace it");
    check(replaceTarget(balls, boundsCenter({{101, 101}, {60, 101}, {60, 60}, {101, 60}}), -1) == -1, "an outline that only starts inside a neighbour's box must not replace the neighbour");

    std::vector<cv::Point> ring, arc;
    for (int degree = 0; degree < 360; degree += 10) {
        const cv::Point point(int(std::lround(100 + 20 * std::cos(degree * 3.14159265 / 180))), int(std::lround(50 + 20 * std::sin(degree * 3.14159265 / 180))));
        ring.push_back(point); if (degree <= 180) arc.push_back(point);
    }
    const auto full = fitCircle(ring), half = fitCircle(arc);
    check(about(float(full.x), 100, .3f) && about(float(full.y), 50, .3f) && about(float(full.r), 20, .3f), "a circle fitted to a ring must find its center and radius");
    check(about(float(half.x), 100, 1) && about(float(half.y), 50, 1) && about(float(half.r), 20, 1), "a circle fitted to a half ring, as of a part-hidden ball, must still find the whole ball");
    check(fitCircle({{3, 4}}).r == 0 && fitCircle({}).r == 0, "too few points give no circle");
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
        Client client(std::stoi(argv[1]));auto catalog=client.get("/api/native/catalog");check(catalog["api_version"]==3,"native catalog incompatible");
        auto found=std::find_if(catalog["scenes"].begin(),catalog["scenes"].end(),[](const Json& scene){return scene.value("recording_number",0)==5&&scene.value("index",-1)==0;});check(found!=catalog["scenes"].end(),"test scene absent");
        auto member=(*found)["members"][0];auto query=Client::frameQuery(member,(*found)["recording"].get<std::string>());auto frame=client.get("/api/frame"+query);auto original=frame;
        check(!frame["boxes"].empty(),"test ball absent");check(!client.image(member,(*found)["recording"].get<std::string>()).empty(),"native image decode failed");
        auto oldMask=frame["boxes"][0]["segmentation"];check(nudgeEdge(frame["boxes"][0],0,true,frame["width"],frame["height"]),"test correction failed");frame["reviewed"]=false;frame["review_status"]="editing";
        auto saved=client.post("/api/annotation",annotationPayload(frame,catalog["summary"]["revision"]));auto reread=client.get("/api/frame"+query);check(reread["boxes"][0]["segmentation"]==oldMask,"HTTP save lost mask");check(reread["boxes"][0].contains("detection_bbox"),"HTTP save lost edge correction");
        auto rectangle=detectionBox(original["boxes"][0]);Json prompt=annotationPayload(original,saved["summary"]["revision"]);prompt["points"]={{{"x",rectangle.x+rectangle.width/2},{"y",rectangle.y+rectangle.height/2},{"label",1}}};prompt["radius"]=18;
        auto proposed=client.post("/api/assist/segment",prompt);auto& inferred=proposed["proposals"][0];check(inferred.contains("segmentation")&&inferred.value("source","")=="efficientsam","real SAM inference failed");
        Json boxed=annotationPayload(original,saved["summary"]["revision"]);boxed["points"]=prompt["points"];boxed["radius"]=18;boxed["box"]={rectangle.x-2,rectangle.y-2,rectangle.width+4,rectangle.height+4};
        auto boxProposal=client.post("/api/assist/segment",boxed);check(boxProposal["proposals"][0].contains("segmentation"),"box prompted SAM inference failed");
        original["review_status"]=original.value("reviewed",false)?"reviewed":"editing";client.post("/api/annotation",annotationPayload(original,saved["summary"]["revision"]));
        std::cout<<"PASS: native HTTP image, mask-preserving save/reload and actual EfficientSAM inference\n";
    }
    std::cout<<"PASS: RLE, original-pixel edge corrections, bounds, mask preservation and brush invalidation\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}}
