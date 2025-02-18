#include <iostream>
#include <string>

#include <ros/ros.h>

#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/PoseArray.h>
#include <nav_msgs/Path.h>

#include <sensor_msgs/PointCloud2.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <sophus/geometry.hpp>

#include <msckf_mono/corner_detector.h>
#include <msckf_mono/StageTiming.h>

#include <datasets/data_synchronizers.h>
#include <datasets/asl_readers.h>

#include <msckf_mono/msckf.h>

#include <msckf_mono/CamStates.h>

using namespace asl_dataset;
using namespace synchronizer;

void initial_imu_est_asl(const double &calib_start, const double &calib_end, std::shared_ptr<IMU> &imu0,
std::shared_ptr<Synchronizer<IMU, Camera, GroundTruth>> &sync, msckf_mono::imuState<float> &firstImuState){
 // start from standstill
  while(imu0->get_time()<calib_start && sync->has_next()){
    sync->next();
  }

  Eigen::Vector3f accel_accum;
  Eigen::Vector3f gyro_accum;
  int num_readings = 0;

  accel_accum.setZero();
  gyro_accum.setZero();

  while(imu0->get_time()<calib_end && sync->has_next()){
    auto data_pack = sync->get_data();
    auto imu_reading = std::get<0>(data_pack);

    if(imu_reading){
      msckf_mono::imuReading<float> imu_data = imu_reading.get();
      accel_accum += imu_data.a;
      gyro_accum += imu_data.omega;
      num_readings++;
    }

    sync->next();
  }

  Eigen::Vector3f accel_mean = accel_accum / num_readings;
  Eigen::Vector3f gyro_mean = gyro_accum / num_readings;

  firstImuState.b_g = gyro_mean;
  firstImuState.g << 0.0, 0.0, -9.81;
  firstImuState.q_IG = Eigen::Quaternionf::FromTwoVectors(-firstImuState.g, accel_mean);

  firstImuState.b_a = firstImuState.q_IG*firstImuState.g + accel_mean;

  firstImuState.p_I_G.setZero();
  firstImuState.v_I_G.setZero();
}

void add_block_init_tumvio(Eigen::MatrixXf &H, Eigen::VectorXf &z, 
 const std::vector<std::tuple<timestamp, msckf_mono::imuReading<float>, msckf_mono::Isometry3<float>>> &data_frame) {
  Eigen::Vector3f alpha;
  Eigen::Vector3f beta;
  alpha << 0, 0, 0;
  beta << 0, 0, 0;
  Eigen::Matrix3f R_bk_w = std::get<2>(data_frame[0]).linear().transpose();
  for (int i=0; i < data_frame.size()-1; i++) {
    Eigen::Matrix3f R_w_bt = std::get<2>(data_frame[i]).linear();
    Eigen::Matrix3f R_bk_bt = R_bk_w * R_w_bt;
    double dt = (std::get<0>(data_frame[i+1]) - std::get<0>(data_frame[i])) / 1.0e9;
    auto imu = std::get<1>(data_frame[i]);
    alpha = alpha + beta * dt + 0.5 * R_bk_bt * imu.a * dt * dt;
    beta = beta + R_bk_bt * imu.a * dt;
  }

  double Dt = (std::get<0>(data_frame[data_frame.size() - 1]) - std::get<0>(data_frame[0])) / 1.0e9;
  if (H.rows() == 0) {
    H.resize(6, 3);;
  } else {
    H.conservativeResize(H.rows() + 6,  Eigen::NoChange);
  }
  H.block<3, 3>(H.rows() - 6, 0) = 0.5 * R_bk_w * Dt * Dt;
  H.block<3, 3>(H.rows() - 3, 0) = R_bk_w * Dt;

  // std::cout << "R_bk_w" << std::endl << R_bk_w << std::endl << std::endl;
  // std::cout << "0.5 * R_bk_w * Dt * Dt" << std::endl << 0.5 * R_bk_w * Dt * Dt << std::endl << std::endl;
  // std::cout << "R_bk_w * Dt" << std::endl << R_bk_w * Dt << std::endl << std::endl;
  // std::cout << "H" << std::endl << H << std::endl << std::endl;

  z.conservativeResize(z.rows() + 6, Eigen::NoChange);
  z.segment(z.rows() - 6, 3) = alpha;
  z.segment(z.rows() - 3, 3) = beta;

  // std::cout << "z" << std::endl << z << std::endl << std::endl;
  // std::cout << "alpha" << std::endl << alpha << std::endl << std::endl;
  // std::cout << "beta" << std::endl << beta << std::endl << std::endl;
}

// Use ground truth values to initialize MSCKF
void initial_imu_est_tumvio(const double &calib_start, const double &calib_end, 
  std::shared_ptr<IMU> &imu0, std::shared_ptr<Camera> &cam0,
  std::shared_ptr<Synchronizer<IMU, Camera, GroundTruth>> &sync, msckf_mono::imuState<float> &firstImuState, 
  std::vector<std::pair<asl_dataset::timestamp, msckf_mono::imuReading<float>>> &residual_imu_reading){

  timestamp prev_gt_ts;
  msckf_mono::imuReading<float> first_imu;
  msckf_mono::Isometry3<float> prev_gt;
  // start from standstill
  while(sync->get_time()<calib_start && sync->has_next()){
    auto data_pack = sync->get_data();
    auto imu_reading = std::get<0>(data_pack);
    auto gt_reading = std::get<2>(data_pack);
    if (gt_reading) {
      prev_gt_ts = sync->get_time();
      prev_gt = gt_reading.get();
    }
    if (imu_reading) {
      first_imu = imu_reading.get();
    }
    // std::cout << sync->get_time() << std::endl;
    sync->next();
  }
  assert(std::get<1>(sync->get_data())); // first one must be camera
  
  // ts, imu, gt
  std::vector<std::tuple<timestamp, msckf_mono::imuReading<float>, msckf_mono::Isometry3<float>>> data_frame;
  std::vector<msckf_mono::Vector3<float>> knot_buffer;
  bool new_cam_frame_received = false;
  unsigned int new_cam_frame_idx;
  Eigen::MatrixXf H;
  Eigen::VectorXf z;

  data_frame.push_back(std::make_tuple(sync->get_time(), first_imu, msckf_mono::Isometry3<float>::Identity()));
  sync->next();

  while(true){
    auto data_pack = sync->get_data();
    auto imu_reading = std::get<0>(data_pack);
    auto cam_reading = std::get<1>(data_pack);
    auto gt_reading = std::get<2>(data_pack);

    if (imu_reading) {
      data_frame.push_back(std::make_tuple(imu0->get_time(), imu_reading.get(), msckf_mono::Isometry3<float>::Identity()));
    }

    if (gt_reading) {
      for (auto &data : data_frame) {
        timestamp ts = std::get<0>(data);
        // second clause might not be necessary because measurements are always the latest
        if (prev_gt_ts <= ts && ts <=sync->get_time()) { 
          const auto &Tg0 = prev_gt;
          const auto &Tg1 = gt_reading.get();
          float interp = static_cast<float>(ts - prev_gt_ts) / static_cast<float>(sync->get_time() - prev_gt_ts);
          auto T10 = Tg1 * Tg0.inverse();
          auto T10_interp = Sophus::SE3<float>::exp(interp * Sophus::SE3<float>(T10.matrix()).log()).matrix();
          auto T_interp = msckf_mono::Isometry3<float>(T10_interp) * Tg0;
          std::get<2>(data) = T_interp;
        }
      }
      prev_gt_ts = sync->get_time();
      prev_gt = gt_reading.get();
    }

    if (cam_reading) {
      // just a sanity check
      for (const auto &data:data_frame) {
        assert(std::get<0>(data) <= sync->get_time());
      }
      data_frame.insert(data_frame.end(), std::make_tuple(sync->get_time(), std::get<1>(data_frame.back()), msckf_mono::Isometry3<float>::Identity()));
      new_cam_frame_received = true;
      new_cam_frame_idx = data_frame.size() - 1;
    }

    if (new_cam_frame_received && !std::get<2>(data_frame[new_cam_frame_idx]).matrix().isIdentity()) {
      // sanity check that all of the ground truth has been initialized
      for (unsigned int i = 0; i <= new_cam_frame_idx; i++) {
        assert(!std::get<2>(data_frame[new_cam_frame_idx]).matrix().isIdentity());
      }

      // add the blocks
      add_block_init_tumvio(H, z, data_frame);

      data_frame.erase(data_frame.begin(), data_frame.begin() + new_cam_frame_idx);
      new_cam_frame_received = false;
      new_cam_frame_idx = 0;

      knot_buffer.push_back(std::get<2>(data_frame[0]).translation());

      if (sync->get_time()>=calib_end) {
        break;
      }
    }

    sync->next();
  }

  // finally estimate the gravity vector
  auto b = H.transpose() * z;
  auto A = H.transpose() * H;
  auto est = A.inverse() * b;
  msckf_mono::Vector3<float> g_est = est.segment(0, 3);

  auto R_bk_w = std::get<2>(data_frame[0]).linear().transpose();

  std::cout << "R_bk_w * g_est"  << std::endl << R_bk_w * g_est << std::endl;

  // use 6th order finite difference to calculate the velocity
  msckf_mono::Vector3<float> v_est;
  v_est << 0, 0, 0;
  std::vector<double> coeff = {-49./20., 6., -15./2., 20./3., -15./4., 6./5., -1./6.};
  for (unsigned int i = 0; i < coeff.size(); i++) {
    v_est += -coeff[i] * knot_buffer[knot_buffer.size() - 1 - i];
    // std::cout << "knot" << std::endl << knot_buffer[knot_buffer.size() - 1 - i] << std::endl << std::endl;
  }
  v_est = v_est / cam0->get_dT();

  // save the remaining IMU readings for use in state estimation later on
  for (unsigned int i = 0; i < data_frame.size(); i++) {
    residual_imu_reading.push_back(std::make_pair(std::get<0>(data_frame[i]), std::get<1>(data_frame[i])));
  }

  std::cout << "R_bk_w * v_est"  << std::endl << R_bk_w * v_est << std::endl;

  g_est = (9.81 * g_est.norm()) * g_est;

  firstImuState.b_g.setZero();
  firstImuState.g << 0.0, 0.0, -9.81;
  firstImuState.q_IG = Eigen::Quaternionf::FromTwoVectors(-firstImuState.g, R_bk_w * g_est);

  // firstImuState.b_a = firstImuState.q_IG*firstImuState.g + R_bk_w * g_est;
  firstImuState.b_a.setZero();

  firstImuState.p_I_G.setZero();
  firstImuState.v_I_G = firstImuState.q_IG.toRotationMatrix() * R_bk_w * v_est;

  std::cout << "firstImuState.g"  << std::endl << firstImuState.g << std::endl;
  std::cout << "firstImuState.v_I_G"  << std::endl << firstImuState.v_I_G << std::endl;
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "image_listener");
  ros::NodeHandle nh;

  std::string data_set;
  std::string gt_rel_path;
  double calib_end, calib_start;
  if(!nh.getParam("data_set_path", data_set)){
    std::cerr << "Must define a data_set_path" << std::endl;
    return 0;
  }
  if(!nh.getParam("stand_still_start", calib_start)){
    std::cerr << "Must define when the system starts a standstill" << std::endl;
    return 0;
  }
  if(!nh.getParam("stand_still_end", calib_end)){
    std::cerr << "Must define when the system stops a standstill" << std::endl;
    return 0;
  }
  if(!nh.getParam("gt_rel_path", gt_rel_path)){
    std::cerr << "Must define gt path relative to the data_set_path" << std::endl;
    return 0;
  }

  ROS_INFO_STREAM("Accessing dataset at " << data_set);

  bool init_states_with_gt;
  nh.param<bool>("init_states_with_gt", init_states_with_gt, false);

  std::shared_ptr<IMU> imu0;
  std::shared_ptr<Camera> cam0;
  std::shared_ptr<GroundTruth> gt0;
  std::shared_ptr<Synchronizer<IMU, Camera, GroundTruth>> sync;

  imu0.reset(new IMU("imu0", data_set+"/imu0"));
  cam0.reset(new Camera("cam0", data_set+"/cam0"));
  gt0.reset(new GroundTruth("gt0", data_set+"/" + gt_rel_path));
  sync.reset(new Synchronizer<IMU, Camera, GroundTruth>(imu0, cam0, gt0));

  msckf_mono::MSCKF<float> msckf;

  msckf_mono::Camera<float> camera;
  auto K = cam0->get_K();
  camera.f_u = K.at<float>(0,0);
  camera.f_v = K.at<float>(1,1);
  camera.c_u = K.at<float>(0,2);
  camera.c_v = K.at<float>(1,2);

  camera.q_CI = cam0->get_q_BS();
  camera.p_C_I = cam0->get_p_BS();

  float feature_cov;
  nh.param<float>("feature_covariance", feature_cov, 7);

  msckf_mono::noiseParams<float> noise_params;
  noise_params.u_var_prime = pow(feature_cov/camera.f_u,2);
  noise_params.v_var_prime = pow(feature_cov/camera.f_v,2);

  Eigen::Matrix<float,12,1> Q_imu_vars;
  float w_var, dbg_var, a_var, dba_var;
  nh.param<float>("imu_vars/w_var", w_var, 1e-5);
  nh.param<float>("imu_vars/dbg_var", dbg_var, 3.6733e-5);
  nh.param<float>("imu_vars/a_var", a_var, 1e-3);
  nh.param<float>("imu_vars/dba_var", dba_var, 7e-4);
  Q_imu_vars << w_var, 	w_var, 	w_var,
                dbg_var,dbg_var,dbg_var,
                a_var,	a_var,	a_var,
                dba_var,dba_var,dba_var;
  noise_params.Q_imu = Q_imu_vars.asDiagonal();

  Eigen::Matrix<float,15,1> IMUCovar_vars;
  float q_var_init, bg_var_init, v_var_init, ba_var_init, p_var_init;
  nh.param<float>("imu_covars/q_var_init", q_var_init, 1e-5);
  nh.param<float>("imu_covars/bg_var_init", bg_var_init, 1e-2);
  nh.param<float>("imu_covars/v_var_init", v_var_init, 1e-2);
  nh.param<float>("imu_covars/ba_var_init", ba_var_init, 1e-2);
  nh.param<float>("imu_covars/p_var_init", p_var_init, 1e-12);
  IMUCovar_vars << q_var_init, q_var_init, q_var_init,
                   bg_var_init,bg_var_init,bg_var_init,
                   v_var_init, v_var_init, v_var_init,
                   ba_var_init,ba_var_init,ba_var_init,
                   p_var_init, p_var_init, p_var_init;
  noise_params.initial_imu_covar = IMUCovar_vars.asDiagonal();

  msckf_mono::MSCKFParams<float> msckf_params;
  nh.param<float>("max_gn_cost_norm", msckf_params.max_gn_cost_norm, 11);
  msckf_params.max_gn_cost_norm = pow(msckf_params.max_gn_cost_norm/camera.f_u, 2);
  nh.param<float>("translation_threshold", msckf_params.translation_threshold, 0.05);
  nh.param<float>("min_rcond", msckf_params.min_rcond, 3e-12);

  nh.param<float>("keyframe_transl_dist", msckf_params.redundancy_angle_thresh, 0.005);
  nh.param<float>("keyframe_rot_dist", msckf_params.redundancy_distance_thresh, 0.05);

  int max_tl, min_tl, max_cs;
  nh.param<int>("max_track_length", max_tl, 1000);	// set to inf to wait for features to go out of view
  nh.param<int>("min_track_length", min_tl, 3);		// set to infinity to dead-reckon only
  nh.param<int>("max_cam_states",   max_cs, 20);


  msckf_params.max_track_length = max_tl;
  msckf_params.min_track_length = min_tl;
  msckf_params.max_cam_states = max_cs;

  std::cout << "cam0->get_K()" << std::endl << cam0->get_K() << std::endl << std::endl;
  std::cout << "cam0->get_dist_coeffs()" << std::endl << cam0->get_dist_coeffs() << std::endl << std::endl;
  std::cout << "distortion_model" << std::endl << cam0->get_dist_model() << std::endl << std::endl;

  std::string distortion_model = cam0->get_dist_model() == "equidistant" ? "equidistant" : "radtan";
  corner_detector::TrackHandler th(cam0->get_K(), cam0->get_dist_coeffs(), distortion_model);

  float ransac_threshold;
  nh.param<float>("ransac_threshold", ransac_threshold, 0.000002);
  th.set_ransac_threshold(ransac_threshold);

  int n_grid_rows, n_grid_cols;
  nh.param<int>("n_grid_rows", n_grid_rows, 8);
  nh.param<int>("n_grid_cols", n_grid_cols, 8);
  th.set_grid_size(n_grid_rows, n_grid_cols);

  int state_k = 0;
  msckf_mono::imuState<float> firstImuState;
  std::vector<std::pair<asl_dataset::timestamp, msckf_mono::imuReading<float>>> imu_reading_buffer;
  if (init_states_with_gt) {
    initial_imu_est_tumvio(calib_start, calib_end, imu0, cam0, sync, firstImuState, imu_reading_buffer);
  } else {
    initial_imu_est_asl(calib_start, calib_end, imu0, sync, firstImuState);
  }

  msckf.initialize(camera, noise_params, msckf_params, firstImuState);
  msckf_mono::imuState<float> imu_state = msckf.getImuState();
  msckf_mono::imuReading<float> imu_data = imu0->get_data();
  auto q = imu_state.q_IG;

  ROS_INFO_STREAM("\nInitial IMU State" <<
    "\n--p_I_G " << imu_state.p_I_G.transpose() <<
    "\n--q_IG " << q.w() << "," << q.x() << "," << q.y() << "," << q.x() <<
    "\n--v_I_G " << imu_state.v_I_G.transpose() <<
    "\n--b_a " << imu_state.b_a.transpose() <<
    "\n--b_g " << imu_state.b_g.transpose() <<
    "\n--a " << imu_data.a.transpose()<<
    "\n--g " << imu_state.g.transpose()<<
    "\n--world_adjusted_a " << (q.toRotationMatrix().transpose()*(imu_data.a-imu_state.b_a)).transpose());

  ros::Publisher odom_pub = nh.advertise<nav_msgs::Odometry>("odom", 100);
  ros::Publisher map_pub = nh.advertise<sensor_msgs::PointCloud2>("map", 100);
  ros::Publisher cam_pose_pub = nh.advertise<geometry_msgs::PoseArray>("cam_state_poses", 100);
  ros::Publisher pruned_cam_states_track_pub = nh.advertise<nav_msgs::Path>("pruned_cam_states_path", 100);
  ros::Publisher cam_state_pub = nh.advertise<msckf_mono::CamStates>("cam_states", 100);

  ros::Publisher imu_track_pub = nh.advertise<nav_msgs::Path>("imu_path", 100);
  nav_msgs::Path imu_path;

  ros::Publisher gt_track_pub = nh.advertise<nav_msgs::Path>("ground_truth_path", 100);
  nav_msgs::Path gt_path;

  ros::Publisher time_state_pub = nh.advertise<msckf_mono::StageTiming>("stage_timing",10);

  image_transport::ImageTransport it(nh);
  image_transport::Publisher raw_img_pub = it.advertise("image", 1);
  image_transport::Publisher track_img_pub = it.advertise("image_track", 1);

  ros::Rate r_imu(1.0/imu0->get_dT());
  ros::Rate r_cam(1.0/cam0->get_dT());

  ros::Time start_clock_time = ros::Time::now();
  ros::Time start_dataset_time;
  start_dataset_time.fromNSec(imu0->get_time());

  while(sync->has_next() && ros::ok()){
    msckf_mono::StageTiming timing_data;
#define TSTART(X) ros::Time start_##X = ros::Time::now();
#define TEND(X) ros::Time end_##X = ros::Time::now();
#define TRECORD(X) {float T = (end_##X-start_##X).toSec();\
                                timing_data.times.push_back(T);\
                                timing_data.stages.push_back(#X);}

    TSTART(get_data);
    auto data_pack = sync->get_data();
    TEND(get_data);
    TRECORD(get_data);

    auto imu_reading = std::get<0>(data_pack);

    if(std::get<1>(data_pack)){
      ros::Time cur_clock_time = ros::Time::now();
      ros::Time cur_dataset_time;
      cur_dataset_time.fromNSec(sync->get_time());

      float elapsed_dataset_time = (cur_dataset_time - start_dataset_time).toSec();
      float elapsed_clock_time = (cur_clock_time - start_clock_time).toSec();

      // hack assumes there will always be 2 or more readings between frames
      if (imu_reading_buffer.size() >= 2) {
        // the frame is ready, process the IMU measurements
        std::vector<msckf_mono::imuReading<float>> imu_readings_w_dt;
        for (unsigned int i = 0; i < imu_reading_buffer.size() - 1; i++) {
          msckf_mono::imuReading<float> r = imu_reading_buffer[i].second;
          r.dT = (imu_reading_buffer[i+1].first - imu_reading_buffer[i].first) / 1e9;
          imu_readings_w_dt.push_back(r);
        }

        imu_readings_w_dt.push_back(imu_reading_buffer.back().second);
        imu_readings_w_dt.back().dT = (sync->get_time() - imu_reading_buffer[imu_reading_buffer.size() - 2].first) / 1e9;
        imu_reading_buffer.erase(imu_reading_buffer.begin(), imu_reading_buffer.end()- 1);
        assert(imu_reading_buffer.size() == 1);
        imu_reading_buffer[0].first = sync->get_time();

        for (auto &r : imu_readings_w_dt) {
          msckf.propagate(r);
        }
      }

      TSTART(feature_tracking_and_warping);
      cv::Mat img = std::get<1>(data_pack).get();

      th.set_current_image(img, ((float)cam0->get_time())/1e9);

      std::vector<msckf_mono::Vector2<float>, Eigen::aligned_allocator<msckf_mono::Vector2<float>>> cur_features;
      corner_detector::IdVector cur_ids;
      th.tracked_features(cur_features, cur_ids);

      std::vector<msckf_mono::Vector2<float>, Eigen::aligned_allocator<msckf_mono::Vector2<float>>> new_features;
      corner_detector::IdVector new_ids;
      th.new_features(new_features, new_ids);

      if(false && elapsed_clock_time > elapsed_dataset_time){ // skipping frames
        ROS_ERROR("skipping frame");
      }else{
        TEND(feature_tracking_and_warping);
        TRECORD(feature_tracking_and_warping);

        TSTART(msckf_augment_state);
        msckf.augmentState(state_k, ((float)sync->get_time())/1e9);
        TEND(msckf_augment_state);
        TRECORD(msckf_augment_state);

        TSTART(msckf_update);
        msckf.update(cur_features, cur_ids);
        TEND(msckf_update);
        TRECORD(msckf_update);

        TSTART(msckf_add_features);
        msckf.addFeatures(new_features, new_ids);
        TEND(msckf_add_features);
        TRECORD(msckf_add_features);

        TSTART(msckf_marginalize);
        msckf.marginalize();
        TEND(msckf_marginalize);
        TRECORD(msckf_marginalize);

        TSTART(msckf_prune_redundant);
        msckf.pruneRedundantStates();
        TEND(msckf_prune_redundant);
        TRECORD(msckf_prune_redundant);

        TSTART(msckf_prune_empty_states);
        msckf.pruneEmptyStates();
        TEND(msckf_prune_empty_states);
        TRECORD(msckf_prune_empty_states);

        auto imu_state = msckf.getImuState();
        auto q = imu_state.q_IG;

        TSTART(publishing);
        ros::Time cur_ros_time;
        cur_ros_time.fromNSec(cam0->get_time());
        {
          nav_msgs::Odometry odom;
          odom.header.stamp = cur_ros_time;
          odom.header.frame_id = "map";
          odom.pose.pose.position.x = imu_state.p_I_G[0];
          odom.pose.pose.position.y = imu_state.p_I_G[1];
          odom.pose.pose.position.z = imu_state.p_I_G[2];
          msckf_mono::Quaternion<float> q_out = imu_state.q_IG.inverse();
          odom.pose.pose.orientation.w = q_out.w();
          odom.pose.pose.orientation.x = q_out.x();
          odom.pose.pose.orientation.y = q_out.y();
          odom.pose.pose.orientation.z = q_out.z();
          odom_pub.publish(odom);
        }

        if(raw_img_pub.getNumSubscribers()>0){
          cv_bridge::CvImage out_img;
          out_img.header.frame_id = "cam0"; // Same timestamp and tf frame as input image
          out_img.header.stamp = cur_ros_time;
          out_img.encoding = sensor_msgs::image_encodings::TYPE_8UC1; // Or whatever
          out_img.image    = img; // Your cv::Mat
          raw_img_pub.publish(out_img.toImageMsg());
        }

        if(track_img_pub.getNumSubscribers()>0){
          cv_bridge::CvImage out_img;
          out_img.header.frame_id = "cam0"; // Same timestamp and tf frame as input image
          out_img.header.stamp = cur_ros_time;
          out_img.encoding = sensor_msgs::image_encodings::TYPE_8UC3; // Or whatever
          out_img.image = th.get_track_image(); // Your cv::Mat
          track_img_pub.publish(out_img.toImageMsg());
        }

        if(map_pub.getNumSubscribers()>0){
          std::vector<Eigen::Vector3f, Eigen::aligned_allocator<Eigen::Vector3f>> map =
            msckf.getMap();
          pcl::PointCloud<pcl::PointXYZ>::Ptr pointcloud(new pcl::PointCloud<pcl::PointXYZ>());
          pointcloud->header.frame_id = "map";
          pointcloud->height = 1;
          for (auto& point:map)
          {
            pointcloud->points.push_back(pcl::PointXYZ(point(0),
                  point(1),
                  point(2)));
          }

          pointcloud->width = pointcloud->points.size();
          map_pub.publish(pointcloud);
        }

        if(cam_pose_pub.getNumSubscribers()>0){
          geometry_msgs::PoseArray cam_poses;

          auto msckf_cam_poses = msckf.getCamStates();
          for( auto& cs : msckf_cam_poses ){
            geometry_msgs::Pose p;
            p.position.x = cs.p_C_G[0];
            p.position.y = cs.p_C_G[1];
            p.position.z = cs.p_C_G[2];
            msckf_mono::Quaternion<float> q_out = cs.q_CG.inverse();
            p.orientation.w = q_out.w();
            p.orientation.x = q_out.x();
            p.orientation.y = q_out.y();
            p.orientation.z = q_out.z();
            cam_poses.poses.push_back(p);
          }

          cam_poses.header.frame_id = "map";
          cam_poses.header.stamp = cur_ros_time;

          cam_pose_pub.publish(cam_poses);
        }

        if(cam_state_pub.getNumSubscribers()>0){
          msckf_mono::CamStates cam_states;

          auto msckf_cam_states = msckf.getCamStates();
          for( auto& cs : msckf_cam_states ){
            msckf_mono::CamState ros_cs;

            ros_cs.stamp.fromSec(cs.time);
            ros_cs.id = cs.state_id;

            ros_cs.number_tracked_features = cs.tracked_feature_ids.size();

            auto& p = ros_cs.pose;
            p.position.x = cs.p_C_G[0];
            p.position.y = cs.p_C_G[1];
            p.position.z = cs.p_C_G[2];
            msckf_mono::Quaternion<float> q_out = cs.q_CG.inverse();
            p.orientation.w = q_out.w();
            p.orientation.x = q_out.x();
            p.orientation.y = q_out.y();
            p.orientation.z = q_out.z();

            cam_states.cam_states.push_back(ros_cs);
          }

          cam_state_pub.publish(cam_states);
        }

        if(pruned_cam_states_track_pub.getNumSubscribers()>0){
          nav_msgs::Path pruned_path;
          pruned_path.header.stamp = cur_ros_time;
          pruned_path.header.frame_id = "map";
          for(auto ci : msckf.getPrunedStates()){
            geometry_msgs::PoseStamped ps;

            ps.header.stamp.fromNSec(ci.time);
            ps.header.frame_id = "map";

            ps.pose.position.x = ci.p_C_G[0];
            ps.pose.position.y = ci.p_C_G[1];
            ps.pose.position.z = ci.p_C_G[2];
            msckf_mono::Quaternion<float> q_out = ci.q_CG.inverse();
            ps.pose.orientation.w = q_out.w();
            ps.pose.orientation.x = q_out.x();
            ps.pose.orientation.y = q_out.y();
            ps.pose.orientation.z = q_out.z();

            pruned_path.poses.push_back(ps);
          }

          pruned_cam_states_track_pub.publish(pruned_path);
        }

        {
          imu_path.header.stamp = cur_ros_time;
          imu_path.header.frame_id = "map";
          geometry_msgs::PoseStamped imu_pose;
          imu_pose.header = imu_path.header;
          imu_pose.pose.position.x = imu_state.p_I_G[0];
          imu_pose.pose.position.y = imu_state.p_I_G[1];
          imu_pose.pose.position.z = imu_state.p_I_G[2];
          msckf_mono::Quaternion<float> q_out = imu_state.q_IG.inverse();
          imu_pose.pose.orientation.w = q_out.w();
          imu_pose.pose.orientation.x = q_out.x();
          imu_pose.pose.orientation.y = q_out.y();
          imu_pose.pose.orientation.z = q_out.z();

          imu_path.poses.push_back(imu_pose);

          imu_track_pub.publish(imu_path);
        }
        TEND(publishing);
        TRECORD(publishing);

        time_state_pub.publish(timing_data);

        r_cam.sleep();
      }
    }

    if (imu_reading) {
      state_k++;
      imu_reading_buffer.push_back(std::make_pair(sync->get_time(), imu_reading.get()));

      msckf_mono::imuState<float> prev_imu_state = msckf.getImuState();
      Eigen::Vector3f cam_frame_av = (camera.q_CI.inverse() * (imu_data.omega-prev_imu_state.b_g));
      th.add_gyro_reading(cam_frame_av);
    }

    sync->next();
  }
}
