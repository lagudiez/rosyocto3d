#include <yocto3d/yocto3d.h>
#include <imu/Orientation.h>
#include <ros/ros.h>

#include <iostream>
#include <stdlib.h>

using namespace std;

int main(int argc, char **argv){
	ros::init(argc, argv, "imu");
	ros::NodeHandle n;

	ros::Publisher orientation_pub = n.advertise<imu::Orientation>("imu/orientation", 1000);
	ros::Rate loop_rate(10);

	yocto3d::initialize();
	while(ros::ok() && yocto3d::isConnected()){
		yocto3d::Vector3 gravity_vector = yocto3d::getAcceleration();

		imu::Orientation message;
		message.gravity.x = gravity_vector.x;
		message.gravity.y = gravity_vector.z;	// we swap z and y in order to move the vector to the (right-hand) camera space 
		message.gravity.z = gravity_vector.y;

		orientation_pub.publish(message);
    	

		ros::spinOnce();
		loop_rate.sleep();
	}
	if(!ros::ok()){
		ROS_INFO("%s", "ros::ok() is false");
	}

	if(!yocto3d::isConnected()){
		ROS_INFO("%s", "yocto3d::isConnected() is false");
	}
	ROS_INFO("%s", "Exiting...");

	return 0;
}
