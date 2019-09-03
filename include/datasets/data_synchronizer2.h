#pragma once

#include <utility>
#include "asl_readers.h"
#include "data_synchronizers.h"
#include "msckf_mono/types.h"

using namespace asl_dataset;
using namespace msckf_mono;

static imuState<float> dummy_gt_reading;

namespace synchronizer {
struct IMUDataPackS2 {
  timestamp ts;
  imuReading<float> imu;
  imuState<float> gt;
};

struct ImageDataPackS2 {
  timestamp ts;
  cv::Mat image;
  imuState<float> gt;
};

class Synchronizer2 {
  std::shared_ptr<Synchronizer<IMU, Camera, GroundTruth>> sync;
  bool received_first_image = false;
  std::pair<timestamp, cv::Mat> prev_image;
  std::pair<timestamp, cv::Mat> curr_image;
  timestamp prev_image;
  std::vector<std::pair<timestamp, imuReading<float>>, imuState<float>> imu_readings_w_gt;

  void add_imu_measurement(timestamp ts, const imuReading<float> &imu_meas) {
    assert(!(imu_readings.size() == 0 && ts > prev_image.first));
    auto imu_meas_cpy = imu_meas;
    if (ts < prev_image.first) {
      imu_meas_cpy.dT = ts - prev_image.first;
    }
    imu_readings.push_back(std::make_pair(prev_image.first, imu_meas_cpy));
  }

  void add_image(timestamp ts, const cv::Mat &image) {
    received_first_image = true;
    prev_image = curr_image;
    curr_image = std::make_pair<timestamp, cv::Mat>(ts, image);
  }

  void add_ground_truth(const imuState<float> &gt) {

  }

  bool has_next() {}

  imuReading<float> get_imu(imuState<float> &gt_reading = dummy_gt_reading) {}
};
}  // namespace synchronizer