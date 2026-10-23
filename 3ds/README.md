# New Nintendo 3DS source (experimental)

This folder contains an experimental homebrew build configuration. It is not a validated playable release; test only with your own supported ROM and hardware/software setup.

Controls: Circle Pad steers, A/B accelerate and brake, L/R shift, D-pad selects camera views, Select inserts a coin, Start confirms, and X opens pause/settings. Intended output is 400 by 240 pixels per eye.

A New 3DS homebrew toolchain, libctru, Citro3D (for the optional PICA200 path), and related build dependencies must be installed separately. Configure with CMake and provide -DSDK=/path/to/your/local/sdk-and-generated-data. Paths to SDKs are supplied locally and are not embedded in this repository.

Game code and packed graphics/audio assets are generated locally from a supported ROM. Those generated files are excluded from this repository. The optional PICA200 build also needs locally generated packed source files before it can compile. No ROM, firmware, SDK, capture, or compiled console build is distributed here.