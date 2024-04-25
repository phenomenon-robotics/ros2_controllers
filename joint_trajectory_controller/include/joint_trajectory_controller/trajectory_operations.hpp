// Copyright (c) 2024 ros2_control Development Team
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

#ifndef JOINT_TRAJECTORY_CONTROLLER__TRAJECTORY_OPERATIONS_HPP_
#define JOINT_TRAJECTORY_CONTROLLER__TRAJECTORY_OPERATIONS_HPP_

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "joint_trajectory_controller/visibility_control.h"
#include "rclcpp/logging.hpp"
#include "rclcpp/time.hpp"
#include "trajectory_msgs/msg/joint_trajectory.hpp"
#include "trajectory_msgs/msg/joint_trajectory_point.hpp"

// auto-generated by generate_parameter_library
#include "joint_trajectory_controller_parameters.hpp"

namespace joint_trajectory_controller
{
using namespace std::chrono_literals;  // NOLINT

/**
 * \return The map between \p t1 indices (implicitly encoded in return vector indices) to \p t2
 * indices. If \p t1 is <tt>"{C, B}"</tt> and \p t2 is <tt>"{A, B, C, D}"</tt>, the associated
 * mapping vector is <tt>"{2, 1}"</tt>.
 */
template <class T>
inline std::vector<size_t> mapping(const T & t1, const T & t2)
{
  // t1 must be a subset of t2
  if (t1.size() > t2.size())
  {
    return std::vector<size_t>();
  }

  std::vector<size_t> mapping_vector(t1.size());  // Return value
  for (auto t1_it = t1.begin(); t1_it != t1.end(); ++t1_it)
  {
    auto t2_it = std::find(t2.begin(), t2.end(), *t1_it);
    if (t2.end() == t2_it)
    {
      return std::vector<size_t>();
    }
    else
    {
      const size_t t1_dist = std::distance(t1.begin(), t1_it);
      const size_t t2_dist = std::distance(t2.begin(), t2_it);
      mapping_vector[t1_dist] = t2_dist;
    }
  }
  return mapping_vector;
}

// sorts the joints of the incoming message to our local order
JOINT_TRAJECTORY_CONTROLLER_PUBLIC
void sort_to_local_joint_order(
  std::shared_ptr<trajectory_msgs::msg::JointTrajectory> trajectory_msg, rclcpp::Logger logger,
  const Params & params)
{
  // rearrange all points in the trajectory message based on mapping
  std::vector<size_t> mapping_vector = mapping(trajectory_msg->joint_names, params.joints);
  auto remap = [logger, params](
                 const std::vector<double> & to_remap,
                 const std::vector<size_t> & mapping) -> std::vector<double>
  {
    if (to_remap.empty())
    {
      return to_remap;
    }
    if (to_remap.size() != mapping.size())
    {
      RCLCPP_WARN(logger, "Invalid input size (%zu) for sorting", to_remap.size());
      return to_remap;
    }
    static std::vector<double> output(params.joints.size(), 0.0);
    // Only resize if necessary since it's an expensive operation
    if (output.size() != mapping.size())
    {
      output.resize(mapping.size(), 0.0);
    }
    for (size_t index = 0; index < mapping.size(); ++index)
    {
      auto map_index = mapping[index];
      output[map_index] = to_remap[index];
    }
    return output;
  };

  for (size_t index = 0; index < trajectory_msg->points.size(); ++index)
  {
    trajectory_msg->points[index].positions =
      remap(trajectory_msg->points[index].positions, mapping_vector);

    trajectory_msg->points[index].velocities =
      remap(trajectory_msg->points[index].velocities, mapping_vector);

    trajectory_msg->points[index].accelerations =
      remap(trajectory_msg->points[index].accelerations, mapping_vector);

    trajectory_msg->points[index].effort =
      remap(trajectory_msg->points[index].effort, mapping_vector);
  }
}

// fill trajectory_msg so it matches joints controlled by this controller
// positions set to current position, velocities, accelerations and efforts to 0.0
JOINT_TRAJECTORY_CONTROLLER_PUBLIC
void fill_partial_goal(
  std::shared_ptr<trajectory_msgs::msg::JointTrajectory> trajectory_msg,
  std::function<double(size_t)> get_default_position, const Params & params)
{
  const auto dof = params.joints.size();
  // joint names in the goal are a subset of existing joints, as checked in goal_callback
  // so if the size matches, the goal contains all controller joints
  if (dof == trajectory_msg->joint_names.size())
  {
    return;
  }

  trajectory_msg->joint_names.reserve(dof);

  for (size_t index = 0; index < dof; ++index)
  {
    {
      if (
        std::find(
          trajectory_msg->joint_names.begin(), trajectory_msg->joint_names.end(),
          params.joints[index]) != trajectory_msg->joint_names.end())
      {
        // joint found on msg
        continue;
      }
      trajectory_msg->joint_names.push_back(params.joints[index]);

      for (auto & it : trajectory_msg->points)
      {
        // Assume hold position with 0 velocity and acceleration for missing joints
        if (!it.positions.empty())
        {
          it.positions.push_back(get_default_position(index));
        }
        if (!it.velocities.empty())
        {
          it.velocities.push_back(0.0);
        }
        if (!it.accelerations.empty())
        {
          it.accelerations.push_back(0.0);
        }
        if (!it.effort.empty())
        {
          it.effort.push_back(0.0);
        }
      }
    }
  }
}

JOINT_TRAJECTORY_CONTROLLER_PUBLIC
bool validate_trajectory_point_field(
  size_t joint_names_size, const std::vector<double> & vector_field,
  const std::string & string_for_vector_field, size_t i, const bool allow_empty,
  rclcpp::Logger logger)
{
  if (allow_empty && vector_field.empty())
  {
    return true;
  }
  if (joint_names_size != vector_field.size())
  {
    RCLCPP_ERROR(
      logger, "Mismatch between joint_names size (%zu) and %s (%zu) at point #%zu.",
      joint_names_size, string_for_vector_field.c_str(), vector_field.size(), i);
    return false;
  }
  return true;
}

JOINT_TRAJECTORY_CONTROLLER_PUBLIC
bool validate_trajectory_msg(
  const trajectory_msgs::msg::JointTrajectory & trajectory, rclcpp::Logger logger, rclcpp::Time now,
  const Params & params)
{
  // If partial joints goals are not allowed, goal should specify all controller joints
  if (!params.allow_partial_joints_goal)
  {
    if (trajectory.joint_names.size() != params.joints.size())
    {
      RCLCPP_ERROR(logger, "Joints on incoming trajectory don't match the controller joints.");
      return false;
    }
  }

  if (trajectory.joint_names.empty())
  {
    RCLCPP_ERROR(logger, "Empty joint names on incoming trajectory.");
    return false;
  }

  const auto trajectory_start_time = static_cast<rclcpp::Time>(trajectory.header.stamp);
  // If the starting time it set to 0.0, it means the controller should start it now.
  // Otherwise we check if the trajectory ends before the current time,
  // in which case it can be ignored.
  if (trajectory_start_time.seconds() != 0.0)
  {
    auto trajectory_end_time = trajectory_start_time;
    for (const auto & p : trajectory.points)
    {
      trajectory_end_time += p.time_from_start;
    }
    if (trajectory_end_time < now)
    {
      RCLCPP_ERROR(
        logger, "Received trajectory with non-zero start time (%f) that ends in the past (%f)",
        trajectory_start_time.seconds(), trajectory_end_time.seconds());
      return false;
    }
  }

  for (size_t i = 0; i < trajectory.joint_names.size(); ++i)
  {
    const std::string & incoming_joint_name = trajectory.joint_names[i];

    auto it = std::find(params.joints.begin(), params.joints.end(), incoming_joint_name);
    if (it == params.joints.end())
    {
      RCLCPP_ERROR(
        logger, "Incoming joint %s doesn't match the controller's joints.",
        incoming_joint_name.c_str());
      return false;
    }
  }

  if (trajectory.points.empty())
  {
    RCLCPP_ERROR(logger, "Empty trajectory received.");
    return false;
  }

  if (!params.allow_nonzero_velocity_at_trajectory_end)
  {
    for (size_t i = 0; i < trajectory.points.back().velocities.size(); ++i)
    {
      if (
        std::fabs(trajectory.points.back().velocities.at(i)) >
        std::numeric_limits<float>::epsilon())
      {
        RCLCPP_ERROR(
          logger, "Velocity of last trajectory point of joint %s is not zero: %.15f",
          trajectory.joint_names.at(i).c_str(), trajectory.points.back().velocities.at(i));
        return false;
      }
    }
  }

  rclcpp::Duration previous_traj_time(0ms);
  for (size_t i = 0; i < trajectory.points.size(); ++i)
  {
    if ((i > 0) && (rclcpp::Duration(trajectory.points[i].time_from_start) <= previous_traj_time))
    {
      RCLCPP_ERROR(
        logger,
        "Time between points %zu and %zu is not strictly increasing, it is %f and %f respectively",
        i - 1, i, previous_traj_time.seconds(),
        rclcpp::Duration(trajectory.points[i].time_from_start).seconds());
      return false;
    }
    previous_traj_time = trajectory.points[i].time_from_start;

    const size_t joint_count = trajectory.joint_names.size();
    const auto & points = trajectory.points;
    // This currently supports only position, velocity and acceleration inputs
    if (params.allow_integration_in_goal_trajectories)
    {
      const bool all_empty = points[i].positions.empty() && points[i].velocities.empty() &&
                             points[i].accelerations.empty();
      const bool position_error =
        !points[i].positions.empty() &&
        !validate_trajectory_point_field(
          joint_count, points[i].positions, "positions", i, false, logger);
      const bool velocity_error =
        !points[i].velocities.empty() &&
        !validate_trajectory_point_field(
          joint_count, points[i].velocities, "velocities", i, false, logger);
      const bool acceleration_error =
        !points[i].accelerations.empty() &&
        !validate_trajectory_point_field(
          joint_count, points[i].accelerations, "accelerations", i, false, logger);
      if (all_empty || position_error || velocity_error || acceleration_error)
      {
        return false;
      }
    }
    else if (
      !validate_trajectory_point_field(
        joint_count, points[i].positions, "positions", i, false, logger) ||
      !validate_trajectory_point_field(
        joint_count, points[i].velocities, "velocities", i, true, logger) ||
      !validate_trajectory_point_field(
        joint_count, points[i].accelerations, "accelerations", i, true, logger))
    {
      return false;
    }
    // reject effort entries
    if (!points[i].effort.empty())
    {
      RCLCPP_ERROR(logger, "Trajectories with effort fields are currently not supported.");
      return false;
    }
  }
  return true;
}

}  // namespace joint_trajectory_controller

#endif  // JOINT_TRAJECTORY_CONTROLLER__TRAJECTORY_OPERATIONS_HPP_
