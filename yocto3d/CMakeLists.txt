cmake_minimum_required(VERSION 2.8.3)
project(yocto3d)
find_package(catkin_simple REQUIRED)
catkin_simple()

cs_add_library(yocto3d
  	src/lib/yocto3d.cpp

	src/lib/yocto_accelerometer.cpp
	src/lib/yocto_altitude.cpp
	src/lib/yocto_anbutton.cpp
	src/lib/yocto_api.cpp
	src/lib/yocto_carbondioxide.cpp
	src/lib/yocto_colorled.cpp
	src/lib/yocto_compass.cpp
	src/lib/yocto_current.cpp
	src/lib/yocto_datalogger.cpp
	src/lib/yocto_digitalio.cpp
	src/lib/yocto_display.cpp
	src/lib/yocto_dualpower.cpp
	src/lib/yocto_files.cpp
	src/lib/yocto_genericsensor.cpp
	src/lib/yocto_gyro.cpp
	src/lib/yocto_hubport.cpp
	src/lib/yocto_humidity.cpp
	src/lib/yocto_led.cpp
	src/lib/yocto_lightsensor.cpp
	src/lib/yocto_magnetometer.cpp
	src/lib/yocto_motor.cpp
	src/lib/yocto_network.cpp
	src/lib/yocto_oscontrol.cpp
	src/lib/yocto_power.cpp
	src/lib/yocto_pressure.cpp
	src/lib/yocto_pwmoutput.cpp
	src/lib/yocto_pwmpowersource.cpp
	src/lib/yocto_realtimeclock.cpp
	src/lib/yocto_refframe.cpp
	src/lib/yocto_relay.cpp
	src/lib/yocto_servo.cpp
	src/lib/yocto_temperature.cpp
	src/lib/yocto_tilt.cpp
	src/lib/yocto_voc.cpp
	src/lib/yocto_voltage.cpp
	src/lib/yocto_wakeupmonitor.cpp
	src/lib/yocto_wakeupschedule.cpp
	src/lib/yocto_watchdog.cpp
	src/lib/yocto_wireless.cpp

	src/lib/yapi/yapi.c
	src/lib/yapi/yfifo.c
	src/lib/yapi/yhash.c
	src/lib/yapi/yjson.c
	src/lib/yapi/ykey.c
	src/lib/yapi/ymemory.c
	src/lib/yapi/ypkt_lin.c
	src/lib/yapi/ypkt_osx.c
	src/lib/yapi/ypkt_win.c
	src/lib/yapi/yprog.c
	src/lib/yapi/ystream.c
	src/lib/yapi/ytcp.c
	src/lib/yapi/ythread.c
  )
target_link_libraries(yocto3d m pthread usb-1.0)

target_link_libraries(yocto3d)

cs_export(LIBRARIES yocto3d m pthread usb-1.0)
