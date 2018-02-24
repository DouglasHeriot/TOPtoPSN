# TOP to PSN - TouchDesigner CPP Plugin

This is a simple TouchDesigner C++ Plugin that takes vertex (xyzw) data encoded in an RGBA 32-bit float texture - and outputs PSN tracker information.

It's only been created for a specific project with special handling for it. This needs more work to be useful in a general case.

* Based on the example C++ implementation of PSN at http://www.posistage.net/posistagenet-specification/
* and the C++ CHOP example code built into TouchDesigner.

Build only tested in Xcode 9.2 on macOS 10.12.

Requires boost (install with Homebrew). Deployment OS only suports the OS its build on or newer, unless you do your own build of Boost with appropriate settings.

Run `git submdoule update --init --recursive` to fetch the required PosiStageNet C++ library.

