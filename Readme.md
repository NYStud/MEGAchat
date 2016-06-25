#  Building karere-native #

## Get the code ##

Checkout the Karere repository:  
`git clone --recursive https://code.developers.mega.co.nz/messenger/karere-native`.  
Note the `--recursive` switch - the repository contains git submodules that need to be checked out as well.

## Toolchains ##

* Android  
Install the android NDK, and create a standalone CLang toolchain, using the `make-standalone-toolchain.sh` scrpt included in the NDK, using following example commandline:  
`./android-ndk-r11b/build/tools/make-standalone-toolchain.sh --toolchain=arm-linux-androideabi-clang --install-dir=/home/user/android-dev/toolchain --stl=libc++ --platform=android-21`
Then, you need to prepare a cross-compile environment for android. For this purpose, you need to use the `/platforms/android/env-android.sh` shell script. Please read the Readme.md in the same directory no how to use that script.

- Notes on building webrtc with Android    
Building of webrtc is supported only on Linux. This is a limitation of Google's webrtc/chromium build system.  
Although webrtc comes with its own copy of the NDK, we are going to use the standard one for building the karere-native code, and let webrtc build with its own NDK. Both compilers are binary compatible. Forcing webrtc to build with an external NDK will not work. For some operations, like assembly code transformations, a host compiler is used, which is the clang version that comes with webrtc. To use an external NDK, we would need to specify explicitly specify the `--sysroot` path of the external NDK, which also gets passed to the clang host compiler, causing errors. 

* iOS  
Building of webrtc and karere is supported only on MacOS. XCode is required.
Then, you need to prepare a cross-compile environment for android. For this purpose, you need to use the `/platforms/ios/env-ios.sh` shell script. Please read the Readme.md in the same directory no how to use that script.


## Build dependencies ##
For Linux and MacOS, you need to install all dependencies using a package manager (the operating system's package manager for Linux, and MacPorts or Homebrew on MacOS). For cross-compiled targets (android, ios), and for Windows, an automated system is provided to download, build and install all dependencies under a specific prefix. See below for details.

### List of dependencies
  - `cmake` and `ccmake` (for a config GUI).  
 - `libevent2.1.x`  
Version 2.0.x will **not** work, you need at least 2.1.x. You may need to build it from source, as 2.1.x is currently considered beta (even though it has critical bugfixes) and system packages at the time of this writing use 2.0.x. For convenience, libevent is added as a git submodule in third-party/libevent, so its latest revision is automatically checked out from the official libevent repository.  
 - `openssl` - Needed by the SDK, webrtc, strophe and Karere itself.  
 - Native WebRTC stack from Chrome. See below for build instructions for it.  
 - The Mega SDK  
Check out the repository, configure the SDK with --enable-chat, and in a minimalistic way - without image libs etc. Only the crypto and HTTP functionality is needed.  
 - `libcrypto++` - Needed by MegaSDK and Karere.
 - `libsodium` - Needed by MegaSDK and Karere.
 - `libcurl` - Needed by MegaSDK and Karere.  
 - `Qt5` - QtCore and QtWidgets required only, needed only for the desktop example app.  

### Automated dependency build system
This is supported only for android, ios, and Windows (for now). The script works in tandem with the cross-compile build environment when building for mobile, so you need that encironemnt set up, as described in the previous sections.
You just need to run `/platforms/setup-deps.sh` script without arguments to get help on how to use it, and then run it with arguments to download, build and install all dependencies.

IMPORTANT for iOS: As Apple does not allow dynamic libraries in iOS apps, you must build all third-party dependencies as static libs, so you need to tell the dependency build system to build everything as static libs.

## Build webrtc ##
Karere provides an autmated system for building webrtc for any of the supported desktop and mobile platforms. This is made very easy by using the `webrtc-build/build-webrtc.sh` script. Run it without arguments to see help on usage. This system is generally an addon to the stock webrtc (actually chromium) build sustem, but it strips it down to download only a few hundred megabytes of source code and tools instead of 10-12GB. It also patches webrtc to fix several issues (use normal openssl instead of its own included boringssl lib, replace macos capturer that uses obsolete API and problematic threading model with modified iOS capturer, etc).

### Verify the build ###
* Cd to build directory
   - non-iOS  
  `cd out/Release|Debug`

  - iOS  
  `cd out/Release-iphoneos|Debug-iphoneos`

* Built executables  

  - Linux and Windows builds    
  Run `peerconnection_server` app to start a signalling server.  
  Run two or more `peerconnection_client` instances and do a call between them via the server.

  - Android  
  The build system generates a test application `WebRTCDemo-debug.apk`. Copy it to a device, install it and run it.

  - Mac and iOS  
  The build generates an AppRTCDemo.app that works with the apprtc web app at `https://apprtc.appspot.com`  

### Using the webrtc stack with CMake ###
Unfortunately the webrtc build does not generate a single lib and config header file (for specific C defines
 to configure the code). Rather, it creates a lot of smaller static libs that can be used only from within the chromium/webrtc build system, which takes care of include paths, linking libs, setting proper defines (and there are quite a few of them). So we either need to build our code within the webrtc/chrome build system, rewrite the build system to something more universal, or do a combination of both. That's what we do currently. Fortunately, the Chrome build system generates a webrtc test app that links in the whole webrtc stack - the app is called `peerconnection_client`. We can get the ninja file generated for this executable and translate it to CMake. The file is located at `trunk/out/Release|Debug/obj/talk/peerconnection_client.ninja`. The webrtc-build/CMakeLists.txt file is basically a translation of this ninja for different platforms, and allows linking webrtc in a higher level CMake file with just a few lines. You do not need to run that cmake script directly, but rather include it from the actual application or library that links to it. This is already done by the rtcModule build system. This will be described in more detail in the webrtc module build procedure.

## Build the Karere codebase, including a test app ##
Change directory to the root of the karere-native checkout  
`mkdir build`  
`cd build`  

### Invoke ccmake config menu

* Desktop OS-es  
If you installed dependencies in non-system prefixes (recommended on MacOS), you need to provide these prefixes to cmake. They will be searched before the system paths, in the order provided:
`ccmake ../examples/qt -DCMAKE_PREFIX_PATH="path/to/prefix1;path/to/prefix2..."`
If you don't need to provide custom prefixes, just issue:
`ccmake ../examples/qt`  

* iOS and Android  
You need to have `env-ios.sh/env-android.sh` sourced in the shell, as explained above. Then, run *ccmake* in cross-compile mode as per the instructions
For example:  
`eval ccmake $CMAKE_XCOMPILE_ARGS ../src`  
This (after configuring via the menu) will cross-compile the Karere SDK for Android or iOS, depending on the cross-compile environment you have set up.  
`eval ccmake -GXcode $CMAKE_XCOMPILE_ARGS ../examples/objc`  
This will not build the iOS app but generate a XCode project linking all dependencies (including webrtc).
You can build that project with XCode.  

### Configure Karere ###
In the ccmake menu that appeared in the previous step, first hit 'c'. The config parameters will get populated.
Then you need to setup the following paths:  
`webrtcRoot` - path to the `src` directory of the webrtc source tree, as you have specified to `build-webrtc.sh`  
`WEBRTC_BUILD_TYPE` - the build mode of the webrtc code, as specified to `build-webrtc.sh`  
This is either `Release` or `Debug` for non-iOS, or `Release-iphoneos` or `Debug-iphoneos` for iOS (to differentiate from simulator builds).  
`CMAKE_BUILD_TYPE` - Set the build mode for the example app, Karere and all its submodules - `Debug` or `Release`.
`optStropheSslLib` - Set to OpenSSL, if not already set.
`optStropheXmlLib` - Can be set to `EXPAT`, `LibXml2` or `Detect` for autodetecting. Tested mostly with expat.
`optStroheNoLibEvent` - make sure it's OFF! If it's ON this means that libevent (including development package) was not found on your system.  
For more info on Strophe build options, check the Readme: third-party/strophe-native/Readme.md  

* iOS  
You must set all options to build anything as shared library to OFF, as Apple doesn't allow dynamic linking on iOS.  

Hit 'c' again to re-configure, and then 'g'. After that ccmake should quit.

* Non-iOS  
In the console, just type  
`make`  
And if all is well, the test app will build.  

* iOS  
After ccmake has quit, you should have an xcode project in the build dir.

## Building the Doxygen documentation ##
From within the build directory of the previous step, provided that you generated a make build, type  
`make doc`  

# Getting familiar with the codebase #
## Basic knlowledge ##
  * The core eventloop mechanism in base/gcm.h, base/gcm.hpp
  * The Promise lib in base/promise.h and example usage for example in /src/test-promise.cpp
  * The setTimeout() and setInterval() timer functions in /src/base/timers.h  
  * The overall client structure in /src/chatClient.h;.cpp
  * The video module public interface in src/rtcModile/IRtcModule.h and related headers
## Video renderer widgets ##
Karere provides platform-specific video renderer widgets for Qt and iOS (probably will work also for MacOS with no or minimal changes).
These widgets are implemented as subclasses of standard widgets. Their code is in src/videoRenderer_xxx.cpp;mm;h. They can be used directly
in the platform's GUI designer by placing the corresponding standard widget and then selecting the VideoRenderer_xxx class name from the menu
to tell the GUI designer to use that subclass. You must include these files in your project, including the headers, to make the subclass visible
to the GUI designer.

## Test Applications ##
Karere provides several example apps, which, among other things, show how the GCM (Gui Call Marshaller) is implemented on the API user's side.
It also shows how to use the video renderer widgets and the IVideoRenderer interface they implement to do video playback.

The test apps are:
* examples/qt - a Qt application.
* examples/objc - an iOS app.

## For application implementors ##
  * The rtctestapp above is the reference app. Build it, study it, experiment with it.
Note theat there is one critical and platform-dependent function that each app that uses Karere must provide, which will be
referenced as `megaPostMessageToGui()`, but it can have any name ,provided that the signature is `extern "C" void(void*)`.
This function is the heart of the message passing mechanism (called the Gui Call Marshaller, or GCM) that Karere relies on.
You must pass a pointer to this function to `services_init()`.  
For more details, read the comments in base/gcm.h, and for reference implementation study rtctestapp/main.cpp
  * IRtcModule, IEventHandler in /src/IRtcModule.h. These are used to initiate rtc calls and receive events.
  * IVideoRenderer in /src/IVideoRenderer.h is used to implement video playback in arbitrary GUI environments.
    Example implementation for Qt is in src/videoRenderer_Qt.h;.cpp.
    The example usage can be seen from the rtctestapp application.

## If Mega API calls are required ##
  * To integrate with the environment, a simple bridge class called MyMegaApi is implemented in /src/sdkApi.h.
    Example usage of it is in /src/chatClient.cpp and in /src/megaCryptoFuncs.cpp. 

## More advanced things that should not be needed but are good to know in order to understand the underlying environment better ##
  * The function call marshalling mechanims in /src/base/gcm.h and /src/base/gcmpp.h. The code is documented in detail.
    The mechanism marshalls lambda calls from a worker thread to the GUI thread. Examples of use of
    marshallCall() can be seen for example in /src/webrtcAdapter.h and in many different places.
    This mechanism should not be directly needed in high-level code that runs in the GUI thread.
