#include "ros/ros.h"
#include <tf/transform_listener.h>
#include <sensor_msgs/point_cloud_conversion.h>

#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <pcl_ros/transforms.h>

#include <centauro_costmap/CostMap.h>
#include <terrain_classifier/cloud_matrix_loador.h>


tf::TransformListener* tfListener = NULL;
sensor_msgs::PointCloud2 cloud_recieved_map_;
Cloud_Matrix_Loador* cml;

ros::Publisher  pub_cloud, pub_costmap1, pub_costmap2, pub_costmap3;


pcl::PointCloud<pcl::PointXYZRGB> ground_cloud1_, ground_cloud2_, ground_cloud3_;
centauro_costmap::CostMap cost_map1_, cost_map2_, cost_map3_;

float robot_x_, robot_y_;

string map_frame = "map";
string output_frame = "base_link_oriented";
string process_frame = "map";

bool initialized = false;

void publish(ros::Publisher pub, pcl::PointCloud<pcl::PointXYZ> cloud, int type = 2)
{
    sensor_msgs::PointCloud2 pointlcoud2;
    pcl::toROSMsg(cloud, pointlcoud2);

    if(type == 2)
    { 
        pub.publish(pointlcoud2);
    }
    else
    {
        sensor_msgs::PointCloud pointlcoud;
        sensor_msgs::convertPointCloud2ToPointCloud(pointlcoud2, pointlcoud);

        pointlcoud.header = pointlcoud2.header;
        pub.publish(pointlcoud);
    }

}

void publish(ros::Publisher pub, pcl::PointCloud<pcl::PointXYZRGB> cloud, int type = 2)
{
    sensor_msgs::PointCloud2 pointlcoud2;
    pcl::toROSMsg(cloud, pointlcoud2);

    pub.publish(pointlcoud2);
}

sensor_msgs::PointCloud2 transform_cloud(sensor_msgs::PointCloud2 cloud_in, string frame_target, ros::Time stamp)
{
    ////////////////////////////////// transform ////////////////////////////////////////
    sensor_msgs::PointCloud2 cloud_out;
    tf::StampedTransform to_target;

    try 
    {
        // tf_listener_->waitForTransform(frame_target, cloud_in.header.frame_id, cloud_in.header.stamp, ros::Duration(1.0));
        // tf_listener_->lookupTransform(frame_target, cloud_in.header.frame_id, cloud_in.header.stamp, to_target);
        tfListener->lookupTransform(frame_target, cloud_in.header.frame_id, stamp, to_target);
    }
    catch (tf::TransformException& ex) 
    {
        ROS_WARN("[draw_frames] TF exception:\n%s", ex.what());
        cloud_in.height = 0;
        cloud_in.width = 0;

        return cloud_in;
    }

    Eigen::Matrix4f eigen_transform;
    pcl_ros::transformAsMatrix (to_target, eigen_transform);
    pcl_ros::transformPointCloud (eigen_transform, cloud_in, cloud_out);

    cloud_out.header.frame_id = frame_target;
    return cloud_out;
}

void convert_to_costmap(Mat height, Mat h_diff, Mat slope, Mat roughness, Mat cost, float resoluation, centauro_costmap::CostMap &cost_map, float robot_x, float robot_y)
{
    cost_map.cells_x = h_diff.cols;
    cost_map.cells_y = h_diff.rows;
    cost_map.resolution = resoluation;

    cost_map.origin_x = robot_x - 0.5 * cost_map.cells_x * resoluation;
    cost_map.origin_y = robot_y - 0.5 * cost_map.cells_y * resoluation;

    cost_map.height.resize(cost_map.cells_x * cost_map.cells_y);
    cost_map.height_diff.resize(cost_map.cells_x * cost_map.cells_y);
    cost_map.slope.resize(cost_map.cells_x * cost_map.cells_y);
    cost_map.roughness.resize(cost_map.cells_x * cost_map.cells_y);
    cost_map.semantic_cost.resize(cost_map.cells_x * cost_map.cells_y);

    cout << "map size: " << cost_map.cells_x << " " << cost_map.cells_y << endl;

    for(int row = 0; row < h_diff.rows; row ++)
    {
        for(int col = 0; col < h_diff.cols; col ++)
        {
            float cost_v        = cost.ptr<float>(row)[col]; 
            float height_v      = height.ptr<float>(row)[col];
            float height_diff   = h_diff.ptr<float>(row)[col];
            float slope_v       = slope.ptr<float>(row)[col];
            float roughness_v   = roughness.ptr<float>(row)[col];

            int index = row * h_diff.rows + col;

            if(cost_v == -1)
            {
                cost_map.height[index]       = std::numeric_limits<float>::quiet_NaN();;
                cost_map.height_diff[index]  = std::numeric_limits<float>::quiet_NaN();;
                cost_map.slope[index]        = std::numeric_limits<float>::quiet_NaN();;
                cost_map.roughness[index]    = std::numeric_limits<float>::quiet_NaN();;
            }
            else
            {
                cost_map.height[index]       = height_v;
                cost_map.height_diff[index]  = height_diff;
                cost_map.slope[index]        = slope_v;
                cost_map.roughness[index]    = roughness_v;
            }
        }
    }
}

void set_output_frame(string output_frame, ros::Time stamp)
{
    cost_map1_.header.frame_id = output_frame;
    cost_map2_.header.frame_id = output_frame;
    cost_map3_.header.frame_id = output_frame;
    cost_map1_.header.stamp = stamp;
    cost_map2_.header.stamp = stamp;
    cost_map3_.header.stamp = stamp; 

    // ground_cloud1_.header.frame_id = output_frame;
    // ground_cloud2_.header.frame_id = output_frame;
    // ground_cloud3_.header.frame_id = output_frame;
}


bool process_cloud(sensor_msgs::PointCloud2 cloud_in, const sensor_msgs::ImageConstPtr& image_msg)
{
    cout << "cloud recieved: " << ros::Time::now() << endl;
    sensor_msgs::PointCloud2 cloud_transformed = transform_cloud(cloud_in, process_frame, image_msg->header.stamp);
    if(cloud_transformed.height == 0 && cloud_transformed.width == 0)
        return 0;

    pcl::PointCloud<pcl::PointXYZ> pcl_cloud;
    pcl::fromROSMsg(cloud_transformed, pcl_cloud);

    ros::Time begin = ros::Time::now();

    // save transform to world
    tf::StampedTransform to_target;
    try 
    {
        tfListener->lookupTransform("map", "ego_rot", image_msg->header.stamp, to_target);
    }
    catch (tf::TransformException& ex) 
    {
        ROS_WARN("TF exception:\n%s", ex.what());
        return 0;
    }

    // process cloud

    robot_x_ = to_target.getOrigin().x();
    robot_y_ = to_target.getOrigin().y();

    ground_cloud1_ = cml->process_cloud(pcl_cloud, 12, 12, 6, 0.05, 0.015, robot_x_, robot_y_);
    ground_cloud1_.header.frame_id = process_frame;
    convert_to_costmap(cml->output_height_, cml->output_height_diff_, cml->output_slope_, cml->output_roughness_, cml->output_cost_, 0.05, cost_map1_, robot_x_, robot_y_);

    // ground_cloud2_ = cml->process_cloud(pcl_cloud, 5, 5, 6, 0.01, 0.015);
    // ground_cloud2_.header.frame_id = process_frame;
    // convert_to_costmap(cml->output_height_, cml->output_height_diff_, cml->output_slope_, cml->output_roughness_, cml->output_cost_, 0.05, cost_map2_, robot_x_, robot_y_);


    // ground_cloud3_ = cml->process_cloud(pcl_cloud, 30, 30, 6, 1.0, 0.015);
    // ground_cloud3_.header.frame_id = process_frame;
    // convert_to_costmap(cml->output_height_, cml->output_height_diff_, cml->output_slope_, cml->output_roughness_, cml->output_cost_, 1.0, cost_map3_, robot_x_, robot_y_);

    cout << "robot position : " << robot_x_ << " " << robot_y_ << endl;

    // transform point back to map frame
    //Eigen::Matrix4f eigen_transform;
    //pcl_ros::transformAsMatrix (to_target, eigen_transform);
    //pcl::transformPointCloud (ground_cloud1_, ground_cloud1_, eigen_transform);
    //pcl::transformPointCloud (ground_cloud2_, ground_cloud2_, eigen_transform);
    //pcl::transformPointCloud (ground_cloud3_, ground_cloud3_, eigen_transform);

    set_output_frame(output_frame, image_msg->header.stamp);

    cout << ros::Time::now() - begin << "  loaded cloud *********************" << endl;

    return 1;
    // publish(pub_cloud, ground_cloud2_); // publishing colored points with defalt cost function

    // pub_costmap1.publish(cost_map1);
    // pub_costmap2.publish(cost_map2);
    // pub_costmap3.publish(cost_map3);    
}

  cv::Point2d project3D_to_image(cv::Point3d& xyz, string frame_id )
  {
    double fx, fy, cx, cy; 
    
    //   fx = 529.9732789120519;
    //   fy = 526.9663404399863;
    //   cx = 477.4416333879422;
    //   cy = 261.8692914553029;
	
    fx = 1090.0084356403304;
    fy = 1093.4799569173076;
    cx = 944.9003536394688;
    cy = 527.4736145938574;

    cv::Point2d uv_rect;
    uv_rect.x = (fx*xyz.x) / xyz.z + cx;
    uv_rect.y = (fy*xyz.y) / xyz.z + cy;

    // cout << "projected uv: "<< xyz.x << " " << xyz.y << endl;
    return uv_rect;
  }

Mat image_cloud_mapper(const sensor_msgs::ImageConstPtr& image_msg, pcl::PointCloud<pcl::PointXYZRGB> &ground_cloud, float map_width, float map_broad, float map_resolution)
{
    // init output image and transform pointcloud to camera frame
    string camera_frame = "kinect2_rgb_optical_frame";
    // string camera_frame = image_msg->header.frame_id;
    pcl::PointCloud<pcl::PointXYZRGB> ground_cloud_camera, ground_cloud_base;

    Mat img_seg;
    int img_rows = std::ceil(map_width/map_resolution);
    int img_cols = std::ceil(map_broad/map_resolution);
    Mat map_label = Mat(img_rows, img_cols, CV_32FC1,  Scalar(-1));
    // Mat map_label = Mat(img_rows, img_cols, CV_8UC1,  Scalar(0));

    if(ground_cloud.points.size() == 0)
        return map_label;
    cout << "convert image" << endl;
    try {
        /////////////////////////////////////////////////////////////////////////////////////////////////
        /////////////////////////////////  convert and scale the image //////////////////////////////////
        /////////////////////////////////////////////////////////////////////////////////////////////////

        Mat img_seg_raw = cv_bridge::toCvShare(image_msg, "mono8")->image;
        // Mat img_seg_raw = cv_bridge::toCvShare(image_msg, "bgr8")->image;
        resize(img_seg_raw, img_seg, Size(1920, 1080), 0, 0, INTER_NEAREST);

        // cout << "image raw: " << img_seg_raw.rows << " " << img_seg_raw.cols << endl;
       // imshow("img_seg", img_seg*100);
       // waitKey(50);
       cout << "converted image" << endl;

        tf::StampedTransform to_camera;
        Eigen::Matrix4f eigen_transform_tocamera;
        tfListener->waitForTransform(camera_frame, ground_cloud.header.frame_id, image_msg->header.stamp, ros::Duration(0.15));
        tfListener->lookupTransform(camera_frame, ground_cloud.header.frame_id, image_msg->header.stamp, to_camera);
        pcl_ros::transformAsMatrix (to_camera, eigen_transform_tocamera);
        pcl::transformPointCloud (ground_cloud, ground_cloud_camera, eigen_transform_tocamera);

        cout << "transformed cloud to camera " << ground_cloud.points.size() << endl;
       // tf::StampedTransform to_base;
       // Eigen::Matrix4f eigen_transform_tobase;
       // tfListener->waitForTransform(process_frame, ground_cloud.header.frame_id, image_msg->header.stamp, ros::Duration(0.15));
       // tfListener->lookupTransform(process_frame, ground_cloud.header.frame_id, image_msg->header.stamp, to_base);
       // pcl_ros::transformAsMatrix (to_base, eigen_transform_tobase);
       // pcl::transformPointCloud (ground_cloud, ground_cloud_base, eigen_transform_tobase);
		for(int i = 0; i < ground_cloud.points.size(); i++)
		{
            pcl::PointXYZRGB point = ground_cloud.points[i];
            point.x -= robot_x_;
            point.y -= robot_y_;
            ground_cloud_base.points.push_back(point);
		}
        cout << "transformed cloud to base" << endl;
    }
    catch (cv_bridge::Exception& ex){
        cout << ex.what() << endl;
        return map_label; 
    } 

    cout << "ready for point projection " << camera_frame << " " << ground_cloud.header.frame_id << endl;
    // project points to image
    for(int i = 0; i < ground_cloud_camera.points.size(); i++)
    {
        pcl::PointXYZRGB point_camera = ground_cloud_camera.points[i];
        pcl::PointXYZRGB point_base   = ground_cloud_base.points[i];

        // cout << "projected uv: "<< ground_cloud.points[i].x << " " << point_camera.y << endl;
        if(point_camera.z < 0)
            continue;

        cv::Point2d uv;
        cv::Point3d pt_cv(point_camera.x, point_camera.y, point_camera.z);
        uv = project3D_to_image(pt_cv, camera_frame);
      
        // cout << "camera: " << point_camera.x << " " << point_camera.y << " " << point_camera.z << endl;
        // cout << "base: " << point_base.x << " " << point_base.y << " " << point_base.z << endl;
        // cout << "projected uv: "<< uv.x << " " << uv.y << endl;

        // check is the projected point inside image range
        if(uv.x >= 100 && uv.x < img_seg.cols -100 && uv.y >= 100 && uv.y < img_seg.rows-100)
        {
            // cout << "projected uv: "<< uv.x << " " << uv.y << endl;

            /////////////////////////////////////////////////////////////////////////////////////////////////
            ///////////////////////////////// reading label value from img_seg //////////////////////////////
            /////////////////////////////////////////////////////////////////////////////////////////////////

            float label_cost = (float)(img_seg.at<uchar>(uv.y, uv.x));
            // Vec3b label_cost_rgb = img_seg.at<Vec3b>(uv.y, uv.x);
            // float label_cost = (label_cost_rgb.val[0] + label_cost_rgb.val[1] + label_cost_rgb.val[2])/3;

            // compute index on the map
            point_base.x     += map_width/2;
            point_base.y     += map_broad/2;
            int col          =  point_base.x / map_resolution;
            int row          =  point_base.y / map_resolution;
            col              =  img_cols - col;

            // cout << "image: " << uv.y << " " << uv.x << endl;
            // cout << "map: " << row << " " << col << endl;
            if(col >= 0 && col < map_label.cols && row >= 0 && row < map_label.rows)
            {
                /////////////////////////////////////////////////////////////////////////////////////////////////
                ///////////////////////////////// writing label value to map_label //////////////////////////////
                /////////////////////////////////////////////////////////////////////////////////////////////////

                // cout << "label value: " << label_cost << endl;
                cv::circle(map_label, Point(col, row), 3, Scalar(label_cost/2), -1);  

                // cv::circle(map_label, Point(col, row), 3, Scalar(label_cost.val[0]), -1);  
                // map_label.at<float>(col, row) = label_cost;
                // map_label.at<Vec3b>(row, col) = label_cost;
                // ground_cloud.points[i].r = label_cost * 100;
                // ground_cloud.points[i].g = label_cost * 100;
                // ground_cloud.points[i].b = label_cost * 100;
            }    

        }
    } 

//    imshow("map_label", map_label);

//    waitKey(50);

    cout << "projection finished" << endl;
    // ground_cloud_camera.header.frame_id = camera_frame;

    return map_label; 
}

void callback_cloud(const sensor_msgs::PointCloud2ConstPtr &cloud_in)
{
    cout << "cloud in " << endl;
    cloud_recieved_map_ = transform_cloud(*cloud_in, map_frame, cloud_in->header.stamp);
    if(cloud_recieved_map_.height == 0 && cloud_recieved_map_.width == 0)
        initialized = false;
    else
        initialized = true;
    // cloud_recieved_map_ = transform_cloud(*cloud_in, map_frame, ros::Time(0));
    // cout << "points number:  " << cloud_recieved_map_.points.size() << endl;

}

void imageCallback_seg(const sensor_msgs::ImageConstPtr& image_msg)
{
    cout << "compressed image" << endl;
    // Mat img_comp = cv_bridge::toCvShare(image_msg, "bgr8")->image;
    // resize(img_comp, img_comp, Size(1920, 1080), 0, 0, INTER_NEAREST);
    // imshow("compressed_img", img_comp);
    // waitKey(50);
    // return;

    if(!initialized)
        return; 

    if(!process_cloud(cloud_recieved_map_, image_msg))
        return; 

    cout << "in image call back" << endl;
    Mat label_map = image_cloud_mapper(image_msg, ground_cloud1_, 12, 12, 0.05);

    // cout << "map: " << cost_map1_.cells_x << " " << cost_map1_.cells_y << endl;
    // cout << "image:  " << label_map.cols << " " << label_map.rows << endl;
    for(int row = 0; row < label_map.rows; row ++)
    {
        for(int col = 0; col < label_map.cols; col ++)
        {
            float semantic_v = label_map.ptr<float>(row)[col];

            int index = row * label_map.rows + col;

            if(semantic_v == -1)
                cost_map1_.semantic_cost[index] = std::numeric_limits<float>::quiet_NaN();
            else
                cost_map1_.semantic_cost[index] = semantic_v;
        }
    }

    publish(pub_cloud, ground_cloud1_);
    pub_costmap1.publish(cost_map1_);
    // pub_costmap2.publish(cost_map2_);
    // pub_costmap3.publish(cost_map3_);

    // imshow("label_map", label_map);
    // waitKey(20);
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "fused_classifier");

    cml = new Cloud_Matrix_Loador();


    ros::NodeHandle node; 
    tfListener = new (tf::TransformListener);

    ros::Subscriber sub_cloud     = node.subscribe<sensor_msgs::PointCloud2>("/points_raw", 1, callback_cloud);

    image_transport::ImageTransport it(node);
    image_transport::Subscriber sub_raw = it.subscribe("/image_seg", 1, imageCallback_seg);

    // ros::Subscriber sub_image_seg = node.subscribe<sensor_msgs::Image>("/image_seg", 1, imageCallback_seg);

    // ros::Subscriber sub_velodyne_left  = node.subscribe<sensor_msgs::PointCloud2>("/ndt_map", 1, callback_cloud);
    pub_cloud      = node.advertise<sensor_msgs::PointCloud2>("/cloud_filtered", 1);

    pub_costmap1 = node.advertise<centauro_costmap::CostMap>("/terrain_classifier/map1", 1);
    pub_costmap2 = node.advertise<centauro_costmap::CostMap>("/terrain_classifier/map2", 1);
    pub_costmap3 = node.advertise<centauro_costmap::CostMap>("/terrain_classifier/map3", 1);

    ros::spin();

    return 0;
}
