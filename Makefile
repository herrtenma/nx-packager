# nx-packager: the core (core/), the Mac command-line tool (cli/) and the iPhone app (ios/).
#   make         -> build/nxpack (Mac)
#   make ios     -> build/ios/Packager.ipa
all: build/nxpack
CORE    = core/nxcontent.c core/nxzip.c core/nxtex.c core/nxart.m core/nxpack.m core/nxzstd.c
CFLAGS  = -O2 -g -Wall -Wextra -Wno-unused-parameter -fobjc-arc -Icore -ffile-prefix-map=$(CURDIR)=.
# astc-encoder (third_party, Apache 2.0) + its C entry (core/nxastc.cpp): objects per target
ASTCDIR = third_party/astc-encoder
ASTCSRC = $(wildcard $(ASTCDIR)/astcenc_*.cpp) core/nxastc.cpp
ASTCDEF = -DASTCENC_NEON=1 -DASTCENC_SSE=0 -DASTCENC_AVX=0 -DASTCENC_POPCNT=0 -DASTCENC_F16C=0 -DASTCENC_SVE=0 -DNDEBUG
CXXFLAGS = -std=c++17 -O3 -ffp-contract=fast $(ASTCDEF) -I$(ASTCDIR) -ffile-prefix-map=$(CURDIR)=.
astcobj = $(patsubst %.cpp,$(1)/%.o,$(notdir $(ASTCSRC)))

MACOBJ = $(call astcobj,build/mac)
build/mac/%.o: $(ASTCDIR)/%.cpp
	@mkdir -p build/mac
	clang++ -target arm64-apple-macos12.0 $(CXXFLAGS) -c -o $@ $<
build/mac/nxastc.o: core/nxastc.cpp
	@mkdir -p build/mac
	clang++ -target arm64-apple-macos12.0 $(CXXFLAGS) -c -o $@ $<
build/nxpack: $(CORE) cli/main.m $(wildcard core/*.h) $(MACOBJ)
	@mkdir -p build
	clang $(CFLAGS) -target arm64-apple-macos12.0 -o $@ cli/main.m $(CORE) $(MACOBJ) -framework Foundation -framework CoreGraphics -framework ImageIO -lz -lc++

# The iPhone app without Xcode: the Command Line Tools' clang + an iPhoneOS SDK (theos/sdks iPhoneOS16.5.sdk).
SDK     ?= $(HOME)/sdks/iPhoneOS16.5.sdk
TRIPLE  ?= arm64-apple-ios15.0
IOSBUILD ?= build/ios
IOSCC   ?= clang
IOSCXX  ?= clang++
IOSAPP   = $(IOSBUILD)/Payload/Packager.app
IOSOBJ   = $(call astcobj,$(IOSBUILD)/obj)
ios: $(IOSBUILD)/Packager.ipa
$(IOSBUILD)/obj/%.o: $(ASTCDIR)/%.cpp
	@mkdir -p $(IOSBUILD)/obj
	$(IOSCXX) -target $(TRIPLE) -isysroot $(SDK) $(CXXFLAGS) -c -o $@ $<
$(IOSBUILD)/obj/nxastc.o: core/nxastc.cpp
	@mkdir -p $(IOSBUILD)/obj
	$(IOSCXX) -target $(TRIPLE) -isysroot $(SDK) $(CXXFLAGS) -c -o $@ $<
$(IOSBUILD)/Packager: $(CORE) ios/app.m $(wildcard core/*.h) $(IOSOBJ)
	@mkdir -p $(IOSBUILD)
	$(IOSCC) $(CFLAGS) -target $(TRIPLE) -isysroot $(SDK) -o $@ ios/app.m $(CORE) $(IOSOBJ) -framework UIKit -framework Foundation \
	  -framework UniformTypeIdentifiers -framework CoreGraphics -framework ImageIO -framework QuartzCore -lz -lc++
$(IOSBUILD)/Packager.ipa: $(IOSBUILD)/Packager ios/Info.plist $(wildcard ios/icon/*.png)
	rm -rf $(IOSBUILD)/Payload && mkdir -p $(IOSAPP)
	cp $(IOSBUILD)/Packager ios/Info.plist $(IOSAPP)/
	cp ios/icon/AppIcon60x60*.png ios/icon/AppIcon76x76*.png ios/icon/AppIcon83.5x83.5*.png $(IOSAPP)/
	if [ -n "$(findstring simulator,$(TRIPLE))" ]; then \
	  plutil -replace CFBundleSupportedPlatforms -json '["iPhoneSimulator"]' $(IOSAPP)/Info.plist; \
	  plutil -replace DTPlatformName -string iphonesimulator $(IOSAPP)/Info.plist; fi
	codesign -f -s - $(IOSAPP)
	rm -f $@ && cd $(IOSBUILD) && zip -qry Packager.ipa Payload
	@ls -lh $@
# The simulator (Xcode's SDK: SIMDEV)
SIMDEV  ?= /Volumes/Xcode/Xcode.app/Contents/Developer
sim:
	DEVELOPER_DIR=$(SIMDEV) $(MAKE) ios IOSBUILD=build/sim TRIPLE=arm64-apple-ios15.0-simulator IOSCC="xcrun clang" IOSCXX="xcrun clang++" \
	  SDK=$$(DEVELOPER_DIR=$(SIMDEV) xcrun --sdk iphonesimulator --show-sdk-path)

clean:
	rm -rf build
.PHONY: all clean ios sim
