// Builds a playable IPA from a port pack (.nxport) and the user's own game dump + prod.keys. Everything happens on
// this device; nothing is downloaded or uploaded. Format of a pack: docs/PORT_PACKS.md.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// A pack's nxport.json (nil + *error if the file isn't a port pack this packager understands).
NSDictionary *_Nullable NxPackManifest(NSString *packPath, NSString *_Nullable *_Nullable error);

// What's in a dump: @{@"title_id": @"010051F0207B2000", @"version": @0, @"display_version": @"1.0.0", @"name": ...}.
NSDictionary *_Nullable NxDumpInfo(NSString *dumpPath, NSString *keysPath, NSString *_Nullable *_Nullable error);

// Whether a dump fits a pack's game; nil if it does, else a message for the user.
NSString *_Nullable NxDumpMismatch(NSDictionary *manifest, NSDictionary *dumpInfo);

// The whole build. progress (any thread) gets a short step name and 0..1.
BOOL NxBuildIPA(NSString *packPath, NSString *dumpPath, NSString *keysPath, NSString *outPath,
                void (^_Nullable progress)(NSString *step, double fraction), NSString *_Nullable *_Nullable error);

// Ports with game data (a pack with "data", docs/PORT_PACKS.md): which of the files fit (base game, update, DLC) and
// the space a build needs: @{@"base", @"update", @"aoc" (paths), @"aoc_titles", @"problem" (nil when it can build),
// @"notes" (files that don't fit), @"needed", @"needed_consuming", @"free" (bytes), @"manifest"}.
NSDictionary *_Nullable NxBuildPlan(NSString *packPath, NSArray<NSString *> *dumps, NSString *keysPath, NSString *outDir,
                                    NSString *_Nullable *_Nullable error);

// The whole build of such a port: the IPA, and the game folder dataDir (the pack's data.folder). options:
// @"consume" YES: the game dump is used up while it is read (the .xci's space freed as the build goes, the dumps deleted at
// the end): needs far less free space, the dump is gone afterwards.
BOOL NxBuildPort(NSString *packPath, NSArray<NSString *> *dumps, NSString *keysPath, NSString *ipaPath, NSString *dataDir,
                 NSDictionary *_Nullable options, void (^_Nullable progress)(NSString *step, double fraction),
                 NSString *_Nullable *_Nullable error);

NS_ASSUME_NONNULL_END
