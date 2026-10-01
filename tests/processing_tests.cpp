#include "recording.hpp"
#include "ball_detection.hpp"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <iostream>
#include <thread>
#include <functional>
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static void rejects(const std::function<void()>& work){try{work();}catch(const std::exception&){return;}throw std::runtime_error("Expected rejection");}
static void finished(RawRecorder& r){for(int n=0;n<500&&!r.statistics().finished;++n)std::this_thread::sleep_for(std::chrono::milliseconds(10));require(r.statistics().finished,"Writer must finish");}
int main(int argc,char** argv){try{
    namespace fs=std::filesystem;auto folder=fs::temp_directory_path()/("boccia-processing-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(folder);
    if(argc==3&&std::string(argv[1])=="--compression-probe"){
        auto image=cv::imread(argv[2],cv::IMREAD_GRAYSCALE);require(!image.empty(),"Cannot read probe image");
        Settings snapshot;RecordedCamera camera{"probe","calibration sample","Probe",{}, {}};auto target=folder/"probe";
        auto before=image.clone();RawRecorder compressed(target,{camera},snapshot,256ull*1024*1024,{true,0});
        RawFrameInfo f{uint32_t(image.cols),uint32_t(image.rows),0,0,0x01080001,0,1,1,1,image.total()};
        for(int n=0;n<30;++n){f.id=n+1;f.hostNs=uint64_t(n)*4000000;compressed.submit(0,f,{image.data,image.total()});}
        compressed.stop();finished(compressed);auto status=compressed.statistics();require(status.error.empty(),"Probe recording failed");RawReplay read(target);
        require(read.readRaw(0,12)==std::vector<uint8_t>(image.data,image.data+image.total())&&cv::norm(image,before,cv::NORM_INF)==0,"Probe byte identity");
        std::cout<<image.cols<<"x"<<image.rows<<" Mono8 sample: "<<status.rawBytes<<" -> "<<status.bytes<<" bytes, "<<100.*(1.-double(status.bytes)/status.rawBytes)<<"% smaller, "<<status.rawBytes/status.seconds/1e6<<" MB/s including disk writes; byte exact\n";return 0;
    }
    cv::Mat image(320,480,CV_8UC3,cv::Scalar(20,20,20));cv::circle(image,{150,150},32,{245,245,245},-1,cv::LINE_AA);auto original=image.clone();
    BallOptions options;options.scale=1;options.minRadius=24;options.maxRadius=40;options.houghVotes=18;options.minCoverage=.4;
    auto balls=detectBalls(image,options);require(!balls.balls.empty(),"Detect synthetic circle");
    auto found=std::min_element(balls.balls.begin(),balls.balls.end(),[](const auto& a,const auto& b){return cv::norm(a.center-cv::Point2f(150,150))<cv::norm(b.center-cv::Point2f(150,150));});
    require(cv::norm(found->center-cv::Point2f(150,150))<1.5&&std::abs(found->radius-32)<2,"Subpixel circle fit recovers known circle");
    require(cv::norm(image,original,cv::NORM_INF)==0,"Detector leaves raw image unchanged");
    auto scaled=options;scaled.scale=.5;auto reduced=detectBalls(image,scaled);require(!reduced.balls.empty()&&cv::norm(reduced.balls.front().center-cv::Point2f(150,150))<1.5,"Scaled detection maps pixel centers back to native coordinates");
    options.roi={{.65f,.1f},{.95f,.1f},{.95f,.9f},{.65f,.9f}};require(detectBalls(image,options).balls.empty(),"ROI excludes outside circle");
    options.roiEnabled=false;require(!detectBalls(image,options).balls.empty(),"Disabling ROI restores circle");
    options.color=true;for(auto [color,name]:std::vector<std::pair<cv::Scalar,std::string>>{{{0,0,240},"Red"},{{240,0,0},"Blue"},{{230,230,230},"White"},{{0,220,0},"Unknown"}}){
        cv::Mat sample(100,100,CV_8UC3,color);require(classifyBallColor(sample,{{50,50},25},options)==name,"HSV ball color classification");}
    std::stop_source stop;stop.request_stop();rejects([&]{detectBalls(image,options,stop.get_token());});
    auto bad=options;bad.roi={{0,0},{1,1},{0,1},{1,0}};rejects([&]{bad.validate();});bad=options;bad.scale=NAN;rejects([&]{bad.validate();});
    require(nlohmann::json(options).get<BallOptions>()==options,"Ball configuration JSON round trip");
    Settings settings;settings.ball.run=true;settings.ball.cameras["mono-A"]=options;settings.cameraNames["mono-A"]="Left";
    writeSettings(folder/"profile.json",settings);require(loadSettings(folder/"profile.json").ball.cameras.at("mono-A")==options,"Single profile persists processing parameters");
    settings.recording={true,24};writeSettings(folder/"profile.json",settings);auto savedOptions=loadSettings(folder/"profile.json").recording;
    require(savedOptions.lossless&&savedOptions.fps==24,"Recording FPS and lossless preference persist");
    rejects([]{RecordingOptions{true,-1}.validate();});rejects([]{RecordingOptions{true,NAN}.validate();});
    std::vector<RecordedCamera> cameras{{"mono-A","TestMono","Left",{},{{"PixelFormat","Mono8"},{"ExposureTime","1234"}}},{"color-B","TestColor","Color",{},{{"PixelFormat","BGR8"}}}};
    std::vector<uint8_t> mono(64*48,72),color(64*48*3);for(size_t i=0;i<color.size();i+=3){color[i]=31;color[i+1]=91;color[i+2]=201;}
    RawFrameInfo m{64,48,0,0,0x01080001,0,100,9876,1000000000,mono.size()};auto c=m;c.pixelFormat=0x02180015;c.size=color.size();c.hostNs+=100000;
    auto monoCopy=mono,colorCopy=color;
    auto decoded=decodeRawFrame(m,mono);require(cv::norm(decoded,cv::Mat(48,64,CV_8UC3,cv::Scalar(72,72,72)),cv::NORM_INF)==0,"Offline Mono8 conversion");
    auto rgb=decodeRawFrame(c,color);require(rgb.at<cv::Vec3b>(10,10)==cv::Vec3b(31,91,201),"Offline BGR8 conversion preserves channel order");
    auto bayer=m;bayer.pixelFormat=0x01080009;require(cv::norm(decodeRawFrame(bayer,mono),decoded,cv::NORM_INF)==0,"Offline Bayer conversion without SDK camera handle");
    auto mosaic=mono;for(int y=0;y<48;++y)for(int x=0;x<64;++x)mosaic[y*64+x]=uint8_t(y%2?(x%2?10:128):(x%2?128:250));
    require(decodeRawFrame(bayer,mosaic).at<cv::Vec3b>(20,20)==cv::Vec3b(10,128,250),"Bayer RG channels retain correct pattern and color order");
    auto padded=m;padded.paddingX=4;padded.size=68*48;std::vector<uint8_t> padding(size_t(padded.size),0);for(int y=0;y<48;++y)std::fill_n(padding.begin()+y*68,64,72);
    require(cv::norm(decodeRawFrame(padded,padding),decoded,cv::NORM_INF)==0,"Raw row padding is preserved and decoded");
    require(monoCopy==mono&&colorCopy==color,"Raw converter leaves payload byte identical");
    {
        RawRecorder compressed(folder/"compressed",cameras,settings,256ull*1024*1024,{true,0});
        for(int n=0;n<20;++n){auto left=m,right=c;left.id=right.id=n+1;left.hostNs=right.hostNs=uint64_t(n)*4000000;compressed.submit(0,left,mono);compressed.submit(1,right,color);}
        compressed.stop();finished(compressed);auto status=compressed.statistics();require(status.error.empty()&&status.bytes<status.rawBytes/3,"Lossless shrinks compressible input");
        RawReplay read(folder/"compressed");require(read.readRaw(0,13)==mono&&read.readRaw(1,2)==color,"Compressed arbitrary-frame seeks restore every original byte");
        require(read.index[0][0].codec==1&&read.read(1,2).image.at<cv::Vec3b>(3,3)==cv::Vec3b(31,91,201),"Compressed source uses same converter");
        auto rawPath=folder/"compressed"/"camera-0"/"frames.raw";std::fstream corrupt(rawPath,std::ios::binary|std::ios::in|std::ios::out);auto offset=read.index[0][0].offset+read.index[0][0].storedSize-1;
        corrupt.seekg(offset);char value=0;corrupt.get(value);value^=1;corrupt.seekp(offset);corrupt.put(value);corrupt.flush();rejects([&]{read.readRaw(0,0);});
    }
    {
        auto random=mono;cv::RNG randomBytes(1024);randomBytes.fill(cv::Mat(48,64,CV_8U,random.data()),cv::RNG::UNIFORM,0,256);
        RawRecorder rawFallback(folder/"noise",{cameras[0]},settings,256ull*1024*1024,{true,0});rawFallback.submit(0,m,random);rawFallback.stop();finished(rawFallback);
        RawReplay read(folder/"noise");require(read.index[0][0].codec==0&&read.readRaw(0,0)==random,"Incompressible input falls back to exact raw without expansion");
    }
    {
        auto third=cameras[0];third.serial="mono-C";RawRecorder sampled(folder/"sampled",{cameras[0],cameras[1],third},settings,256ull*1024*1024,{true,30});
        for(int n=0;n<250;++n)for(size_t camera=0;camera<3;++camera){auto f=camera==1?c:m;f.id=1000+n+(n>=100?1:0);f.hostNs=uint64_t(n)*4000000+camera*100000;
            sampled.submit(camera,f,camera==1?color:mono);}
        sampled.stop();finished(sampled);auto status=sampled.statistics();require(status.error.empty(),"FPS-limited recording succeeds");
        RawReplay read(folder/"sampled");for(size_t camera=0;camera<3;++camera){require(status.frames[camera]==30&&status.sampledOut[camera]==220,"250 FPS input stores exactly 30 frames per second");
            require(status.gaps[camera]==1&&read.cameras[camera].gaps==1,"Intentional skips are not camera drops");
            for(size_t frame=0;frame<30;++frame)require(read.index[camera][frame].id==read.index[0][frame].id,"Common receipt-time buckets select corresponding synthetic frames");}
    }
    {
        RawFrameInfo packed=m;packed.pixelFormat=0x010C0006;packed.size=64*48*12/8;std::vector<uint8_t> payload(size_t(packed.size));for(size_t i=0;i<payload.size();++i)payload[i]=uint8_t(i%13+80);
        RawRecorder preserve(folder/"packed",{cameras[0]},settings,256ull*1024*1024,{true,0});preserve.submit(0,packed,payload);preserve.stop();finished(preserve);
        RawReplay read(folder/"packed");require(read.readRaw(0,0)==payload&&read.index[0][0].filterStride==0,"Packed 12-bit bytes remain lossless without byte predictor");
    }
    auto truncated=m;truncated.size=2;rejects([&]{decodeRawFrame(truncated,std::span(mono.data(),2));});
    auto capture=folder/"capture";RawRecorder recorder(capture,cameras,settings);
    for(int n=0;n<150;++n){m.id=100+n;c.id=200+n;m.timestamp+=4000;c.timestamp+=4000;m.hostNs+=4000000;c.hostNs+=4000000;recorder.submit(0,m,mono);recorder.submit(1,c,color);}
    recorder.stop();finished(recorder);auto status=recorder.statistics();require(status.error.empty()&&status.frames[0]==150&&status.frames[1]==150,"Record every source frame, not sampled previews");
    RawReplay replay(capture);require(replay.index[0].size()==150&&replay.index[1].size()==150,"Read all frame indexes");
    require(replay.cameras[0].settings.at("ExposureTime")=="1234"&&replay.settings.cameraNames.at("mono-A")=="Left","Recording carries device settings and profile snapshot");
    require(replay.nearest(1,replay.index[0][43].hostNs)==43&&replay.nearest(1,0)==0&&replay.nearest(1,UINT64_MAX)==149,"Nearest receipt time pairing and bounds");
    auto read=replay.read(1,43);require(read.info.id==243&&read.info.timestamp==9876+44*4000&&read.image.at<cv::Vec3b>(5,5)==cv::Vec3b(31,91,201),"Seek preserves frame IDs timestamps and pixels");
    {std::ifstream raw(capture/"camera-0"/"frames.raw",std::ios::binary);std::vector<uint8_t> bytes(mono.size());raw.read(reinterpret_cast<char*>(bytes.data()),bytes.size());require(bytes==mono,"On-disk raw bytes are exact");}
    {
        auto legacy=folder/"legacy";fs::copy(capture,legacy,fs::copy_options::recursive);nlohmann::json manifest;{std::ifstream in(legacy/"session.json");in>>manifest;}
        manifest["format"]="irayple-raw-v1";{std::ofstream out(legacy/"session.json");out<<manifest;}
        for(int camera=0;camera<2;++camera){auto path=legacy/("camera-"+std::to_string(camera))/"frames.jsonl";std::ifstream in(path);std::string line,content;
            while(std::getline(in,line)){auto j=nlohmann::json::parse(line);for(const char* key:{"stored_size","codec","filter_stride","source_gaps","source_invalid","sampled_out"})j.erase(key);content+=j.dump()+"\n";}
            in.close();std::ofstream out(path);out<<content;}
        RawReplay old(legacy);require(old.readRaw(0,42)==mono&&old.readRaw(1,17)==color,"Old uncompressed v1 recordings remain readable");
    }
    RawRecorder limited(folder/"overflow",{cameras[0]},settings,8);limited.submit(0,m,mono);finished(limited);require(!limited.statistics().accepting&&!limited.statistics().error.empty(),"Queue overflow visibly stops recording");
    {std::ofstream index(capture/"camera-0"/"frames.jsonl",std::ios::app);index<<"{incomplete";}
    RawReplay partial(capture);require(partial.index[0].size()==150&&!partial.warning.empty(),"Recover complete frames from interrupted final index line");
    rejects([&]{RawReplay cancelled(capture,stop.get_token());});
    {RawRecorder incomplete(folder/"invalid",{cameras[0]},settings);m.id=1;incomplete.submit(0,m,mono);m.id=2;m.status=1;m.hostNs+=4000000;incomplete.submit(0,m,mono);
        m.id=4;m.status=0;m.hostNs+=4000000;incomplete.submit(0,m,mono);incomplete.stop();finished(incomplete);}
    RawReplay invalid(folder/"invalid");require(invalid.index[0].size()==2&&invalid.cameras[0].gaps==1&&invalid.cameras[0].invalid==1&&!invalid.warning.empty(),"Replay reports source gaps and skips invalid frames");
    std::cout<<"PASS: circle fit, ROI, color, cancellation, profile, raw byte preservation, 300 frame recording, replay seek, timestamp pairing, overflow and partial recovery\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
