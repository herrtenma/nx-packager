// nxpack: the packager on a Mac (the same core as the iPhone app).
//   nxpack <port.nxport> <game.xci|.nsp> <prod.keys> <out.ipa>
//   nxpack --info <port.nxport> [<game.xci|.nsp> <prod.keys>]
#import <Foundation/Foundation.h>
#import "nxpack.h"

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *err = nil;
        if (argc >= 3 && !strcmp(argv[1], "--info")) {
            NSDictionary *m = NxPackManifest(@(argv[2]), &err);
            if (!m) { fprintf(stderr, "%s\n", err.UTF8String); return 1; }
            NSDictionary *g = m[@"game"];
            printf("%s %s: needs %s %s (title %s, version %d)\n", [m[@"port_name"] UTF8String], [m[@"port_version"] UTF8String],
                   [g[@"name"] UTF8String], [g[@"display_version"] UTF8String], [g[@"title_id"] UTF8String], [g[@"version"] intValue]);
            if (argc >= 5) {
                NSDictionary *i = NxDumpInfo(@(argv[3]), @(argv[4]), &err);
                if (!i) { fprintf(stderr, "%s\n", err.UTF8String); return 1; }
                NSString *mm = NxDumpMismatch(m, i);
                printf("dump: %s %s (title %s, version %d): %s\n", [i[@"name"] UTF8String], [i[@"display_version"] UTF8String],
                       [i[@"title_id"] UTF8String], [i[@"version"] intValue], mm ? mm.UTF8String : "OK");
            }
            return 0;
        }
        if (argc != 5) {
            fprintf(stderr, "usage: nxpack <port.nxport> <game.xci|.nsp> <prod.keys> <out.ipa>\n       nxpack --info <port.nxport> [<game> <prod.keys>]\n");
            return 2;
        }
        __block NSString *last = nil;
        BOOL ok = NxBuildIPA(@(argv[1]), @(argv[2]), @(argv[3]), @(argv[4]), ^(NSString *step, double f) {
            if (![step isEqual:last]) fprintf(stderr, "%3.0f%% %s\n", f * 100, step.UTF8String), last = step;
        }, &err);
        if (!ok) { fprintf(stderr, "error: %s\n", err.UTF8String); return 1; }
        printf("%s\n", argv[4]);
        return 0;
    }
}
