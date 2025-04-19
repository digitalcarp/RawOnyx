// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2025 Daniel Gao

#include "fmt/format.h"
#include "fmt/ranges.h"

#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void printHelp()
{
    fmt::println(R"(Usage: u2net <model_path> <image> [<output>]

    Model Code: https://github.com/xuebinqin/U-2-Net
    )");
}

constexpr int WINDOW_WIDTH = 800;
constexpr int WINDOW_HEIGHT = 600;

template <size_t SIZE>
using Shape = std::array<int64_t, SIZE>;

constexpr int BATCH_SIZE = 1;
constexpr int RGB_NUM_CHANNELS = 3;
constexpr int TENSOR_SIZE = 320;

constexpr Shape<4> INPUT_SHAPE = {BATCH_SIZE, RGB_NUM_CHANNELS, TENSOR_SIZE, TENSOR_SIZE};
constexpr Shape<3> OUTPUT_SHAPE = {BATCH_SIZE, TENSOR_SIZE, TENSOR_SIZE};

bool checkCudaSupport()
{
    std::vector<std::string> available = Ort::GetAvailableProviders();
    for (const auto& p : available) {
        if (p.find("CUDA") == 0) return true;
    }

    return false;
}

void appendCudaProvider(Ort::SessionOptions& session_options)
{
    const OrtApi& api = Ort::GetApi();
    OrtCUDAProviderOptionsV2* cuda_options = nullptr;
    Ort::ThrowOnError(api.CreateCUDAProviderOptions(&cuda_options));

    std::vector<const char*> keys{
        "device_id",
        // "gpu_mem_limit",
        "arena_extend_strategy", "cudnn_conv_algo_search", "do_copy_in_default_stream",
        "cudnn_conv_use_max_workspace"
        // "cudnn_conv1d_pad_to_nc1d"
    };
    std::vector<const char*> values{
        "0",
        // "2147483648",
        "kNextPowerOfTwo", "EXHAUSTIVE", "1", "1"
        // "1"
    };

    Ort::ThrowOnError(api.UpdateCUDAProviderOptions(cuda_options, keys.data(),
                                                    values.data(), keys.size()));
    Ort::ThrowOnError(api.SessionOptionsAppendExecutionProvider_CUDA_V2(session_options,
                                                                        cuda_options));

    api.ReleaseCUDAProviderOptions(cuda_options);
}

struct Resize {
    double x_scale = 1.0;
    double y_scale = 1.0;

    uint32_t img_w = 0;
    uint32_t img_h = 0;
    uint32_t new_w = 0;
    uint32_t new_h = 0;
};

double divd(double x, double y) { return x / y; }

Resize computeResize(int img_w, int img_h, int tensor_size)
{
    double scale_w = divd(tensor_size, img_w);
    double scale_h = divd(tensor_size, img_h);

    // Scale with tensor_size as upper bound
    if (scale_w < scale_h) {
        // Fit width
        scale_h = scale_w;
    } else {
        // Fit height
        scale_w = scale_h;
    }

    Resize resize;
    resize.img_w = img_w;
    resize.img_h = img_h;
    resize.new_w = img_w * scale_w;
    resize.new_h = img_h * scale_h;

    resize.x_scale = scale_w;
    resize.y_scale = scale_h;

    return resize;
}

std::vector<float> generateInputTensorData(const cv::Mat& img, const Resize& resize)
{
    cv::Size new_size{static_cast<int>(resize.new_w), static_cast<int>(resize.new_h)};
    cv::Mat resized_img;
    cv::resize(img, resized_img, new_size, 0, 0, cv::INTER_CUBIC);

    const size_t area = TENSOR_SIZE * TENSOR_SIZE;
    // Zero initialize data needed in case image needs padding with 0
    std::vector<float> input(area * RGB_NUM_CHANNELS, 0);

    const uchar* mat_data = static_cast<uchar*>(resized_img.data);
    const int channels = resized_img.channels();

    // Numbers from ImageNet mean and std
    // https://github.com/xuebinqin/U-2-Net/blob/master/data_loader.py#L207
    constexpr double R_MEAN = 0.485;
    constexpr double G_MEAN = 0.456;
    constexpr double B_MEAN = 0.406;
    constexpr double R_STD = 0.229;
    constexpr double G_STD = 0.224;
    constexpr double B_STD = 0.225;

    // https://github.com/xuebinqin/U-2-Net/blob/master/data_loader.py#L218
    constexpr uint32_t R_TRANSPOSE = 1;
    constexpr uint32_t G_TRANSPOSE = 2;
    constexpr uint32_t B_TRANSPOSE = 0;

    double min, max;
    cv::minMaxLoc(resized_img, &min, &max);

    auto convert = [&](int value, double mean, double std) -> float
    {
        double x = (divd(value, max) - mean) / std;
        return std::clamp<float>(x, 0, 1);
    };

    for (uint32_t row = 0; row < resize.new_h; row++) {
        for (uint32_t col = 0; col < resize.new_w; col++) {
            // OpenCV image channel order is 8-bit BGR
            uchar b = mat_data[row * resized_img.step + channels * col + 0];
            uchar g = mat_data[row * resized_img.step + channels * col + 1];
            uchar r = mat_data[row * resized_img.step + channels * col + 2];

            auto offset = [&](uint32_t transpose)
            { return transpose * area + row * TENSOR_SIZE + col; };

            input[offset(R_TRANSPOSE)] = convert(r, R_MEAN, R_STD);
            input[offset(G_TRANSPOSE)] = convert(g, G_MEAN, G_STD);
            input[offset(B_TRANSPOSE)] = convert(b, B_MEAN, B_STD);
        }
    }

    return input;
}

cv::Mat readOutputTensorData(const std::vector<float>& output, const Resize& resize)
{
    float min = *std::min_element(output.begin(), output.end());
    float max = *std::max_element(output.begin(), output.end());
    float delta = max - min;

    cv::Mat mask(resize.new_h, resize.new_w, CV_8UC1);
    uchar* data = static_cast<uchar*>(mask.data);

    for (uint32_t row = 0; row < resize.new_h; row++) {
        for (uint32_t col = 0; col < resize.new_w; col++) {
            float value = output[row * TENSOR_SIZE + col];
            float normalized = (value - min) / delta * 255;
            data[row * mask.step + col] = normalized;
        }
    }

    cv::Mat result;
    cv::Size new_size{static_cast<int>(resize.img_w), static_cast<int>(resize.img_h)};
    cv::resize(mask, result, new_size, 0, 0, cv::INTER_LINEAR);

    return result;
}

class Demo {
public:
    void init(std::string_view model_path);
    cv::Mat run(const cv::Mat& img);

private:
    Ort::Env m_env{nullptr};
    Ort::Session m_session{nullptr};
    Ort::MemoryInfo m_mem_info{nullptr};
};

} // namespace

void Demo::init(std::string_view model_path)
{
    m_env = Ort::Env(OrtLoggingLevel::ORT_LOGGING_LEVEL_WARNING, "demo");

    Ort::SessionOptions session_options;
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    const bool use_cuda = checkCudaSupport();
    if (use_cuda) {
        appendCudaProvider(session_options);
    }

    m_session = Ort::Session(m_env, model_path.data(), session_options);

    if (use_cuda) {
        constexpr int device_id = 0; // Might not work if you have multiple GPUs
        m_mem_info =
                Ort::MemoryInfo("Cuda", OrtArenaAllocator, device_id, OrtMemTypeDefault);
    } else {
        m_mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    }
}

cv::Mat Demo::run(const cv::Mat& img)
{
    Resize resize = computeResize(img.size().width, img.size().height, TENSOR_SIZE);

    fmt::println("Resizing input image from {}x{} to {}x{} (scale = [{:.5}, {:.5}])",
                 resize.img_w, resize.img_h, resize.new_w, resize.new_h, resize.x_scale,
                 resize.y_scale);

    // Vectors initialize to zero which is unnecessary for our use cases.
    // Consider using a structure with different construct semantics.
    std::vector<float> input = generateInputTensorData(img, resize);

    Ort::Value input_tensor =
            Ort::Value::CreateTensor(m_mem_info, input.data(), input.size(),
                                     INPUT_SHAPE.data(), INPUT_SHAPE.size());

    std::vector<float> output(TENSOR_SIZE * TENSOR_SIZE);
    Ort::Value output_tensor =
            Ort::Value::CreateTensor(m_mem_info, output.data(), output.size(),
                                     OUTPUT_SHAPE.data(), OUTPUT_SHAPE.size());

    constexpr int64_t NUM_INPUTS = 1;
    constexpr std::array<const char*, NUM_INPUTS> INPUT_NAMES{"input"};
    constexpr int64_t NUM_OUTPUTS = 1;
    constexpr std::array<const char*, NUM_OUTPUTS> OUTPUT_NAMES{"output"};

    Ort::RunOptions run_options;
    m_session.Run(run_options, INPUT_NAMES.data(), &input_tensor, NUM_INPUTS,
                  OUTPUT_NAMES.data(), &output_tensor, NUM_OUTPUTS);

    return readOutputTensorData(output, resize);
}

int main(int argc, char* argv[])
{
    if (argc != 3 && argc != 4) {
        printHelp();
        return -1;
    }

    if (std::string_view(argv[1]) == "--help") {
        printHelp();
        return 0;
    }

    const std::string_view model_path = argv[1];
    const std::string image_path = argv[2];

    const std::string out_img_path = (argc == 4) ? argv[3] : "";
    if (!out_img_path.empty()) {
        fmt::println("Mask will be written to: {}", out_img_path);
    }

    const cv::Mat img = cv::imread(image_path, cv::IMREAD_COLOR_BGR);
    cv::Mat mask;

    // Have the Ort::Session lifetime outlive the first waitKey(0) so it is
    // easier to check memory usage of the model.
    Demo demo;

    try {
        fmt::println("Setting up model...");
        demo.init(model_path);
        fmt::println("Done!");
        fmt::println("Running inference...");
        mask = demo.run(img);
        fmt::println("Done!");
    } catch (const std::exception& e) {
        fmt::println("{}", e.what());
        return -1;
    }

    if (!out_img_path.empty()) {
        fmt::println("Writing mask: {}", out_img_path);
        cv::imwrite(out_img_path, mask);
    }

    cv::Mat colored_mask = cv::Mat::zeros(img.size(), img.type());
    colored_mask.setTo(cv::Scalar(0, 255, 255), mask);

    constexpr double alpha = 0.5;
    constexpr double beta = 1.0 - alpha;
    cv::Mat display;
    cv::addWeighted(img, alpha, colored_mask, beta, /*gamma*/ 0, display);

    const std::string WINDOW = "U2-Net Demo";
    cv::namedWindow(WINDOW, cv::WINDOW_NORMAL);
    cv::resizeWindow(WINDOW, WINDOW_WIDTH, WINDOW_HEIGHT);

    cv::imshow(WINDOW, display);

    cv::waitKey(0);

    cv::destroyAllWindows();
    return 0;
}
