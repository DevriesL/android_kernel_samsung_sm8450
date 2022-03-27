#!/bin/bash

export PROJECT_ROOT=$PWD

rm -rf out/
./build_kernel_GKI.sh b0q_chn_openx user waipio

find out/msm-waipio-waipio-gki/dist/ -type f -name "*.ko" -exec \
  kernel_platform/prebuilts-master/clang/host/linux-x86/clang-r416183b/bin/llvm-objcopy --strip-debug {} \;

unlz4 boot.img.lz4
mv boot.img Android_boot_image_editor/boot.img
cd Android_boot_image_editor
rm -rf build/
./gradlew unpack
cd ..
cp -f out/msm-waipio-waipio-gki/dist/Image Android_boot_image_editor/build/unzip_boot/kernel
cd Android_boot_image_editor
./gradlew pack
rm boot.img
cd ..

unlz4 vendor_boot.img.lz4
mv vendor_boot.img Android_boot_image_editor/vendor_boot.img
cd Android_boot_image_editor
rm -rf build/
./gradlew unpack
cd ..
cp -f out/msm-waipio-waipio-gki/dist/*.ko Android_boot_image_editor/build/unzip_boot/root/lib/modules/
cp -f out/msm-waipio-waipio-gki/dist/*.ko Android_boot_image_editor/build/unzip_boot/root.1/lib/modules/
cd Android_boot_image_editor
./gradlew pack
rm vendor_boot.img
cd ..

mv Android_boot_image_editor/vendor_boot.img.clear out/vendor_boot.img
mv Android_boot_image_editor/boot.img.clear out/boot.img
cd out
lz4 -B6 --content-size boot.img
lz4 -B6 --content-size vendor_boot.img
tar -cvf AP_Kernel.tar boot.img.lz4 vendor_boot.img.lz4
rm boot.img vendor_boot.img boot.img.lz4 vendor_boot.img.lz4
mv AP_Kernel.tar ../
cd ..