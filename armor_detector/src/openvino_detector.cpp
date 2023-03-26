// Copyright 2023 Yunlong Feng
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "armor_detector/openvino_detector.hpp"

#include <algorithm>

namespace rm_auto_aim
{

static const int INPUT_W = 416;        // Width of input
static const int INPUT_H = 416;        // Height of input
static constexpr int NUM_CLASSES = 8;  // Number of classes
static constexpr int NUM_COLORS = 4;   // Number of color

cv::Mat letterbox(
  const cv::Mat & img, Eigen::Matrix3f & transform_matrix,
  std::vector<int> new_shape = {INPUT_W, INPUT_H})
{
  // Get current image shape [height, width]

  int img_h = img.rows;
  int img_w = img.cols;

  // Compute scale ratio(new / old) and target resized shape
  float scale = std::min(new_shape[1] * 1.0 / img_h, new_shape[0] * 1.0 / img_w);
  int resize_h = static_cast<int>(round(img_h * scale));
  int resize_w = static_cast<int>(round(img_w * scale));

  // Compute padding
  int pad_h = new_shape[1] - resize_h;
  int pad_w = new_shape[0] - resize_w;

  // Resize and pad image while meeting stride-multiple constraints
  cv::Mat resized_img;
  cv::resize(img, resized_img, cv::Size(resize_w, resize_h));

  // divide padding into 2 sides
  float half_h = pad_h * 1.0 / 2;
  float half_w = pad_w * 1.0 / 2;

  // Compute padding boarder
  int top = static_cast<int>(round(half_h - 0.1));
  int bottom = static_cast<int>(round(half_h + 0.1));
  int left = static_cast<int>(round(half_w - 0.1));
  int right = static_cast<int>(round(half_w + 0.1));

  /* clang-format off */
  /* *INDENT-OFF* */

  // Compute point transform_matrix
  transform_matrix << 1.0 / scale, 0, -half_w / scale,
                      0, 1.0 / scale, -half_h / scale,
                      0, 0, 1;

  /* *INDENT-ON* */
  /* clang-format on */

  // Add border
  cv::copyMakeBorder(
    resized_img, resized_img, top, bottom, left, right, 0, cv::Scalar(
      114, 114,
      114));

  return resized_img;
}

static void generate_proposals(
  std::vector<ArmorObject> & output_objs, std::vector<float> & scores,
  std::vector<cv::Rect> & rects, const cv::Mat & output_buffer,
  const Eigen::Matrix<float, 3, 3> & transform_matrix, float conf_threshold)
{
  for (int i = 0; i < output_buffer.rows; ++i) {
    float confidence = output_buffer.at<float>(i, 8);
    if (confidence < conf_threshold) {
      continue;
    }

    double color_score, num_score;
    cv::Point color_id, num_id;
    cv::Mat color_scores = output_buffer.row(i).colRange(9, 9 + NUM_COLORS);
    cv::Mat num_scores =
      output_buffer.row(i).colRange(9 + NUM_COLORS, 9 + NUM_COLORS + NUM_CLASSES);
    // Argmax
    cv::minMaxLoc(color_scores, NULL, &color_score, NULL, &color_id);
    cv::minMaxLoc(num_scores, NULL, &num_score, NULL, &num_id);

    float x_1 = output_buffer.at<float>(i, 0);
    float y_1 = output_buffer.at<float>(i, 1);
    float x_2 = output_buffer.at<float>(i, 2);
    float y_2 = output_buffer.at<float>(i, 3);
    float x_3 = output_buffer.at<float>(i, 4);
    float y_3 = output_buffer.at<float>(i, 5);
    float x_4 = output_buffer.at<float>(i, 6);
    float y_4 = output_buffer.at<float>(i, 7);

    Eigen::Matrix<float, 3, 4> apex_norm;
    Eigen::Matrix<float, 3, 4> apex_dst;

    /* clang-format off */
    /* *INDENT-OFF* */
    apex_norm << x_1, x_2, x_3, x_4,
                y_1, y_2, y_3, y_4,
                1,   1,   1,   1;
    /* *INDENT-ON* */
    /* clang-format on */

    apex_dst = transform_matrix * apex_norm;

    ArmorObject obj;

    obj.pts.reserve(4);

    obj.pts[0] = cv::Point2f(apex_dst(0, 0), apex_dst(1, 0));
    obj.pts[1] = cv::Point2f(apex_dst(0, 1), apex_dst(1, 1));
    obj.pts[2] = cv::Point2f(apex_dst(0, 2), apex_dst(1, 2));
    obj.pts[3] = cv::Point2f(apex_dst(0, 3), apex_dst(1, 3));

    auto rect = cv::boundingRect(obj.pts);

    obj.box = rect;
    obj.color = static_cast<ArmorColor>(color_id.x);
    obj.number = static_cast<ArmorNumber>(num_id.x);
    obj.prob = confidence;

    rects.push_back(rect);
    scores.push_back(confidence);
    output_objs.push_back(std::move(obj));
  }
}

OpenVINODetector::OpenVINODetector(
  const std::filesystem::path & model_path, const std::string & device_name,
  float conf_threshold, int top_k, float nms_threshold, bool auto_init)
: model_path_(model_path)
  , device_name_(device_name)
  , conf_threshold_(conf_threshold)
  , top_k_(top_k)
  , nms_threshold_(nms_threshold)
{
  if (auto_init) {
    init();
  }
}

void OpenVINODetector::init()
{
  if (ov_core_ == nullptr) {
    ov_core_ = std::make_unique<ov::Core>();
  }

  auto model = ov_core_->read_model(model_path_);

  // Set infer type
  ov::preprocess::PrePostProcessor ppp(model);
  // Set input output precision
  ppp.input().tensor().set_element_type(ov::element::f32);
  ppp.output().tensor().set_element_type(ov::element::f32);

  // Compile model
  compiled_model_ = std::make_unique<ov::CompiledModel>(
    ov_core_->compile_model(
      model, device_name_,
      ov::hint::performance_mode(ov::hint::PerformanceMode::LATENCY)));
}

std::future<bool> OpenVINODetector::push_input(const cv::Mat & rgb_img, int64_t timestamp_nanosec)
{
  if (rgb_img.empty()) {
    // return false when img is empty
    return std::async([]() {return false;});
  }

  // Reprocess
  Eigen::Matrix3f transform_matrix;  // transform matrix from resized image to source image.
  cv::Mat resized_img = letterbox(rgb_img, transform_matrix);

  // Start async detect
  return std::async(
    std::launch::async, &OpenVINODetector::process_callback, this, resized_img, transform_matrix,
    timestamp_nanosec, rgb_img);
}

void OpenVINODetector::set_callback(DetectorCallback callback)
{
  infer_callback_ = callback;
}

bool OpenVINODetector::process_callback(
  const cv::Mat resized_img, Eigen::Matrix3f transform_matrix,
  int64_t timestamp_nanosec, const cv::Mat & src_img)
{
  // BGR->RGB, u8(0-255)->f32(0.0-1.0), HWC->NCHW
  // note: TUP's model no need to normalize
  cv::Mat blob =
    cv::dnn::blobFromImage(
    resized_img, 1., cv::Size(INPUT_W, INPUT_H), cv::Scalar(0, 0, 0),
    true);

  // Feed blob into input
  auto input_port = compiled_model_->input();
  ov::Tensor input_tensor(input_port.get_element_type(),
    ov::Shape(std::vector<size_t>{1, 3, INPUT_W, INPUT_H}),
    blob.ptr(0));

  // Start inference
  auto infer_request = compiled_model_->create_infer_request();
  infer_request.set_input_tensor(input_tensor);
  infer_request.infer();

  auto output = infer_request.get_output_tensor();

  // Process output data
  auto output_shape = output.get_shape();
  // 3549 x 21 Matrix
  cv::Mat output_buffer(output_shape[1], output_shape[2], CV_32F, output.data());

  // Parsed variable
  std::vector<ArmorObject> objs_tmp, objs_result;
  std::vector<cv::Rect> rects;
  std::vector<float> scores;
  std::vector<int> indices;

  // Parse YOLO output
  generate_proposals(
    objs_tmp, scores, rects, output_buffer, transform_matrix,
    this->conf_threshold_);

  // NMS & TopK
  cv::dnn::NMSBoxes(
    rects, scores, this->conf_threshold_, this->nms_threshold_, indices, 1.0,
    this->top_k_);
  for (size_t i = 0; i < indices.size(); ++i) {
    objs_result.push_back(std::move(objs_tmp[i]));
  }

  // Call callback function
  if (this->infer_callback_) {
    this->infer_callback_(objs_result, timestamp_nanosec, src_img);
    return true;
  }

  return false;
}

}  // namespace rm_auto_aim
