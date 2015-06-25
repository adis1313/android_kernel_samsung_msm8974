#!/bin/bash
if [ "${1}" = "skip" ] ; then
	device=$(echo $(\ls *.img) | sed s/.img//g)
	rm arter97-kernel-"$device"-"$(cat version)".zip 2>/dev/null
	cp *.img kernelzip/boot.img
	cd kernelzip/
	7z a -mx9 arter97-kernel-"$device"-"$(cat ../version)"-tmp.zip *
	zipalign -v 4 arter97-kernel-"$device"-"$(cat ../version)"-tmp.zip ../arter97-kernel-"$device"-"$(cat ../version)".zip
	rm arter97-kernel-"$device"-"$(cat ../version)"-tmp.zip
	cd ..
	ls -al arter97-kernel-"$device"-"$(cat version)".zip
	exit 0
fi

./build_clean.sh
./build_kernel_e330.sh CC='$(CROSS_COMPILE)gcc' "$@" || exit 1

device=$(echo $(\ls e330*.img) | sed s/.img//g)
rm arter97-kernel-"$device"-"$(cat version)".zip 2>/dev/null
cp e330*.img kernelzip/boot.img
cd kernelzip/
7z a -mx9 arter97-kernel-"$device"-"$(cat ../version)"-tmp.zip *
zipalign -v 4 arter97-kernel-"$device"-"$(cat ../version)"-tmp.zip ../arter97-kernel-"$device"-"$(cat ../version)".zip
rm arter97-kernel-"$device"-"$(cat ../version)"-tmp.zip
cd ..
ls -al arter97-kernel-"$device"-"$(cat version)".zip

./build_clean.sh nozip
./build_kernel_i9506.sh CC='$(CROSS_COMPILE)gcc' "$@" || exit 1

rm arter97-kernel-i9506-"$(cat version)".zip 2>/dev/null
cp i9506.img kernelzip/boot.img
cd kernelzip/
7z a -mx9 arter97-kernel-i9506-"$(cat ../version)"-tmp.zip *
zipalign -v 4 arter97-kernel-i9506-"$(cat ../version)"-tmp.zip ../arter97-kernel-i9506-"$(cat ../version)".zip
rm arter97-kernel-i9506-"$(cat ../version)"-tmp.zip
cd ..
ls -al arter97-kernel*.zip
