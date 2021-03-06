#include <../include/simple_layer/grid_layer.hpp>
#include <pluginlib/class_list_macros.h>

PLUGINLIB_EXPORT_CLASS(grid_layer::GridLayer, costmap_2d::Layer)

using costmap_2d::FREE_SPACE;
using costmap_2d::LETHAL_OBSTACLE;
using costmap_2d::INSCRIBED_INFLATED_OBSTACLE;
using costmap_2d::NO_INFORMATION;

namespace grid_layer {

  void GridLayer::onInitialize() {
    nodeHandle_ = ros::NodeHandle("~/" + name_);
    current_ = true;
    default_value_ = NO_INFORMATION;
    matchSize();

    new_map_ = false;

    subscriber_ = nodeHandle_.subscribe("/traversability_estimation/traversability_map", 1, &GridLayer::topicCallback, this);
  }

  void GridLayer::topicCallback(const grid_map_msgs::GridMap& map_msg) {
    grid_map::GridMapRosConverter::fromMessage(map_msg, map_);
    new_map_ = true;
    resetMaps();
  }

  void GridLayer::matchSize() {
    Costmap2D* master = layered_costmap_->getCostmap();
    resizeMap(master->getSizeInCellsX(), master->getSizeInCellsY(), master->getResolution(), master->getOriginX(), master->getOriginY());
  }

  /*
    Main idea:
    use polar coordinates and make a transformation based on geometric rules
    to convert between two different 2D coordinate systems (traversability map and cost map)

    cost map initialized as NO_INFORMATION
    traversability: [0, 0.5] --> FREE_SPACE
    traversability: (0.5, 0.6] --> INSCRIBED_INFLATED_OBSTACLE
    traversability: anything else --> LETHAL_OBSTACLE
  */

  /* My modified update bounds */
  void GridLayer::updateBounds(double robot_x, double robot_y, double robot_yaw, double* min_x, double* min_y, double* max_x, double* max_y) {
    if (new_map_) {
      // ROS_WARN("LETHAL_OBSTACLE = %d, FREE_SPACE = %d, INSCRIBED_INFLATED_OBSTACLE = %d, NO_INFORMATION = %d\n", LETHAL_OBSTACLE, FREE_SPACE, INSCRIBED_INFLATED_OBSTACLE, NO_INFORMATION);

      /* get traversability map's "traversability" and "uncertainty" layers */
      grid_map::Matrix& trav_data = map_["traversability"];

      /* the map coordinates for which we will determine a cost */
      unsigned int mx, my;
      /* the world coordinates for which we will determine a cost */
      double mark_x, mark_y;
      /* cost variable */
      double cost;

      /* get traversability map's resolution and dimensions in meters and cells */
      double trav_map_res = map_.getResolution(), trav_map_met_x = map_.getLength().x(), trav_map_met_y = map_.getLength().y();
      int trav_map_cells_y = map_.getSize()[0], trav_map_cells_x = map_.getSize()[1], trav_data_size = trav_data.size();
      /* get costmap's resolution and dimensions in meters and cells */
      double cost_map_res = getResolution(), cost_map_met_x = getSizeInMetersX(), cost_map_met_y = getSizeInMetersY();
      int cost_map_cells_x = getSizeInCellsX(), cost_map_cells_y = getSizeInCellsY();
      /* ratios to help as with conversion -- xRatio = (Math.abs(srcMax-srcMin))/(Math.abs(destMax-destMin)); */
      double trav_max_x = trav_map_met_x, trav_min_x = 0, trav_max_y = trav_map_met_y, trav_min_y = 0,
             cost_max_x = cost_map_met_x, cost_min_x = 0, cost_max_y = cost_map_met_y, cost_min_y = 0,
             x_ratio = std::abs(trav_max_x - trav_min_x) / std::abs(cost_max_x - cost_min_x),
             y_ratio = std::abs(trav_max_y - trav_min_y) / std::abs(cost_max_y - cost_min_y);

      /* iterate traversability map, while calculating costs */
      for (grid_map::GridMapIterator iterator(map_); !iterator.isPastEnd(); ++iterator) {
        /* get linear index */
        const int i = iterator.getLinearIndex();
        double trav_value = trav_data(i);

        /* if it is not NaN, determine cost */
        if (!std::isnan(trav_value)) {
          /* position from traversability map's world to costmap's world */
          double cx = trav_map_res * int(trav_map_cells_x / 2 - i % trav_map_cells_x);
          double cy = trav_map_res * int(trav_map_cells_y / 2 - i / trav_map_cells_y);
          double d = sqrt(pow(cx, 2) + pow(cy, 2));
          mark_x = d * cos(atan(cy/cx)+robot_yaw) + getSizeInMetersX()/2;
          mark_y = d * sin(atan(cy/cx)+robot_yaw) + getSizeInMetersY()/2;

          /* set cost to map */
          if(worldToMap(mark_x, mark_y, mx, my)) {
            // ROS_WARN("%f", trav_value);
            /* apply our heuristic rule */
            if (trav_value <= 0.5) {        // traversability: [0, 0.5] --> FREE_SPACE
              cost = FREE_SPACE;
            }
            else if (trav_value <= 0.6) {   // traversability: (0.5, 0.6] --> INSCRIBED_INFLATED_OBSTACLE
              cost = INSCRIBED_INFLATED_OBSTACLE;
            }
            else {                          // traversability: anything else --> LETHAL_OBSTACLE
              cost = LETHAL_OBSTACLE;
            }

            // ROS_INFO("%f\n", cost);
            // ROS_INFO("trav_value = %f, cost = %f", trav_value, cost);
            // ROS_WARN("mark_x = %f, mark_y = %f", mark_x, mark_y);

            setCost(mx, my, cost);

            /* in the end we will have our desired bounds */
            /* UNOPTIMIZED BUT COMPLETELY FUNCTIONAL */
            *min_x = std::min(*min_x, -100.0);
            *min_y = std::min(*min_y, -100.0);
            *max_x = std::max(*max_x, 100.0);
            *max_y = std::max(*max_y, 100.0);

            // *min_x = std::min(*min_x, mark_x);
            // *min_y = std::min(*min_y, mark_y);
            // *max_x = std::max(*max_x, mark_x);
            // *max_y = std::max(*max_y, mark_y);
          }
        }
      }

      new_map_ = false;
    }
  }

  void GridLayer::updateCosts(costmap_2d::Costmap2D& master_grid, int min_i, int min_j, int max_i, int max_j) {
    for (int j = min_j; j < max_j; j++) {
      for (int i = min_i; i < max_i; i++) {
        int index = getIndex(i, j);
        if (costmap_[index] == NO_INFORMATION)
          continue;
        master_grid.setCost(i, j, costmap_[index]);
      }
    }
  }

} // end namespace
