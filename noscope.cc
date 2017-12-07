#include <sys/mman.h>

#include <chrono>
#include <ctime>
#include <random>
#include <algorithm>
#include <iterator>
#include <memory>

#include "tensorflow/core/public/session.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/graph/default_device.h"
#include "tensorflow/core/util/command_line_flags.h"

#include "tensorflow/noscope-lite/mse.h"
#include "tensorflow/noscope-lite/filters.h"
#include "tensorflow/noscope-lite/MemoryTests.h"
#include "tensorflow/noscope-lite/noscope_labeler.h"
#include "tensorflow/noscope-lite/noscope_data.h"

using tensorflow::Flag;

static bool file_exists(const std::string& name) {
  std::ifstream f(name.c_str());
  return f.good();
}

static tensorflow::Session* InitSession(const std::string& graph_fname) {
  tensorflow::Session *session;
  tensorflow::SessionOptions opts;
  tensorflow::GraphDef graph_def;
  // YOLO needs some memory
  //opts.config.mutable_gpu_options()->set_per_process_gpu_memory_fraction(0.9);
  //opts.config.mutable_gpu_options()->set_allow_growth(true);
  tensorflow::Status status = NewSession(opts, &session);
  TF_CHECK_OK(status);

  status = tensorflow::ReadBinaryProto(
      tensorflow::Env::Default(),
      graph_fname, &graph_def);
  //tensorflow::graph::SetDefaultDevice("/gpu:0", &graph_def);
  TF_CHECK_OK(status);

  status = session->Create(graph_def);
  TF_CHECK_OK(status);

  return session;
}

static noscope::NoscopeData* LoadVideo(const std::string& video, const std::string& dumped_videos,
                                 const int kSkip, const int kNbFrames, const int kStartFrom) {
  auto start = std::chrono::high_resolution_clock::now();
  noscope::NoscopeData *data = NULL;
  if (dumped_videos == "/dev/null") {
    std::cerr << "Loading new video\n";
    data = new noscope::NoscopeData(video, kSkip, kNbFrames, kStartFrom);
  } else {
    if (file_exists(dumped_videos)) {
      std::cerr << "Loading dumped video\n";
      data = new noscope::NoscopeData(dumped_videos);
    } else {
      std::cerr << "Dumping video\n";
      data = new noscope::NoscopeData(video, kSkip, kNbFrames, kStartFrom);
      data->DumpAll(dumped_videos);
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  std::cout << "Loaded video\n";
  std::chrono::duration<double> diff = end - start;
  std::cout << "Time to load (and resize) video: " << diff.count() << " s" << std::endl;
  return data;
}

noscope::filters::DifferenceFilter GetDiffFilter(const bool kUseBlocked,
                                              const bool kSkipDiffDetection) {
  noscope::filters::DifferenceFilter nothing{noscope::filters::DoNothing, "DoNothing"};
  noscope::filters::DifferenceFilter blocked{noscope::filters::BlockedMSE, "BlockedMSE"};
  noscope::filters::DifferenceFilter global{noscope::filters::GlobalMSE, "GlobalMSE"};

  if (kSkipDiffDetection) {
    return nothing;
  }
  if (kUseBlocked) {
    return blocked;
  } else {
    return global;
  }
}

int main(int argc, char* argv[]) {
  std::string small_cnn_graph="/dev/null";
  std::string big_cnn_graph;
  std::string video;
  std::string avg_fname="avg.txt";
  std::string confidence_csv="conf.csv";
  std::string diff_thresh_str="0";
  std::string distill_thresh_lower_str="0", distill_thresh_upper_str="0";
  std::string conf_thresh_str="0";
  std::string skip="1";
  std::string nb_frames="12400";
  std::string start_from="0";
  std::string target_object_id="0";
  std::string skip_small_cnn="0";
  std::string skip_diff_detection="0";
  std::string dumped_videos="/dev/null";
  std::string diff_detection_weights="/dev/null";
  std::string use_blocked="0";
  std::string ref_image="0";
  std::vector<Flag> flag_list = {
      Flag("small_cnn_graph", &small_cnn_graph, "Small CNN Graph to be executed"),
      Flag("video", &video, "Video to load"),
      Flag("big_cnn_graph", &big_cnn_graph, "Big CNN Graph to be executed"),
      Flag("avg_fname", &avg_fname, "Filename with the average (txt)"),
      Flag("confidence_csv", &confidence_csv, "CSV to output confidences to"),
      Flag("diff_thresh", &diff_thresh_str, "Difference filter threshold"),
      Flag("distill_thresh_lower", &distill_thresh_lower_str, "Distill threshold (lower)"),
      Flag("distill_thresh_upper", &distill_thresh_upper_str, "Distill threshold (upper)"),
      Flag("conf_thresh", &conf_thresh_str, "Confidence threshold for large cnn"),
      Flag("skip", &skip, "Number of frames to skip, minimal is 1"),
      Flag("nb_frames", &nb_frames, "Number of frames to read"),
      Flag("start_from", &start_from, "Where to start from"),
      Flag("target_object_id", &target_object_id, "class id in big cnn model (trained in coco db)"),
      Flag("skip_small_cnn", &skip_small_cnn, "0/1 skip small CNN or not"),
      Flag("skip_diff_detection", &skip_diff_detection, "0/1 skip diff detection or not"),
      Flag("dumped_videos", &dumped_videos, ""),
      Flag("diff_detection_weights", &diff_detection_weights, "Difference detection weights(for blocked DD)"),
      Flag("use_blocked", &use_blocked, "0/1 whether or not to use blocked DD"),
      Flag("ref_image", &ref_image, "reference image,input index of frame"),
  };
  std::string usage = tensorflow::Flags::Usage(argv[0], flag_list);
  const bool parse_result = tensorflow::Flags::Parse(&argc, argv, flag_list);
  const float diff_thresh = std::stof(diff_thresh_str);
  const float conf_thresh = std::stof(conf_thresh_str);
  const float distill_thresh_lower = std::stof(distill_thresh_lower_str);
  const float distill_thresh_upper = std::stof(distill_thresh_upper_str);
  const size_t kSkip = std::stoi(skip);
  const size_t kNbFrames = std::stoi(nb_frames);
  const size_t kStartFrom = std::stoi(start_from);
  const int kTargetClass = std::stoi(target_object_id);
  const bool kSkipSmallCNN = std::stoi(skip_small_cnn);
  const bool kSkipDiffDetection = std::stoi(skip_diff_detection);
  const bool kUseBlocked = std::stoi(use_blocked);
  const size_t kRefImage = std::stoi(ref_image);
  if (!parse_result) {
    LOG(ERROR) << usage;
    return -1;
  }

  if (diff_detection_weights != "/dev/null" && !kSkipDiffDetection) {
    noscope::filters::LoadWeights(diff_detection_weights);
  }

  tensorflow::Session *SmallCNN_Session = InitSession(small_cnn_graph);
  tensorflow::Session *LargeCNN_Session = InitSession(big_cnn_graph);
  noscope::NoscopeData *data = LoadVideo(video, dumped_videos, kSkip, kNbFrames, kStartFrom);
  noscope::filters::DifferenceFilter df = GetDiffFilter(kUseBlocked, kSkipDiffDetection);

  noscope::NoscopeLabeler labeler = noscope::NoscopeLabeler(
      SmallCNN_Session,
      LargeCNN_Session,
      df,
      avg_fname,
      *data);

  std::cerr << "Loaded NoscopeLabeler\n";

  auto start = std::chrono::high_resolution_clock::now();
  labeler.RunDifferenceFilter(diff_thresh, 10000000, kUseBlocked, kRefImage);
  auto diff_end = std::chrono::high_resolution_clock::now();
  if (!kSkipSmallCNN) {
    labeler.PopulateCNNFrames();
    labeler.RunSmallCNN(distill_thresh_lower, distill_thresh_upper);
  }
  auto dist_end = std::chrono::high_resolution_clock::now();
  labeler.RunLargeCNN(kTargetClass, conf_thresh);
  auto yolo_end = std::chrono::high_resolution_clock::now();
  std::vector<double> runtimes(4);
  {
    std::chrono::duration<double> diff = yolo_end - start;
    std::cout << "Total time: " << diff.count() << " s" << std::endl;

    diff = diff_end - start;
    runtimes[0] = diff.count();
    diff = dist_end - start;
    runtimes[1] = diff.count();
    diff = yolo_end - start;
    runtimes[2] = diff.count();
    runtimes[3] = diff.count();
  }
  runtimes[2] -= runtimes[1];
  runtimes[1] -= runtimes[0];
  labeler.DumpConfidences(confidence_csv,
                          small_cnn_graph,
                          kSkip,
                          kSkipSmallCNN,
                          diff_thresh,
                          distill_thresh_lower,
                          distill_thresh_upper,
                          runtimes);

  return 0;
}
