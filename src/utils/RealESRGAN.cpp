#include "RealESRGAN.h"
#include "SRPixelConvert.h"
#include "FluxPlayer/utils/Logger.h"
#include "FluxPlayer/utils/Config.h"
#include "../../third_party/onnxruntime/include/onnxruntime_c_api.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
extern "C" {
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}
#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#else
#include <dlfcn.h>
#endif

namespace FluxPlayer {
namespace {
using SRClock = std::chrono::steady_clock;
double milliseconds(SRClock::time_point from, SRClock::time_point to) {
    return std::chrono::duration<double,std::milli>(to-from).count();
}
constexpr int kTile = 256, kHalo = 32, kCore = kTile - 2 * kHalo;
/// 模型哈希锁定导出产物，禁止同名替换导致形状/前处理契约失效。
std::string modelHash(const std::string& path) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read model");
    auto sha = std::unique_ptr<AVSHA, void(*)(AVSHA*)>(av_sha_alloc(), [](AVSHA* p) { av_free(p); });
    if (!sha || av_sha_init(sha.get(), 256) < 0) throw std::runtime_error("Cannot initialize model digest");
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
        av_sha_update(sha.get(), reinterpret_cast<const uint8_t*>(buffer.data()), input.gcount());
    if (!input.eof()) throw std::runtime_error("Model read failed");
    uint8_t digest[32]; av_sha_final(sha.get(), digest);
    const char* hex = "0123456789abcdef";
    std::string result;
    for (auto b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
    return result;
}
/// Reflect-101 保持边缘连续；长度为 1 的输入退化为复制。
int reflect(int x, int length) {
    if (length <= 1) return 0;
    while (x < 0 || x >= length) x = x < 0 ? -x : 2 * length - 2 - x;
    return x;
}
#ifdef _WIN32
struct SrAdapter { unsigned index = 0; std::string name = "adapter 0"; };
SrAdapter preferredSrAdapter() {
    SrAdapter result;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
    LUID preferred{}; bool havePreferred = false;
    IDXGIFactory6* modern = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&modern)))) {
        IDXGIAdapter1* adapter = nullptr;
        if (SUCCEEDED(modern->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                preferred = desc.AdapterLuid; havePreferred = true;
            }
            adapter->Release();
        }
        modern->Release();
    }
    size_t largestMemory = 0; bool selected = false;
    for (unsigned i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;
        DXGI_ADAPTER_DESC1 desc{}; const HRESULT rc = adapter->GetDesc1(&desc); adapter->Release();
        if (FAILED(rc) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        const bool matches = havePreferred && desc.AdapterLuid.HighPart == preferred.HighPart && desc.AdapterLuid.LowPart == preferred.LowPart;
        if (matches || (!havePreferred && (!selected || desc.DedicatedVideoMemory > largestMemory))) {
            result.index = i; result.name = std::filesystem::path(desc.Description).u8string();
            largestMemory = desc.DedicatedVideoMemory; selected = true;
            if (matches) break;
        }
    }
    factory->Release();
    return result;
}
#endif
}
struct RealESRGAN::Impl {
    void* library = nullptr;
#ifdef __APPLE__
    std::string coremlFormat = "NeuralNetwork";
#endif
#ifdef _WIN32
    HMODULE directml = nullptr;
    std::filesystem::path libraryDirectory;
    SrAdapter adapter;
#endif
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    OrtAllocator* allocator = nullptr;
    std::string inputName, outputName, provider, digest, runtimeVersion;
    bool externalModel = false;
    std::vector<float> inputBuffer, outputBuffer;
    OrtValue *inputTensor = nullptr, *outputTensor = nullptr;
    std::array<float,256> normalized{};
    std::vector<std::array<int,kTile>> reflectedX, reflectedY;
    int indexedWidth = 0, indexedHeight = 0;
    static int scaleFromShape(const std::array<int64_t,4>& shape) {
        if (shape[0]!=1 || shape[1]!=3 || shape[2]<=0 || shape[2]!=shape[3] || shape[2]%kTile)
            throw std::runtime_error("External model output must be a fixed square RGB tile with an integer scale");
        if(shape[2]>4096)throw std::runtime_error("External model output tile exceeds the 4096-pixel safety limit");
        return static_cast<int>(shape[2]/kTile);
    }
    void detectScale() {
        // Let ORT allocate the probe output; never assume the user's model is x2/x4.
        std::vector<float> pixels(3*kTile*kTile,0.5f);
        const int64_t shape[]={1,3,kTile,kTile};
        OrtValue* rawInput=nullptr;
        check(api->CreateTensorWithDataAsOrtValue(memory,pixels.data(),pixels.size()*sizeof(float),
            shape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&rawInput));
        auto input=std::unique_ptr<OrtValue,void(*)(OrtValue*)>(rawInput,api->ReleaseValue);
        const char* in=inputName.c_str(); const char* out=outputName.c_str();
        const OrtValue* inputs[]={input.get()};
        OrtValue* rawOutput=nullptr;
        auto* status=api->Run(session,nullptr,&in,inputs,1,&out,1,&rawOutput);
        auto output=std::unique_ptr<OrtValue,void(*)(OrtValue*)>(rawOutput,api->ReleaseValue);
        check(status);
        OrtTensorTypeAndShapeInfo* rawInfo=nullptr;
        check(api->GetTensorTypeAndShape(output.get(),&rawInfo));
        auto info=std::unique_ptr<OrtTensorTypeAndShapeInfo,void(*)(OrtTensorTypeAndShapeInfo*)>(rawInfo,api->ReleaseTensorTypeAndShapeInfo);
        size_t count=0; ONNXTensorElementDataType dtype;
        check(api->GetDimensionsCount(info.get(),&count));
        check(api->GetTensorElementType(info.get(),&dtype));
        if(count!=4 || dtype!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error("External model must produce one FP32 NCHW RGB tensor");
        std::array<int64_t,4> actual{};
        check(api->GetDimensions(info.get(),actual.data(),actual.size()));
        scale=scaleFromShape(actual);
        LOG_INFO("Offline SR: external model native scale detected from inference: "+std::to_string(scale)+"x");
    }
    void prepareInput(int width,int height) {
        if(!inputTensor) {
            inputBuffer.resize(3*kTile*kTile);
            const int64_t shape[]={1,3,kTile,kTile};
            check(api->CreateTensorWithDataAsOrtValue(memory,inputBuffer.data(),inputBuffer.size()*sizeof(float),
                shape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&inputTensor));
            for(size_t i=0;i<normalized.size();++i)normalized[i]=float(i)/255.f;
            const int side=kTile*scale;
            outputBuffer.resize(size_t(3)*side*side);
            const int64_t outputShape[]={1,3,side,side};
            check(api->CreateTensorWithDataAsOrtValue(memory,outputBuffer.data(),outputBuffer.size()*sizeof(float),
                outputShape,4,ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,&outputTensor));
        }
        if(indexedWidth==width && indexedHeight==height)return;
        reflectedX.resize((width+kCore-1)/kCore);reflectedY.resize((height+kCore-1)/kCore);
        for(size_t block=0;block<reflectedX.size();++block)
            for(int i=0;i<kTile;++i)reflectedX[block][i]=reflect(int(block)*kCore+i-kHalo,width);
        for(size_t block=0;block<reflectedY.size();++block)
            for(int i=0;i<kTile;++i)reflectedY[block][i]=reflect(int(block)*kCore+i-kHalo,height);
        indexedWidth=width;indexedHeight=height;
    }
    int scale = 2;
    bool profiling = false;
    unsigned profileRuns = 0;
    void endProfile() noexcept {
        if(!profiling || !session)return;
        profiling=false;char* path=nullptr;
        auto* status=api->SessionEndProfiling(session,allocator,&path);
        if(status){LOG_WARN(std::string("ORT profile finalization: ")+api->GetErrorMessage(status));api->ReleaseStatus(status);}
        else if(path){LOG_INFO(std::string("Offline SR ORT profile: ")+path);allocator->Free(allocator,path);}
    }
    uint64_t measuredFrames = 0;
    uint64_t reusedTileCount = 0;
    double prepareMs = 0, runMs = 0, stitchMs = 0;
    void report(double prepare, double run, double stitch, int tiles) {
        ++measuredFrames;
        // 首帧单列；后续统计不含首帧，避免初始化成本混入后续均值。
        if(measuredFrames>1) {prepareMs+=prepare;runMs+=run;stitchMs+=stitch;}
        if(measuredFrames==1 || measuredFrames%30==0) {
            const double count=measuredFrames==1?1:double(measuredFrames-1);
            LOG_INFO("Offline SR detail: "+std::string(measuredFrames==1?"first frame":"post-first average")+
                ", frames="+std::to_string(measuredFrames)+", tiles/frame="+std::to_string(tiles)+
                ", prepare ms="+std::to_string(measuredFrames==1?prepare:prepareMs/count)+
                ", ORT Run ms="+std::to_string(measuredFrames==1?run:runMs/count)+
                ", stitch ms="+std::to_string(measuredFrames==1?stitch:stitchMs/count));
        }
    }
    ~Impl() {
        if (api) {
            endProfile();
            if (inputTensor) api->ReleaseValue(inputTensor);
            if (outputTensor) api->ReleaseValue(outputTensor);
            if (session) api->ReleaseSession(session);
            if (memory) api->ReleaseMemoryInfo(memory);
            if (env) api->ReleaseEnv(env);
        }
#ifdef _WIN32
        if (library) FreeLibrary(static_cast<HMODULE>(library));
        if (directml) FreeLibrary(directml);
#else
        if (library) dlclose(library);
#endif
    }
    void check(OrtStatus* status) {
        if (!status) return;
        std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        throw std::runtime_error("ORT: " + message);
    }
    void createSession(const std::string& model, bool accelerated) {
        OrtSessionOptions* raw = nullptr;
        check(api->CreateSessionOptions(&raw));
        auto options = std::unique_ptr<OrtSessionOptions, void(*)(OrtSessionOptions*)>(raw, api->ReleaseSessionOptions);
        // 有界利用 CPU；不改变模型、精度和分块，避免固定双线程限制 CPU 回退吞吐。
        const unsigned threads=std::min(4u,std::max(1u,std::thread::hardware_concurrency()));
        check(api->SetIntraOpNumThreads(raw, static_cast<int>(threads)));
        check(api->SetSessionGraphOptimizationLevel(raw, ORT_ENABLE_ALL));
        const char* profile=std::getenv("FLUX_SR_PROFILE");
        if(profile && *profile) {
            auto prefix=std::filesystem::u8path(profile);
            if(!prefix.parent_path().empty())std::filesystem::create_directories(prefix.parent_path());
#ifdef _WIN32
            prefix+=accelerated?"-directml":"-cpu";
#else
            prefix+=accelerated?"-coreml":"-cpu";
#endif
            check(api->EnableProfiling(raw,prefix.c_str()));
            check(api->SetSessionLogSeverityLevel(raw,0));
            check(api->SetSessionLogVerbosityLevel(raw,1));
        }
#ifdef __APPLE__
        if (accelerated) {
            std::string computeUnits="ALL";
            if(const char* requested=std::getenv("FLUX_SR_COREML_COMPUTE_UNITS")) {
                computeUnits=requested;
                if(computeUnits!="ALL" && computeUnits!="CPUAndGPU")
                    throw std::runtime_error("FLUX_SR_COREML_COMPUTE_UNITS must be ALL or CPUAndGPU");
            }
            // 编译缓存以真实 ORT 版本、已校验模型及固定 EP/shape 配置隔离。
            auto cache=std::filesystem::u8path(Config::getAppDataDir())/"sr-coreml"/
                (runtimeVersion+"-"+digest+"-256-"+coremlFormat+"-"+computeUnits+"-static-acc32");
            std::error_code ec;
            if (!externalModel) std::filesystem::create_directories(cache,ec);
            const std::string cachePath=cache.u8string();
            const char* keys[6] = {"ModelFormat", "MLComputeUnits", "RequireStaticInputShapes", "ModelCacheDirectory"};
            const char* values[6] = {coremlFormat.c_str(), computeUnits.c_str(), "1", cachePath.c_str()};
            // 外部 ONNX 可能引用独立权重文件；仅主文件散列不足以标识整个模型。
            // 不复用其持久编译缓存，避免权重变化后命中旧缓存。
            size_t optionCount=(ec || externalModel)?3:4;
            keys[optionCount]="AllowLowPrecisionAccumulationOnGPU";values[optionCount++]="0";
            const char* computePlan=std::getenv("FLUX_SR_COREML_COMPUTE_PLAN");
            if(computePlan && std::string(computePlan)=="1") {
                keys[optionCount]="ProfileComputePlan";values[optionCount++]="1";
                // ORT writes the plan to stderr via NSLog, not the ORT JSON profile.
                // Preferred devices / estimated cost weights are not measured GPU timings.
                LOG_INFO("Offline SR: CoreML ProfileComputePlan requested (macOS 14.4+); see stderr for device preferences and estimated cost weights");
            }
            if(ec)LOG_WARN("CoreML cache unavailable; continuing without cache: "+ec.message());
            check(api->SessionOptionsAppendExecutionProvider(raw, "CoreML", keys, values, optionCount));
            LOG_INFO("Offline SR: CoreML format="+coremlFormat+", compute units="+computeUnits+", low-precision accumulation=0");
        }
#elif defined(_WIN32)
        if (accelerated) {
            // 显式加载随应用部署的版本，避免依赖当前目录或系统 DirectML 版本。
            if (!directml) {
                directml = LoadLibraryExW((libraryDirectory / L"DirectML.dll").c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (!directml) throw std::runtime_error("Cannot load bundled DirectML.dll (Windows error " + std::to_string(GetLastError()) + ")");
            }
            using AppendDml = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);
            const auto append = reinterpret_cast<AppendDml>(GetProcAddress(static_cast<HMODULE>(library), "OrtSessionOptionsAppendExecutionProvider_DML"));
            if (!append) throw std::runtime_error("ONNX Runtime does not include DirectML EP");
            // DirectML 不支持 memory patterns 和并行 session execution。
            check(api->DisableMemPattern(raw));
            check(api->SetSessionExecutionMode(raw, ORT_SEQUENTIAL));
            // Diagnostic opt-in: changes CPU waiting only, not model arithmetic.
            // Alternating tests did not establish a reliable throughput gain;
            // keep the existing low-CPU wait policy as the default.
            const char* syncSpin=std::getenv("FLUX_SR_DML_CPU_SYNC_SPIN");
            const bool spinEnabled=syncSpin && std::string(syncSpin)=="1";
            check(api->AddSessionConfigEntry(raw,"ep.dml.enable_cpu_sync_spinning",spinEnabled?"1":"0"));
            adapter = preferredSrAdapter();
            check(append(raw, static_cast<int>(adapter.index)));
            LOG_INFO(std::string("Offline SR: DirectML CPU sync spinning=")+(spinEnabled?"1":"0"));
        }
#endif
#ifdef _WIN32
        const auto native = std::filesystem::u8path(model).wstring();
#else
        const auto native = model;
#endif
        check(api->CreateSession(env, native.c_str(), raw, &session));
        profiling=profile && *profile;profileRuns=0;
#ifdef _WIN32
        provider = accelerated ? "DirectML EP (adapter " + std::to_string(adapter.index) + "; " + adapter.name + "; FP32; may include CPU operators)" : "CPU EP (slow)";
#elif defined(__APPLE__)
        provider = accelerated ? "CoreML EP ("+coremlFormat+(coremlFormat=="NeuralNetwork"?" fast":" FP32")+"; may include CPU operators)" : "CPU EP (slow)";
        if(accelerated && coremlFormat=="NeuralNetwork")
            LOG_WARN("Offline SR: fast CoreML NeuralNetwork may use FP16 intermediates; output is not bit-exact with MLProgram FP32");
#else
        provider = accelerated ? "CoreML EP (may include CPU operators)" : "CPU EP (slow)";
#endif
    }
    void validate(bool output) {
        size_t count = 0;
        check(output ? api->SessionGetOutputCount(session, &count) : api->SessionGetInputCount(session, &count));
        if (count != 1) throw std::runtime_error("Expected exactly one model input/output");
        OrtTypeInfo* raw = nullptr;
        check(output ? api->SessionGetOutputTypeInfo(session, 0, &raw) : api->SessionGetInputTypeInfo(session, 0, &raw));
        auto type = std::unique_ptr<OrtTypeInfo, void(*)(OrtTypeInfo*)>(raw, api->ReleaseTypeInfo);
        const OrtTensorTypeAndShapeInfo* tensor = nullptr;
        check(api->CastTypeInfoToTensorInfo(raw, &tensor));
        if (!tensor) throw std::runtime_error("Model input/output is not a tensor");
        ONNXTensorElementDataType dtype;
        check(api->GetTensorElementType(tensor, &dtype));
        check(api->GetDimensionsCount(tensor, &count));
        if (dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || count != 4) throw std::runtime_error("Expected FP32 NCHW model");
        std::array<int64_t,4> shape{};
        check(api->GetDimensions(tensor, shape.data(), shape.size()));
        const int side = output ? kTile * scale : kTile;
        if (!externalModel) {
            if (shape != std::array<int64_t,4>{1,3,side,side}) throw std::runtime_error("Bundled model shape is invalid");
        } else if (!output) {
            const std::array<int64_t,4> expected{1,3,kTile,kTile};
            for (size_t i=0;i<shape.size();++i)
                if (shape[i]>0 && shape[i]!=expected[i])
                    throw std::runtime_error("External model must accept FP32 RGB NCHW [1,3,256,256] tiles");
        } else {
            if ((shape[0]>0 && shape[0]!=1) || (shape[1]>0 && shape[1]!=3))
                throw std::runtime_error("External model output must be batch 1 with 3 RGB channels");
            if (shape[2]>0 && shape[3]>0) scaleFromShape({1,3,shape[2],shape[3]});
        }
        char* name = nullptr;
        check(output ? api->SessionGetOutputName(session, 0, allocator, &name) : api->SessionGetInputName(session, 0, allocator, &name));
        (output ? outputName : inputName) = name;
        allocator->Free(allocator, name);
    }
};
RealESRGAN::RealESRGAN() : impl_(std::make_unique<Impl>()) {}
RealESRGAN::~RealESRGAN() = default;
void RealESRGAN::load(const std::string& library, const std::string& model, int scale, bool cpu, bool externalModel) {
    const auto loadStart=SRClock::now();
    impl_ = std::make_unique<Impl>();
    auto& p = *impl_; p.scale = scale; p.externalModel = externalModel;
    std::error_code modelError;
    const auto modelPath=std::filesystem::u8path(model);
    if (!std::filesystem::is_regular_file(modelPath, modelError) || modelError)
        throw std::runtime_error("Select one local regular ONNX model file");
    std::string extension=modelPath.extension().u8string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if (extension!=".onnx") throw std::runtime_error("External models must use the .onnx file format");
    if (!externalModel && scale != 2) throw std::runtime_error("Bundled model must be the verified x2 model");
    const std::string digest=modelHash(model);
    if (!externalModel) {
        if (scale != 2 || digest != "872bff6e0e5a7b28f2f9ecdeb5b941f8b79b7ea26cefe01664ec8a81fc687ec0")
            throw std::runtime_error("Bundled model must be the verified x2 model");
    }
    p.digest=digest;
#ifdef _WIN32
    const auto nativeLibrary = std::filesystem::absolute(std::filesystem::u8path(library));
    p.libraryDirectory = nativeLibrary.parent_path();
    p.library = LoadLibraryExW(nativeLibrary.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!p.library) throw std::runtime_error("Cannot load ONNX Runtime (Windows error " + std::to_string(GetLastError()) +
        "). Check bundled DLLs and Microsoft Visual C++ 2015-2022 x64 Redistributable: " + library);
    auto entry = p.library ? reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(static_cast<HMODULE>(p.library), "OrtGetApiBase")) : nullptr;
#else
    p.library = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto entry = p.library ? reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(dlsym(p.library, "OrtGetApiBase")) : nullptr;
#endif
    if (!entry || !(p.api = entry()->GetApi(ORT_API_VERSION))) throw std::runtime_error("Compatible ONNX Runtime unavailable: " + library);
    p.runtimeVersion=entry()->GetVersionString();
    p.check(p.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "FluxPlayerOfflineSR", &p.env));
    p.check(p.api->GetAllocatorWithDefaultOptions(&p.allocator));
    p.check(p.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &p.memory));
#if defined(__APPLE__) || defined(_WIN32)
    if (!cpu) {
#ifdef __APPLE__
        if(const char* requested=std::getenv("FLUX_SR_COREML_MODEL_FORMAT")) {
            p.coremlFormat=requested;
            if(p.coremlFormat!="NeuralNetwork" && p.coremlFormat!="MLProgram")
                throw std::runtime_error("FLUX_SR_COREML_MODEL_FORMAT must be NeuralNetwork or MLProgram");
        }
#endif
        auto tryAccelerated = [&] {
            try { p.createSession(model, true); }
            catch (const std::exception& e) {
                LOG_WARN(std::string("Accelerated SR initialization failed before output: ") + e.what());
                if (p.session) p.api->ReleaseSession(p.session);
                p.session = nullptr;
            }
        };
        tryAccelerated();
#ifdef __APPLE__
        if(!p.session && p.coremlFormat=="NeuralNetwork") {
            LOG_WARN("Offline SR: retrying standard CoreML MLProgram before CPU fallback");
            p.coremlFormat="MLProgram";
            tryAccelerated();
        }
#endif
        if(!p.session)LOG_WARN("Offline SR: accelerated initialization unavailable; retrying CPU before output");
    }
#endif
        auto inspect=[&] {
            p.validate(false); p.validate(true);
            if(p.externalModel)p.detectScale();
            else if(scale!=2)throw std::runtime_error("Bundled model must be the verified x2 model");
        };
        if(p.externalModel) {
            p.scale=0;
            if(p.session) {
                try { inspect(); }
                catch(const std::exception& e) {
                    LOG_WARN(std::string("Accelerated external model validation failed; retrying CPU: ")+e.what());
                    p.api->ReleaseSession(p.session);p.session=nullptr;
                    p.profiling=false;
                }
            }
            if(!p.session) {
                p.createSession(model,false);
                inspect();
            }
        } else {
            if(!p.session)p.createSession(model,false);
            p.validate(false);p.validate(true);
        }
    LOG_INFO("Offline SR: ORT " + std::string(entry()->GetVersionString()) + ", " + p.provider + ", tile=256 halo=32, detected scale="+std::to_string(p.scale)+"x, load ms="+std::to_string(milliseconds(loadStart,SRClock::now())));
}
int RealESRGAN::scale() const { return impl_->scale; }
std::string RealESRGAN::device() const { return impl_->provider; }
uint64_t RealESRGAN::reusedTiles() const { return impl_->reusedTileCount; }
void RealESRGAN::enhance(const std::vector<uint8_t>& rgb, int width, int height, std::vector<uint8_t>& result, const std::atomic<bool>& cancelled,
                        const std::vector<uint8_t>& previousRgb) {
    auto& p = *impl_;
    if (!p.session || p.scale<1 || p.scale>16 || width <= 0 || height <= 0 || rgb.size() != size_t(width) * height * 3)
        throw std::runtime_error("Invalid RGB input");
    const int64_t wide=int64_t(width)*p.scale, high=int64_t(height)*p.scale;
    if (wide > 8192 || high > 8192 || wide * high > 7680LL * 4320) throw std::runtime_error("Intermediate size limit exceeded");
    const int ow=static_cast<int>(wide), oh=static_cast<int>(high), side=kTile*p.scale;
    const auto prepareStart=SRClock::now();
    double prepare=0,run=0,stitch=0;
    int tiles=0;
    const size_t outputBytes=size_t(ow)*oh*3;
    // 诊断开关用于同一构建的全量推理对照，不改变模型或像素处理。
    const char* disableReuse=std::getenv("FLUX_SR_DISABLE_TILE_REUSE");
    const bool canReuse=!(disableReuse && *disableReuse) && previousRgb.size()==rgb.size() && result.size()==outputBytes;
    const char* referencePack = std::getenv("FLUX_SR_REFERENCE_PACK");
    const auto pack = referencePack && *referencePack ? packSuperResolutionRgbReference : packSuperResolutionRgb;
    result.resize(outputBytes);
    p.prepareInput(width,height);
    auto& input=p.inputBuffer;
    const char* inputName = p.inputName.c_str(); const char* outputName = p.outputName.c_str();
    const OrtValue* inputs[] = {p.inputTensor};
    prepare+=milliseconds(prepareStart,SRClock::now());
    for (int y = 0; y < height; y += kCore) for (int x = 0; x < width; x += kCore) {
        const auto tileStart=SRClock::now();
        if (cancelled.load()) throw std::runtime_error("Cancelled");
        const auto& xs=p.reflectedX[x/kCore];const auto& ys=p.reflectedY[y/kCore];
        bool identical=canReuse;
        for (int ty = 0; ty < kTile; ++ty) {
            const size_t row=size_t(ys[ty])*width*3;
            for (int tx = 0; tx < kTile; ++tx) {
                const size_t source=row+size_t(xs[tx])*3;
                // 包括 halo 和 Reflect-101 后的全部输入，不能仅比较输出核心区域。
                if(identical && (rgb[source]!=previousRgb[source] || rgb[source+1]!=previousRgb[source+1] || rgb[source+2]!=previousRgb[source+2]))
                    identical=false;
                for (int c = 0; c < 3; ++c) input[c * kTile * kTile + ty * kTile + tx] = p.normalized[rgb[source+c]];
            }
        }
        OrtValue* rawOutput = p.outputTensor;
        const auto runStart=SRClock::now();prepare+=milliseconds(tileStart,runStart);
        if(identical) { ++p.reusedTileCount; ++tiles; continue; }
        OrtStatus* status = p.api->Run(p.session, nullptr, &inputName, inputs, 1, &outputName, 1, &rawOutput);
        const auto stitchStart=SRClock::now();run+=milliseconds(runStart,stitchStart);
        p.check(status);
        // 显式诊断最多采集30次tile Run，避免长片profile无限增长。
        if(p.profiling && ++p.profileRuns>=30)p.endProfile();
        if (cancelled.load()) throw std::runtime_error("Cancelled");
        float* values = nullptr;
        p.check(p.api->GetTensorMutableData(rawOutput, reinterpret_cast<void**>(&values)));
        for (int dy = 0; dy < std::min(kCore,height-y)*p.scale; ++dy) {
            const size_t offset = size_t(dy+kHalo*p.scale)*side+kHalo*p.scale;
            pack(values+offset, values+size_t(side)*side+offset, values+size_t(2)*side*side+offset,
                result.data()+(size_t(y*p.scale+dy)*ow+x*p.scale)*3, size_t(std::min(kCore,width-x))*p.scale);
        }
        stitch+=milliseconds(stitchStart,SRClock::now());++tiles;
    }
    p.report(prepare,run,stitch,tiles);
}
}
