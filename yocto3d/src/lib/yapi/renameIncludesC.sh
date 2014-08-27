#!/bin/bash
for i in $( ls ../../../include/yocto3d/yapi | grep \.h); do
	# for each header file, we edit the source file to include the "yocto3d/yapi" prefix when including that header.
	sed -i "s|\"$i\"|\"yocto3d/yapi/$i\"|g" `find -name '*.c'`
done