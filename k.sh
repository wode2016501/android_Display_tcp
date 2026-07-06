mkdir build 
cd build
cmake -DCMAKE_TOOLCHAIN_FILE=/media/wode/ec61acb7-033a-45dd-93e8-6a5ee077741d/ndk/android-ndk-r27-beta2/build/cmake/android.toolchain.cmake       -DANDROID_ABI=arm64-v8a       -DANDROID_PLATFORM=android-26  ..
make
cd -
mkdir -p out
javac -source 1.8 -target 1.8       -bootclasspath '/opt/android-sdk/platforms/android-35/android.jar'        -d out       MyNativeBridge.java

'/opt/android-sdk/build-tools/34.0.0/d8'  --output out/ out/com/my/scrcpy/binding/*.class

cd out
jar cvf my_server.jar classes.dex
cd -
adb push build/libmy_scrcpy_codec.so out/my_server.jar /data/local/tmp/ 

#adb shell  LD_LIBRARY_PATH=/data/local/tmp CLASSPATH=/data/local/tmp/my_server.jar app_process /data/local/tmp com.my.scrcpy.binding.MyNativeBridge

