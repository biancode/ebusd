#!/bin/sh
set -e
(cd buildenv && docker build -t ebusd-buildenv .)
docker run --rm -it -v `pwd`/../..:/build ebusd-buildenv ./make_debian.sh
export EBUSD_VERSION=`cat ../../VERSION`
export EBUSD_ARCH=`docker version|grep -i "Arch[^:]*server"|head -n 1|sed -e 's#^.*/##'`
cp -a ../../ebusd-${EBUSD_VERSION}_${EBUSD_ARCH}.deb runtime/
(cd runtime && docker build -t ebusd .)
echo "docker image created."
echo
echo "to run it interactively on serial device /dev/ttyUSB1:"
echo "docker run --rm -it --device=/dev/ttyUSB1:/dev/ttyUSB0 -p 127.0.0.1:8888:8888 ebusd"
echo
echo "to start it in background on serial device /dev/ttyUSB1:"
echo "docker run device=/dev/ttyUSB1:/dev/ttyUSB0 -p 127.0.0.1:8888:8888 ebusd"
