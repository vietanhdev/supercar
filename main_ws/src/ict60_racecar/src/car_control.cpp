#include "car_control.h"


CarControl::CarControl()
{

    readConfig();

    steer_publisher = node_obj1.advertise<std_msgs::Float32>(config.getTeamName() + "_steerAngle",10);
    speed_publisher = node_obj2.advertise<std_msgs::Float32>(config.getTeamName() + "_speed",10);


    // Init the kalman filter
    KF = cv::KalmanFilter(2, 1, 0);
    state = cv::Mat(2, 1, CV_32F); /* (phi, delta_phi) */
    process_noise = cv::Mat(2, 1, CV_32F);
    measurement = cv::Mat::zeros(1, 1, CV_32F);

    setIdentity(KF.measurementMatrix);
    setIdentity(KF.processNoiseCov, cv::Scalar::all(1e-5));
    setIdentity(KF.measurementNoiseCov, cv::Scalar::all(1e-1));
    setIdentity(KF.errorCovPost, cv::Scalar::all(1));
    randn(KF.statePost, cv::Scalar::all(0), cv::Scalar::all(0.1));



}

CarControl::~CarControl() {}

void CarControl::readConfig() {
    MAX_SPEED = config.get<float>("max_speed");
    MAX_ANGLE = config.get<float>("max_angle");
    delta_to_angle_coeff = config.get<float>("delta_to_angle_coeff");
    middle_interested_point_pos = config.get<float>("middle_interested_point_pos");
    min_num_of_middle_points = config.get<int>("min_num_of_middle_points");
    min_traffic_sign_bound_area = config.get<int>("min_traffic_sign_bound_area");
    traffic_sign_valid_duration = config.get<int>("traffic_sign_valid_duration");
    speed_on_preparing_to_turn_trafficsign = config.get<float>("speed_on_preparing_to_turn_trafficsign");
    lane_area_to_turn = config.get<int>("lane_area_to_turn");
    turning_angle_on_trafficsign = config.get<float>("turning_angle_on_trafficsign");
    speed_on_turning_trafficsign = config.get<float>("speed_on_turning_trafficsign");
    turning_duration_trafficsign = config.get<int>("turning_duration_trafficsign");

}


void CarControl::driverCar(float speed_data, float angle_data) {
    std_msgs::Float32 angle;
    std_msgs::Float32 speed;

    angle.data = angle_data;
    speed.data = speed_data;

    steer_publisher.publish(angle);
    speed_publisher.publish(speed);
}


void CarControl::driverCar(Road & road, const std::vector<TrafficSign> & traffic_signs) {

    //  STEP 1: FIND THE CAR POSITION

    // Do nothing when we find a bad result from lane detector
    if (road.middle_points.size() < 10) {
        return;
    }

    // Sort the middle points asc based on y
    std::sort(std::begin(road.middle_points), std::end(road.middle_points),
            [] (const cv::Point& lhs, const cv::Point& rhs) {
        return lhs.y < rhs.y;
    });

    // Choose an interested point (point having y = 60% ymax)
    int index_of_interested_point = static_cast<int>(road.middle_points.size() * middle_interested_point_pos);


    cv::Point middle_point = road.middle_points[index_of_interested_point];
    
    if (debug_flag) {
        std::cout << middle_point << std::endl;
    }
    
    cv::Point center_point = cv::Point(Road::road_center_line_x, middle_point.y);


    //  STEP 2: FIND THE "QUALITY" OF LANE DETECTION

    // The total difference of x of 2 continuous point in the lane line
    // Unit: pixel
    double total_of_difference_left = 0;
    double total_of_difference_right = 0;

    // The average difference of x of 2 continuous point in the lane line
    // Unit: pixel
    double average_difference_left = 0;
    double average_difference_right = 0;
    

    for (size_t i = 1; i < road.left_points.size(); ++i) {
        total_of_difference_left += abs(road.left_points[i].x - road.left_points[i-1].x);
    }
    for (size_t i = 1; i < road.right_points.size(); ++i) {
        total_of_difference_right += abs(road.right_points[i].x - road.right_points[i-1].x);
    }

    // Calculate the average difference
    average_difference_left = total_of_difference_left / road.left_points.size();
    average_difference_right = total_of_difference_right / road.right_points.size();

    std::cout << "average_difference_left: " << average_difference_left << std::endl;
    std::cout << "average_difference_right: " << average_difference_right << std::endl;


    //  STEP 3: FIND THE BASE CONTROLLING PARAMS ( BASED ON LANE LINES )
    float speed_data = MAX_SPEED;
    int delta = center_point.x - middle_point.x;
    float angle_data = delta * delta_to_angle_coeff;


    //  STEP 4: ADJUST CONTROLLING PARAMS USING TRAFFIC SIGN DETECTOR
    if (!traffic_signs.empty()) {

        std::cout << "TRAFFIC SIGN DETECTED!" << std::endl;
        std::cout << "Number: " << traffic_signs.size() << std::endl;

        for (int i = 0; i < traffic_signs.size(); ++i) {
            std::cout << traffic_signs[i].id << " : " << traffic_signs[i].rect << std::endl;
        }


        if (traffic_signs[0].rect.area() > min_traffic_sign_bound_area) {

            prepare_to_turn = true;
            last_sign_id = traffic_signs[0].id;
            last_sign_time_point = Timer::getCurrentTime();

        }
    }


    // Giảm tốc khi đến khúc cua
    if (prepare_to_turn && Timer::calcTimePassed(last_sign_time_point) < traffic_sign_valid_duration) {
        speed_data = speed_on_preparing_to_turn_trafficsign;
    }

    // Khi nhận thấy đang có tín hiệu rẽ,
    // và diện tích đường mở rộng (đã đến ngã ba, ngã tư) thì thực hiện rẽ
    if (prepare_to_turn && road.lane_area > lane_area_to_turn && Timer::calcTimePassed(last_sign_time_point) < traffic_sign_valid_duration) {
        prepare_to_turn = false;
        std::cout << "TURNING " << last_sign_id << std::endl;

        if (last_sign_id == TrafficSign::SignType::TURN_LEFT) {
            turning_coeff = -turning_angle_on_trafficsign;
        } else if (last_sign_id == TrafficSign::SignType::TURN_RIGHT) {
            turning_coeff = +turning_angle_on_trafficsign;
        }
        
        speed_data = speed_on_turning_trafficsign;
        turning_time_point = std::chrono::system_clock::now();
        is_turning = true;
    }



    std::cout << "turning_coeff: " << turning_coeff << std::endl;

    if (Timer::calcTimePassed(turning_time_point) > turning_duration_trafficsign) {
        turning_coeff = 0;
        speed_data = MAX_SPEED;
        is_turning = false;
    } else {
        speed_data = speed_on_turning_trafficsign;
    }


    // STEP 5: FIND AND AVOID OBSTACLE
    
    // Print line diff
    if (debug_flag) {
        ObstacleDetector::printLineDiff(road.middle_points);
    }
    

    // // Calculate the diff b/w current time and the last obstacle time
    // // If the time is out of obstacle avoiding range, reset obstacle_avoid_coeff
    // if (is_turning == true && Timer::calcTimePassed(obstacle_avoiding_time_point) > 1000) {
    //     obstacle_avoid_coeff = 0;
    //     ++success_turning_times;
    // }

    // if (debug_flag) {
    //     std::cout << "LAST OBSTACLE TIME: " << Timer::calcTimePassed(obstacle_avoiding_time_point) << std::endl;
    //     std::cout << "OBSTACLE COEFF: " << obstacle_avoid_coeff << std::endl;
    // }
    

    // Find the obstacle and adjust obstacle_avoid_coeff
    // for (int i = road.middle_points.size()-5; i >=  10; --i) {
    //     int diff = road.middle_points[i+3].x - road.middle_points[i].x;
    //     int distance_to_obstacle;

    //     if (abs(diff) > 5 and abs(diff) < 10) {
    //         distance_to_obstacle = Road::road_center_line_x - road.middle_points[i].y;
            
    //         if (debug_flag) {
    //             std::cout << "OBSTACLE DISTANCE: " << distance_to_obstacle << std::endl;
    //         }

    //         obstacle_avoiding_time_point = std::chrono::high_resolution_clock::now();
        
    //         if (diff > 0) {
    //             obstacle_avoid_coeff = -5;
    //             break;
    //         } else if (diff < 0) {
    //             obstacle_avoid_coeff = +5;
    //             break;
    //         }
        
    //     }

    // }
    

    angle_data += obstacle_avoid_coeff + turning_coeff;

    if (turning_coeff != 0) {
        angle_data = turning_coeff;
    }

    std::cout << "lane_area: " << road.lane_area << std::endl;
    


    // STEP 6: FINAL ADJUSTMENT AND PUBLISH

    // Filter the angle (remove small angle)
    if (abs(angle_data) < 1) {
        angle_data = 0;
    }

    // Fit to value ranges
    if (angle_data > MAX_ANGLE) angle_data = MAX_ANGLE;
    if (angle_data < -MAX_ANGLE) angle_data = -MAX_ANGLE;
    if (speed_data > MAX_SPEED) speed_data = MAX_SPEED;

    // Publish message
    if (debug_flag) {
        std::cout << "SPEED: " << speed_data << std::endl;
        std::cout << "ANGLE: " << angle_data << std::endl;
    }

    std_msgs::Float32 angle;
    std_msgs::Float32 speed;

    angle.data = angle_data;
    speed.data = speed_data;

    steer_publisher.publish(angle);
    speed_publisher.publish(speed);


}
