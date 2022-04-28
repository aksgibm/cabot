/*******************************************************************************
 * Copyright (c) 2022  Carnegie Mellon University
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *******************************************************************************/

#include "cabot_navigation2/cabot_planner.hpp"

#include <math.h>
#include <memory.h>

#include <nav2_navfn_planner/navfn.hpp>
#include <nav2_map_server/map_io.hpp>
#include <nav2_util/lifecycle_node.hpp>
#include <nav2_util/lifecycle_utils.hpp>
#include <ament_index_cpp/get_package_share_directory.hpp>
#include <boost/filesystem.hpp>

#include "yaml-cpp/yaml.h"

using namespace std::chrono_literals;
namespace fs = boost::filesystem;

namespace cabot_navigation2 {
class Test : public nav2_util::LifecycleNode {
 public:
  Test(const rclcpp::NodeOptions &options = rclcpp::NodeOptions());
  ~Test() {}

  nav2_util::CallbackReturn on_configure(const rclcpp_lifecycle::State & state) override;
  nav2_util::CallbackReturn on_activate(const rclcpp_lifecycle::State & state) override;
  nav2_util::CallbackReturn on_deactivate(const rclcpp_lifecycle::State & state) override;
  nav2_util::CallbackReturn on_cleanup(const rclcpp_lifecycle::State & state) override;
  nav2_util::CallbackReturn on_shutdown(const rclcpp_lifecycle::State & state) override;  
  void run_test();

 private:
  nav_msgs::msg::Path getPath();

  std::string plugin_type_;
  nav2_core::GlobalPlanner::Ptr planner_;
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::TimerBase::SharedPtr timer2_;
  nav_msgs::msg::OccupancyGrid map_;
  nav_msgs::msg::Path path_;
  nav_msgs::msg::Path plan_;
  rclcpp_lifecycle::LifecyclePublisher<nav_msgs::msg::OccupancyGrid>::SharedPtr map_publisher_;
  rclcpp_lifecycle::LifecyclePublisher<nav_msgs::msg::Path>::SharedPtr path_publisher_;
  rclcpp_lifecycle::LifecyclePublisher<nav_msgs::msg::Path>::SharedPtr plan_publisher_;
  std::unique_ptr<std::thread> thread_;
  std::shared_ptr<nav2_costmap_2d::Costmap2DROS> costmap_ros_;
  std::unique_ptr<nav2_util::NodeThread> costmap_thread_;
  nav2_costmap_2d::Costmap2D *costmap_;
};
}  // namespace cabot_planner

int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  auto node = std::make_shared<cabot_navigation2::Test>();
  rclcpp::spin(node->get_node_base_interface());
  rclcpp::shutdown();
}

namespace cabot_navigation2 {

template <typename T>
T yaml_get_value(const YAML::Node &node, const std::string &key) {
  try {
    return node[key].as<T>();
  } catch (YAML::Exception &e) {
    std::stringstream ss;
    ss << "Failed to parse YAML tag '" << key << "' for reason: " << e.msg;
    throw YAML::Exception(e.mark, ss.str());
  }
}

Test::Test(const rclcpp::NodeOptions &options)
  : nav2_util::LifecycleNode("cabot_planner_test", "", true, options) {

  RCLCPP_INFO(get_logger(), "Creating");

  // Setup the global costmap
  costmap_ros_ = std::make_shared<nav2_costmap_2d::Costmap2DROS>(
      "global_costmap", std::string{get_namespace()}, "global_costmap");
  // Launch a thread to run the costmap node
  costmap_thread_ = std::make_unique<nav2_util::NodeThread>(costmap_ros_);
}

nav2_util::CallbackReturn Test::on_configure(const rclcpp_lifecycle::State & state) {
  RCLCPP_INFO(get_logger(), "on_configure");
  map_publisher_ = create_publisher<nav_msgs::msg::OccupancyGrid>("map", 10);
  path_publisher_ = create_publisher<nav_msgs::msg::Path>("path", 10);
  plan_publisher_ = create_publisher<nav_msgs::msg::Path>("plan", 10);

  costmap_ros_->on_configure(state);
  costmap_ = costmap_ros_->getCostmap();

  planner_ = std::make_shared<CaBotPlanner>();

  auto node = shared_from_this();
  planner_->configure(node, "CaBot", costmap_ros_->getTfBuffer(), costmap_ros_);

  return nav2_util::CallbackReturn::SUCCESS;
}

nav2_util::CallbackReturn Test::on_activate(const rclcpp_lifecycle::State & state) {
  RCLCPP_INFO(get_logger(), "on_activate");
  costmap_ros_->on_activate(state);
  planner_->activate();
  plan_publisher_->on_activate();
  path_publisher_->on_activate();
  map_publisher_->on_activate();

  /*
  timer_ = create_wall_timer(1s, [this]() -> void {
    map_publisher_->publish(map_);
    path_publisher_->publish(path_);
  });
  */
  timer2_ = create_wall_timer(0.033s, [this]() -> void {
    plan_ = std::static_pointer_cast<CaBotPlanner>(planner_)->getPlan();
    plan_publisher_->publish(plan_);
  });
  thread_ = std::make_unique<std::thread>(
    [&]()
    {
      RCLCPP_INFO(get_logger(), "run_test");
      run_test();
    });

  return nav2_util::CallbackReturn::SUCCESS;
}
nav2_util::CallbackReturn Test::on_deactivate(const rclcpp_lifecycle::State & state) {
  RCLCPP_INFO(get_logger(), "on_deactivate");

  return nav2_util::CallbackReturn::SUCCESS;
}
nav2_util::CallbackReturn Test::on_cleanup(const rclcpp_lifecycle::State & state) {
  RCLCPP_INFO(get_logger(), "on_cleanup");

  return nav2_util::CallbackReturn::SUCCESS;
}
nav2_util::CallbackReturn Test::on_shutdown(const rclcpp_lifecycle::State & state) {
  RCLCPP_INFO(get_logger(), "on_shutdown");

  return nav2_util::CallbackReturn::SUCCESS;
}


void Test::run_test() {
  fs::path base_path =
      ament_index_cpp::get_package_share_directory("cabot_navigation2");
  base_path /= "test";
  fs::path yaml_path = base_path / "test-cases.yaml";

  YAML::Node doc = YAML::LoadFile(yaml_path.string());
  const YAML::Node &tests = doc["tests"];

  for (unsigned long i = 0; i < tests.size(); i++) {
    const YAML::Node &test = tests[i];
    auto label = yaml_get_value<std::string>(test, "label");
    auto map = yaml_get_value<std::string>(test, "map");
    auto path = yaml_get_value<std::vector<float>>(test, "path");
    auto detour = yaml_get_value<std::string>(test, "detour");
    auto skip = yaml_get_value<bool>(test, "skip");
    if (skip) continue;

    nav_msgs::msg::Path path_;
    for (unsigned long j = 0; j < path.size(); j += 2) {
      geometry_msgs::msg::PoseStamped pose;
      pose.pose.position.x = path[j];
      pose.pose.position.y = path[j + 1];
      float yaw = 0;
      if (j < path.size()-3) {
        yaw = std::atan2(path[j+3]-path[j+1], path[j+2] - path[j]);
      } else if (3 < path.size()) {
        yaw = std::atan2(path[j+1]-path[j-1], path[j] - path[j-2]);
      }
      tf2::Quaternion q;
      q.setRPY(0, 0, yaw);
      pose.pose.orientation.x = q.x();
      pose.pose.orientation.y = q.y();
      pose.pose.orientation.z = q.z();
      pose.pose.orientation.w = q.w();
      path_.poses.push_back(pose);
    }
    path_.header.frame_id = "map";
    RCLCPP_INFO(get_logger(), "publish path");
    path_publisher_->publish(path_);

    fs::path map_path = base_path / map;
    nav2_map_server::LoadParameters yaml;
    if (boost::filesystem::exists(map_path)) {
      yaml = nav2_map_server::loadMapYaml(map_path.string());
      nav2_map_server::loadMapFromFile(yaml, map_);
      RCLCPP_INFO(get_logger(), "publish map");
      map_publisher_->publish(map_);
    } else {
      printf("file not found\n");
    }

    DetourMode mode = DetourMode::RIGHT;
    if (detour == "left") {
      mode = DetourMode::LEFT;
    }

    int rate = 10;
    rclcpp::Rate r(rate);
    for (int i = 0; i < rate; i++) {
      r.sleep();
    }

    for (int j = 0; j < 100; j++) {
      RCLCPP_INFO(get_logger(), "create plan");
      geometry_msgs::msg::PoseStamped start, goal;
      start.pose.position = path_.poses[0].pose.position;
      planner_->createPlan(start, goal);
      r.sleep();
      /*
      planner_->setParam(
          map_.info.width, map_.info.height, map_.info.origin.position.x,
          map_.info.origin.position.y, map_.info.resolution, mode);

      planner_->setPath(path_);

      planner_->prepare();
      plan_ = planner_->getPlan();

      auto start = std::chrono::system_clock::now();
      int count = 0;
      unsigned char *data = costmap_->getCharMap();
      planner_->setCost(data);
      while (rclcpp::ok()) {
        bool result = planner_->iterate();
        rclcpp::spin_some(this->get_node_base_interface());
        count++;
        // r.sleep();
        if (result) {
          break;
        }
      }
      auto end = std::chrono::system_clock::now();
      auto ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
      printf("%d iteration = %ldms\n", count, ms.count());
      */
    }
  }
}
}  // namespace cabot_planner
