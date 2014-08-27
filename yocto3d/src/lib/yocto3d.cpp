#include "yocto3d/yocto3d.h"

#include "yocto3d/yocto_api.h"
#include "yocto3d/yocto_tilt.h"
#include "yocto3d/yocto_compass.h"
#include "yocto3d/yocto_gyro.h"
#include "yocto3d/yocto_accelerometer.h"


#include <iostream>
#include <stdlib.h>

using namespace std;

string  errmsg;
YAccelerometer *accelerometer;

namespace yocto3d
{
  void initialize(){
    // Get access to your device, connected locally on USB for instance
    if(yRegisterHub("usb", errmsg) != YAPI_SUCCESS){
      cerr<<"ERROR: "<<errmsg<<endl;
      exit(1);
    }

    accelerometer = yFirstAccelerometer();
  }
  bool isConnected(){
    return accelerometer->isOnline();
  }
  Vector3 getAcceleration(){
    // Hot-plug is easy: just check that the device is online
    if(accelerometer->isOnline())
    {
        return Vector3(accelerometer->get_xValue(), accelerometer->get_yValue(), accelerometer->get_zValue());
    }else{
        return Vector3(0,0,0);
    }
  }
}

