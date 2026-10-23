# nx-packager: the core (core/), the Mac command-line tool (cli/) and the iPhone app (ios/).
#   make         -> build/nxpack (Mac)
#   make ios     -> build/ios/Packager.ipa
CORE    = core/nxcontent.c core/nxzip.c core/nxpack.m
CFLAGS  = -O2 -g -Wall -Wextra -Wno-unused-parameter -fobjc-arc -Icore -ffile-prefix-map=$(CURDIR)=.

build/nxpack: $(CORE) cli/main.m $(wildcard core/*.h)
	@mkdir -p build
	clang $(CFLAGS) -target arm64-apple-macos12.0 -o $@ cli/main.m $(CORE) -framework Foundation -lz

# The iPhone app without Xcode: the Command Line Tools' clang + an iPhoneOS SDK (theos/sdks iPhoneOS16.5.sdk).
SDK     ?= $(HOME)/sdks/iPhoneOS16.5.sdk
TRIPLE  ?= arm64-apple-ios15.0
IOSBUILD ?= build/ios
IOSCC   ?= clang
IOSAPP   = $(IOSBUILD)/Payload/Packager.app
ios: $(IOSBUILD)/Packager.ipa
$(IOSBUILD)/Packager: $(CORE) ios/app.m $(wildcard core/*.h)
	@mkdir -p $(IOSBUILD)
	$(IOSCC) $(CFLAGS) -target $(TRIPLE) -isysroot $(SDK) -o $@ ios/app.m $(CORE) -framework UIKit -framework Foundation \
	  -framework UniformTypeIdentifiers -framework CoreGraphics -framework QuartzCore -lz
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
	DEVELOPER_DIR=$(SIMDEV) $(MAKE) ios IOSBUILD=build/sim TRIPLE=arm64-apple-ios15.0-simulator IOSCC="xcrun clang" \
	  SDK=$$(DEVELOPER_DIR=$(SIMDEV) xcrun --sdk iphonesimulator --show-sdk-path)

clean:
	rm -rf build
.PHONY: clean ios sim
