# Codec

## Prerequisites
Install [cmake](https://cmake.org/download/) if not already installed on your system. On Mac or Linux this is easiest done by the corresponding package manager on that system (brew, apt, etc)

It is preferred to build with cmake from a separate "build" folder

## Building and installing the code

To create the platform build files (makefile, MSVS code project, or similar), go to the build folder and run (_-S .._ specifies where to find the CMakeLists.txt)
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

To build (in the build folder) for release mode with the build files from above. You can also pass optional `-j` flag to build in parallel and speed up!
```
cmake --build build --config Release -j 16
```

To build the DLL and install the (Release) built library (copy it to the C location)
```
cmake --build build --target SmplCodecDLL --config Release -j 16
cmake --install build --component SmplCodecDLL
```

To run all unit tests either execute ./build/runUnitTests or use ctest (i.e. with verbose flag to get full output)
```
ctest --test-dir build -C Release --verbose
```

To clean the workspace either delete all files generated in build folder or run
```
cmake --build build --target clean
```

### Building for ARM
Download [Android NDK](https://github.com/android/ndk/wiki/Unsupported-Downloads#r21e) r21e version specific to your system and extract it into say `~/deps/android-ndk-r21e`. Note that we can then use this NDK to build Opus+SMPL for most target architectures out there like ARM/Intel 32/64 bit etc.

```
export NDK_HOME="${HOME}/deps/android-ndk-r21e"

# Configure and build for target as ARM 64 bit.
cmake -H. -Bbuild/release/arm64-v8a -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=${NDK_HOME}/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
cmake --build build/release/arm64-v8a -j 16

# Configure and build for target as ARM 32 bit.
cmake -H. -Bbuild/release/armeabi-v7a -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=${NDK_HOME}/build/cmake/android.toolchain.cmake -DANDROID_ABI=armeabi-v7a
cmake --build build/release/armeabi-v7a -j 16
```

## Benchmarking
Please run `bench_encode_decode` to benchmark for different complexity/bitrate/sampling rate settings for both Opus+Silk and Opus+SMPL variants.

### Raspberry PI board (official)
TBD

### Android device (playing around)
Here are steps on how to run any binary on Android device using `adb`. You can get `adb` by installing *Android SDK* locally. For my MAC `adb` is available at `~/Library/Android/sdk/platform-tools/adb`.

Now assuming you have build the binary using the steps above, you can run it like:
```
adb push build/release/arm64-v8a/runUnitTests /data/local/tmp/
adb shell chmod 755 /data/local/tmp/runUnitTests
adb shell /data/local/tmp/runUnitTests
```

## Build Fuzzing target
```bash

cd Codec/C

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_DECODER_FUZZER=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j 16

./build/SmplCodecDecoderFuzzer -max_total_time=300 ../../Meta/codec_fuzzing_corpus
```
