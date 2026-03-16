# Cross Compilation

Compiling the codec for the host platform itself is relatively straightforward
and is already covered in README.md. This doc gives some pointers on cross
compiling it for other platforms.

## Using Android Toolchain on MAC M1
As an android developer you may already be having the required setup
to cross compile for ARMv7 or AARCH64. These steps are specific to the
MAC M1 + Android toolchain setup.

### Basic Setup
* Download Android NDK from here: https://developer.android.com/ndk/downloads
  * export ANDROID_NDK_ROOT variable to the unzipped NDK directory root.
* Use brew for cmake: `brew install cmake`


Here is how my current setup is.
```bash
# I am on an ARM MAC (M1)
$ uname -a
Darwin jatin-mbp 22.5.0 Darwin Kernel Version 22.5.0: Mon Apr 24 20:52:24 PDT 2023; root:xnu-8796.121.2~5/RELEASE_ARM64_T6000 arm64

# Android NDK setup.
$ echo $ANDROID_NDK_ROOT
/Users/jatin/.waandroid/deps/android-ndk-r21e

# CMake version
$ cmake --version
cmake version 3.25.2
```

### Building for ARMv7
```bash
# Configure.
$ cmake -S . -B build/armv7 -DOPUS_BUILD_PROGRAMS=ON -DOPUS_BUILD_TESTING=ON -DCMAKE_TOOLCHAIN_FILE=ARMv7.cmake -DCMAKE_BUILD_TYPE=Debug

# Build
$ cmake --build build/armv7 --config Debug -j 16
```

### Building for AARCH64
```bash
# Configure.
$ cmake -S . -B build/aarch64 -DOPUS_BUILD_PROGRAMS=ON -DOPUS_BUILD_TESTING=ON -DCMAKE_TOOLCHAIN_FILE=AARCH64.cmake -DCMAKE_BUILD_TYPE=Debug

# Build
$ cmake --build build/aarch64 --config Debug -j 16
```

## Using Android Emulator
I basically used Android Studio's device manager to create a virtual device for each of the the two architectures of interest.
* AARCH64 being the current one has good support and works very well.
* ARMv7 being outdated, required lots of patience for it to work on MAC M1 at least.
  * Check [this](https://stackoverflow.com/questions/30405740/android-studio-how-can-i-make-an-avd-with-arm-instead-of-haxm) post on creating the ARMv7 AVD as it is bit hidden.

Once an emulator is up and running, you can copy over the files and run them. Here are steps for ARMv7, AARCH64 has similar steps.

```bash
# Get the emulator ID.
$ adb devices -l
List of devices attached
emulator-5554          device product:sdk_google_phone_armv7 model:sdk_google_phone_armv7 device:generic transport_id:26

# Copy executables of interest into /data directory. /data is one
# of the places which allows executing random binaries.
$ adb push build/armv7 /data/mlow

# Get into the device and execute
$ adb root
$ adb shell '/data/mlow/runUnitTests --gtest_filter=SmplFiltTest.*'
```
