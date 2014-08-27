#!/bin/bash
for i in $( ls ../../include/yocto3d/ | grep \.h); do
	# for each header file, we edit the source file to include the "yocto3d/" prefix when including that header.
	sed -i "s|\"$i\"|\"yocto3d/$i\"|g" `find -name '*.cpp'`
done