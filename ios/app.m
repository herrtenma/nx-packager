// The Packager on iPhone / iPad: reads a port pack, a game dump and prod.keys from its folder, builds the port's IPA
// (core/nxpack.m) into the same folder, hands it to an installer. One screen, no file picker (pickers don't hand files
// back to apps running inside LiveContainer). The folder: under LiveContainer, LiveContainer's own Documents/Packager
// (Files > LiveContainer > Packager: same process sandbox); installed on its own, the app's Documents/Packager (Files >
// On My iPhone > Packager).
//
// Visual spec (approved; soft glass):
//   background  vertical gradient, light #EEF1F6 -> #E2E7EF, dark #0D1015 -> #161B22
//   layout      content max 560 pt wide, centred, 20 pt side margins; scrolls; header 28 pt below the safe area
//   header      "Packager" SF Pro Rounded 34 bold (#1C1F26 / #F2F4F8), "Build ports from your own games" 15 secondary
//   card        .systemUltraThinMaterial + white overlay a0.65 (light) / a0.08 (dark), radius 22 continuous, 1 pt border
//               white a0.55 / a0.12, shadow #000 a0.08 r16 y6, padding 18, 14 pt apart
//   card row    SF Symbol 22 pt in a 40 pt circle (#0A84FF a0.12), title 17 semibold, detail 14 secondary;
//               ok: checkmark.circle.fill #34C759
//               (spring 0.4 s, damping 0.7); problem: #FF453A 14 pt
//   folder      a card: "Put these files in" + the folder's place in Files (17 semibold), "Check again" capsule 32
//               high, 14 semibold #0A84FF on #0A84FF a0.12
//   build       full width capsule 54 high, #0A84FF, white 17 semibold, disabled a0.4, pressed scale 0.97 (0.1 s)
//   progress    6 pt bar radius 3 (track white a0.3 / a0.15 dark, fill #0A84FF, 0.25 s ease-out), step 13 pt secondary
//   done        a 4th card "Ready": IPA name + size, the pack's after-install note (14), "Install…" (filled) and
//               "Show in Files" (tinted capsule)
//   footer      "Everything happens on this iPhone. Nothing is uploaded." 12 tertiary; links 13 #0A84FF
#import <UIKit/UIKit.h>

#import "nxpack.h"

static UIColor *rgb(uint32_t c, CGFloat a) {
    return [UIColor colorWithRed:(c >> 16 & 255) / 255.0 green:(c >> 8 & 255) / 255.0 blue:(c & 255) / 255.0 alpha:a];
}
static UIColor *dyn(UIColor *light, UIColor *dark) {
    return [UIColor colorWithDynamicProvider:^UIColor *(UITraitCollection *t) {
        return t.userInterfaceStyle == UIUserInterfaceStyleDark ? dark : light;
    }];
}
static UIColor *accent(CGFloat a) { return rgb(0x0A84FF, a); }
static UIFont *rounded(CGFloat size, UIFontWeight w) {
    UIFont *f = [UIFont systemFontOfSize:size weight:w];
    UIFontDescriptor *d = [f.fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded];
    return d ? [UIFont fontWithDescriptor:d size:size] : f;
}
static NSString *docs(void) { return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject; }
// Under LiveContainer an app's Documents is <LiveContainer>/Documents/Data/Application/<uuid>/Documents.
static BOOL in_livecontainer(void) { return [docs() rangeOfString:@"/Documents/Data/Application/"].location != NSNotFound; }
static NSString *folder(void) {
    NSString *d = docs();
    NSRange lc = [d rangeOfString:@"/Documents/Data/Application/"];
    if (lc.location != NSNotFound) d = [[d substringToIndex:lc.location] stringByAppendingPathComponent:@"Documents"];
    d = [d stringByAppendingPathComponent:@"Packager"];
    [NSFileManager.defaultManager createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
    return d;
}
static NSString *folder_in_files(void) { return in_livecontainer() ? @"Files › LiveContainer › Packager" : @"Files › On My iPhone › Packager"; }

// ---- a glass card ---------------------------------------------------------------------------------------------------
@interface NxCard : UIView
@property(nonatomic, readonly) UIView *content;
@end
@implementation NxCard {
    UIVisualEffectView *_blur;
    UIView *_tint;
}
- (instancetype)init {
    self = [super initWithFrame:CGRectZero];
    self.layer.cornerRadius = 22, self.layer.cornerCurve = kCACornerCurveContinuous;
    self.layer.shadowColor = UIColor.blackColor.CGColor, self.layer.shadowOpacity = 0.08f, self.layer.shadowRadius = 16;
    self.layer.shadowOffset = CGSizeMake(0, 6);
    _blur = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemUltraThinMaterial]];
    _blur.layer.cornerRadius = 22, _blur.layer.cornerCurve = kCACornerCurveContinuous, _blur.clipsToBounds = YES;
    _tint = [UIView new];
    _tint.backgroundColor = dyn([UIColor colorWithWhite:1 alpha:0.65], [UIColor colorWithWhite:1 alpha:0.08]);
    _content = [UIView new];
    [self addSubview:_blur], [_blur.contentView addSubview:_tint], [self addSubview:_content];
    self.layer.borderWidth = 1;
    [self traitCollectionDidChange:nil];
    return self;
}
- (void)traitCollectionDidChange:(UITraitCollection *)previous {
    [super traitCollectionDidChange:previous];
    BOOL dark = self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark;
    self.layer.borderColor = [UIColor colorWithWhite:1 alpha:dark ? 0.12 : 0.55].CGColor;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    _blur.frame = _tint.frame = self.bounds;
    _content.frame = CGRectInset(self.bounds, 18, 18);
    self.layer.shadowPath = [UIBezierPath bezierPathWithRoundedRect:self.bounds cornerRadius:22].CGPath;
}
@end

static UIButton *capsule(NSString *title, BOOL filled) {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:title forState:UIControlStateNormal];
    b.titleLabel.font = [UIFont systemFontOfSize:filled ? 17 : 14 weight:UIFontWeightSemibold];
    [b setTitleColor:filled ? UIColor.whiteColor : accent(1) forState:UIControlStateNormal];
    b.backgroundColor = filled ? accent(1) : accent(0.12);
    b.layer.cornerCurve = kCACornerCurveContinuous;
    return b;
}

// ---- one step: icon, title, detail, Choose…, check ------------------------------------------------------------------
@interface NxStep : NxCard
- (instancetype)initWithSymbol:(NSString *)symbol title:(NSString *)title;
- (void)setDetail:(NSString *)detail state:(int)state;   // 0 neutral, 1 ok, -1 problem
@end
@implementation NxStep {
    UIView *_circle;
    UIImageView *_icon, *_check;
    UILabel *_title, *_detail;
    int _state;
}
- (instancetype)initWithSymbol:(NSString *)symbol title:(NSString *)title {
    self = [super init];
    _circle = [UIView new];
    _circle.backgroundColor = accent(0.12), _circle.layer.cornerRadius = 20;
    _icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol withConfiguration:
        [UIImageSymbolConfiguration configurationWithPointSize:22 weight:UIImageSymbolWeightMedium]]];
    _icon.tintColor = accent(1), _icon.contentMode = UIViewContentModeCenter;
    _title = [UILabel new];
    _title.text = title, _title.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold], _title.textColor = UIColor.labelColor;
    _detail = [UILabel new];
    _detail.font = [UIFont systemFontOfSize:14], _detail.numberOfLines = 0;
    _check = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"checkmark.circle.fill" withConfiguration:
        [UIImageSymbolConfiguration configurationWithPointSize:20 weight:UIImageSymbolWeightSemibold]]];
    _check.tintColor = rgb(0x34C759, 1), _check.alpha = 0;
    for (UIView *v in @[_circle, _title, _detail, _check]) [self.content addSubview:v];
    [_circle addSubview:_icon];
    self.isAccessibilityElement = NO;
    return self;
}
- (void)setDetail:(NSString *)detail state:(int)state {
    _detail.text = detail;
    _detail.textColor = state < 0 ? rgb(0xFF453A, 1) : UIColor.secondaryLabelColor;
    BOOL was = _state == 1;
    _state = state;
    if (state == 1 && !was) {
        _check.transform = CGAffineTransformMakeScale(0.4, 0.4);
        [UIView animateWithDuration:0.4 delay:0 usingSpringWithDamping:0.7 initialSpringVelocity:0 options:0
                         animations:^{ self->_check.alpha = 1, self->_check.transform = CGAffineTransformIdentity; } completion:nil];
    } else if (state != 1) _check.alpha = 0;
    [self setNeedsLayout];
    [self.superview setNeedsLayout];
}
- (CGFloat)heightForWidth:(CGFloat)w {
    CGFloat tw = w - 36 - 52;
    return 36 + MAX(40, 22 + 4 + [_detail sizeThatFits:CGSizeMake(tw, 400)].height);
}
- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect b = self.content.bounds;
    _circle.frame = CGRectMake(0, 0, 40, 40);
    _icon.frame = _circle.bounds;
    CGFloat x = 52, tw = b.size.width - x;
    _title.frame = CGRectMake(x, 0, tw - 26, 22);
    _check.frame = CGRectMake(x + MIN([_title sizeThatFits:CGSizeMake(tw, 22)].width, tw - 26) + 6, 1, 20, 20);
    _detail.frame = CGRectMake(x, 26, tw, [_detail sizeThatFits:CGSizeMake(tw, 400)].height);
}
@end

// ---- the screen -----------------------------------------------------------------------------------------------------
@interface NxViewController : UIViewController
@end
@implementation NxViewController {
    CAGradientLayer *_bg;
    UIScrollView *_scroll;
    UILabel *_titleLabel, *_subtitle, *_footer, *_stepLabel;
    NxStep *_packStep, *_dumpStep, *_keysStep;
    NxCard *_where;                       // where the files go
    UILabel *_whereTitle, *_wherePath;
    UIButton *_build, *_install, *_files, *_rescan;
    UIView *_track, *_fill;
    NxCard *_ready;
    UILabel *_readyTitle, *_readyDetail;
    UIStackView *_links;
    NSDictionary *_manifest, *_dumpInfo;
    NSString *_dumpError;
    int _scan;                            // scans in flight are dropped when a newer one starts
    NSString *_packPath, *_dumpPath, *_keysPath, *_ipaPath;
    BOOL _dumpOk, _keysOk, _building;
    double _progress;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    _bg = [CAGradientLayer layer];
    [self.view.layer addSublayer:_bg];
    _scroll = [UIScrollView new];
    _scroll.alwaysBounceVertical = YES;
    [self.view addSubview:_scroll];
    _titleLabel = [UILabel new];
    _titleLabel.text = @"Packager", _titleLabel.font = rounded(34, UIFontWeightBold);
    _titleLabel.textColor = dyn(rgb(0x1C1F26, 1), rgb(0xF2F4F8, 1));
    _subtitle = [UILabel new];
    _subtitle.text = @"Build ports from your own games", _subtitle.font = [UIFont systemFontOfSize:15];
    _subtitle.textColor = UIColor.secondaryLabelColor;
    _packStep = [[NxStep alloc] initWithSymbol:@"shippingbox" title:@"Port pack"];
    _dumpStep = [[NxStep alloc] initWithSymbol:@"gamecontroller" title:@"Game dump"];
    _keysStep = [[NxStep alloc] initWithSymbol:@"key" title:@"prod.keys"];
    _where = [NxCard new];
    _whereTitle = [UILabel new];
    _whereTitle.text = @"Put these three files in", _whereTitle.font = [UIFont systemFontOfSize:14];
    _whereTitle.textColor = UIColor.secondaryLabelColor;
    _wherePath = [UILabel new];
    _wherePath.text = folder_in_files(), _wherePath.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
    _wherePath.numberOfLines = 0;
    _rescan = capsule(@"Check again", NO);
    [_rescan addTarget:self action:@selector(scan) forControlEvents:UIControlEventTouchUpInside];
    for (UIView *v in @[_whereTitle, _wherePath, _rescan]) [_where.content addSubview:v];
    _build = capsule(@"Build", YES);
    [_build addTarget:self action:@selector(buildDown) forControlEvents:UIControlEventTouchDown | UIControlEventTouchDragEnter];
    [_build addTarget:self action:@selector(buildUp) forControlEvents:UIControlEventTouchUpOutside | UIControlEventTouchCancel | UIControlEventTouchDragExit];
    [_build addTarget:self action:@selector(build) forControlEvents:UIControlEventTouchUpInside];
    _track = [UIView new], _fill = [UIView new];
    _track.backgroundColor = dyn([UIColor colorWithWhite:1 alpha:0.3], [UIColor colorWithWhite:1 alpha:0.15]);
    _track.layer.cornerRadius = 3, _track.clipsToBounds = YES, _track.hidden = YES;
    _fill.backgroundColor = accent(1), _fill.layer.cornerRadius = 3;
    [_track addSubview:_fill];
    _stepLabel = [UILabel new];
    _stepLabel.font = [UIFont systemFontOfSize:13], _stepLabel.textColor = UIColor.secondaryLabelColor;
    _stepLabel.textAlignment = NSTextAlignmentCenter, _stepLabel.numberOfLines = 0;
    _ready = [NxCard new];
    _ready.hidden = YES;
    _readyTitle = [UILabel new];
    _readyTitle.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold], _readyTitle.numberOfLines = 0;
    _readyDetail = [UILabel new];
    _readyDetail.font = [UIFont systemFontOfSize:14], _readyDetail.textColor = UIColor.secondaryLabelColor, _readyDetail.numberOfLines = 0;
    _install = capsule(@"Install…", YES);
    _install.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    _files = capsule(@"Show in Files", NO);
    [_install addTarget:self action:@selector(install) forControlEvents:UIControlEventTouchUpInside];
    [_files addTarget:self action:@selector(showInFiles) forControlEvents:UIControlEventTouchUpInside];
    for (UIView *v in @[_readyTitle, _readyDetail, _install, _files]) [_ready.content addSubview:v];
    _footer = [UILabel new];
    _footer.text = @"Everything happens on this iPhone. Nothing is uploaded.";
    _footer.font = [UIFont systemFontOfSize:12], _footer.textColor = UIColor.tertiaryLabelColor;
    _footer.textAlignment = NSTextAlignmentCenter, _footer.numberOfLines = 0;
    _links = [UIStackView new];
    _links.spacing = 6, _links.alignment = UIStackViewAlignmentCenter;
    NSArray *links = @[@[@"Port packs", @"https://github.com/herrtenma/nx-packager#ports"],
                       @[@"Source", @"https://github.com/herrtenma/nx-packager"],
                       @[@"Licenses", @"https://github.com/herrtenma/nx-packager/blob/main/LICENSE"]];
    for (NSUInteger i = 0; i < links.count; i++) {
        if (i) {
            UILabel *dot = [UILabel new];
            dot.text = @"·", dot.font = [UIFont systemFontOfSize:13], dot.textColor = UIColor.tertiaryLabelColor;
            [_links addArrangedSubview:dot];
        }
        UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
        [b setTitle:links[i][0] forState:UIControlStateNormal];
        b.titleLabel.font = [UIFont systemFontOfSize:13], b.tintColor = accent(1);
        NSURL *u = [NSURL URLWithString:links[i][1]];
        [b addAction:[UIAction actionWithHandler:^(UIAction *a) { [UIApplication.sharedApplication openURL:u options:@{} completionHandler:nil]; }]
            forControlEvents:UIControlEventTouchUpInside];
        [_links addArrangedSubview:b];
    }
    for (UIView *v in @[_titleLabel, _subtitle, _where, _packStep, _dumpStep, _keysStep, _build, _track, _stepLabel, _ready, _footer, _links])
        [_scroll addSubview:v];
    for (NxStep *s in @[_packStep, _dumpStep, _keysStep]) {   // cards fade in
        s.alpha = 0;
        [UIView animateWithDuration:0.3 delay:0.05 * [@[_packStep, _dumpStep, _keysStep] indexOfObject:s] options:0
                         animations:^{ s.alpha = 1; } completion:nil];
    }
    [self scan];
    // Back from the Files app: look again.
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(scan) name:UIApplicationDidBecomeActiveNotification object:nil];
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"NXTestBuild"])   // simulator screenshots: -NXTestBuild YES
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ if (self->_build.enabled) [self build]; });
}

// ---- layout -------------------------------------------------------------------------------------------------------
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGRect b = self.view.bounds;
    BOOL dark = self.traitCollection.userInterfaceStyle == UIUserInterfaceStyleDark;
    _bg.colors = dark ? @[(id)rgb(0x0D1015, 1).CGColor, (id)rgb(0x161B22, 1).CGColor] : @[(id)rgb(0xEEF1F6, 1).CGColor, (id)rgb(0xE2E7EF, 1).CGColor];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _bg.frame = b;
    [CATransaction commit];
    _scroll.frame = b;
    UIEdgeInsets sa = self.view.safeAreaInsets;
    CGFloat w = MIN(560, b.size.width - sa.left - sa.right - 40), x = (b.size.width - w) / 2, y = 28;
    _titleLabel.frame = CGRectMake(x, y, w, 41), y += 43;
    _subtitle.frame = CGRectMake(x, y, w, 20), y += 20 + 24;
    {
        CGFloat iw = w - 36, rw = [_rescan sizeThatFits:CGSizeMake(200, 32)].width + 28;
        CGFloat ph = [_wherePath sizeThatFits:CGSizeMake(iw - rw - 12, 200)].height;
        _whereTitle.frame = CGRectMake(0, 0, iw - rw - 12, 18);
        _wherePath.frame = CGRectMake(0, 22, iw - rw - 12, ph);
        _rescan.frame = CGRectMake(iw - rw, 4, rw, 32), _rescan.layer.cornerRadius = 16;
        CGFloat h = MAX(22 + ph, 40) + 36;
        _where.frame = CGRectMake(x, y, w, h), y += h + 14;
    }
    for (NxStep *s in @[_packStep, _dumpStep, _keysStep]) {
        CGFloat h = [s heightForWidth:w];
        s.frame = CGRectMake(x, y, w, h), y += h + 14;
    }
    y += 10;
    _build.frame = CGRectMake(x, y, w, 54), _build.layer.cornerRadius = 27;
    _track.frame = CGRectMake(x + 8, y + 24, w - 16, 6);
    _fill.frame = CGRectMake(0, 0, _track.bounds.size.width * _progress, 6);
    y += 54 + 8;
    CGFloat sh = [_stepLabel sizeThatFits:CGSizeMake(w, 200)].height;
    _stepLabel.frame = CGRectMake(x, y, w, _stepLabel.text.length ? sh : 0), y += (_stepLabel.text.length ? sh : 0) + 14;
    if (!_ready.hidden) {
        CGFloat iw = w - 36, th = [_readyTitle sizeThatFits:CGSizeMake(iw, 200)].height, dh = [_readyDetail sizeThatFits:CGSizeMake(iw, 400)].height;
        _readyTitle.frame = CGRectMake(0, 0, iw, th);
        _readyDetail.frame = CGRectMake(0, th + 6, iw, dh);
        CGFloat by = th + 6 + dh + 14, iwid = [_install sizeThatFits:CGSizeMake(300, 40)].width + 32;
        _install.frame = CGRectMake(0, by, iwid, 40), _install.layer.cornerRadius = 20;
        _files.frame = CGRectMake(iwid + 10, by, [_files sizeThatFits:CGSizeMake(300, 40)].width + 32, 40), _files.layer.cornerRadius = 20;
        CGFloat h = by + 40 + 36;
        _ready.frame = CGRectMake(x, y, w, h), y += h + 14;
    }
    y += 10;
    CGFloat fh = [_footer sizeThatFits:CGSizeMake(w, 200)].height;
    _footer.frame = CGRectMake(x, y, w, fh), y += fh + 4;
    CGSize ls = [_links systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
    _links.frame = CGRectMake((b.size.width - ls.width) / 2, y, ls.width, 30), y += 30 + 20;
    _scroll.contentSize = CGSizeMake(b.size.width, y);
}
- (void)traitCollectionDidChange:(UITraitCollection *)previous {
    [super traitCollectionDidChange:previous];
    [self.view setNeedsLayout];
}

// ---- the folder -----------------------------------------------------------------------------------------------------
// The newest .nxport, prod.keys (or any .keys), and of the .xci / .nsp files the one that fits the pack (else the first,
// with what's wrong with it). Dumps are checked in the background (a few header reads each).
- (void)scan {
    if (_building) return;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *dir = folder();
    NSArray *names = [[fm contentsOfDirectoryAtPath:dir error:nil] sortedArrayUsingSelector:@selector(localizedStandardCompare:)];
    NSString *pack = nil, *keys = nil;
    NSDate *newest = nil;
    NSMutableArray<NSString *> *dumps = [NSMutableArray array];
    for (NSString *n in names) {
        NSString *path = [dir stringByAppendingPathComponent:n], *ext = n.pathExtension.lowercaseString;
        if ([ext isEqual:@"nxport"]) {
            NSDate *d = [fm attributesOfItemAtPath:path error:nil].fileModificationDate;
            if (!newest || [d compare:newest] == NSOrderedDescending) newest = d, pack = path;
        } else if ([ext isEqual:@"keys"] && (!keys || [n isEqual:@"prod.keys"])) keys = path;
        else if ([ext isEqual:@"xci"] || [ext isEqual:@"nsp"]) [dumps addObject:path];
    }
    if (![pack isEqual:_packPath]) {
        NSString *err = nil;
        _packPath = pack, _manifest = pack ? NxPackManifest(pack, &err) : nil;
        if (pack && !_manifest) [_packStep setDetail:[NSString stringWithFormat:@"%@: %@", pack.lastPathComponent, err] state:-1];
    }
    if (!pack) [_packStep setDetail:@"A port's .nxport file, from its releases page." state:0];
    _keysPath = keys;
    if (!keys) [_keysStep setDetail:@"prod.keys, dumped from your own Switch." state:0];
    else [_keysStep setDetail:keys.lastPathComponent state:1];
    int scan = ++_scan;
    NSDictionary *manifest = _manifest;
    if (!dumps.count) {
        _dumpPath = nil, _dumpInfo = nil, _dumpError = nil;
        [_dumpStep setDetail:manifest ? [NSString stringWithFormat:@"Your own dump of %@ %@ (.xci).", manifest[@"game"][@"name"],
                                         manifest[@"game"][@"display_version"]] : @"Your own dump of the game (.xci)." state:0];
        [self refresh];
        return;
    }
    if (!keys) {
        _dumpPath = dumps.firstObject, _dumpInfo = nil, _dumpError = nil;
        [_dumpStep setDetail:[NSString stringWithFormat:@"%@ · add prod.keys to check it", _dumpPath.lastPathComponent] state:0];
        [self refresh];
        return;
    }
    if (!_dumpInfo) [_dumpStep setDetail:@"Checking…" state:0];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *best = nil, *bestErr = nil;
        NSDictionary *bestInfo = nil;
        for (NSString *d in dumps) {
            NSString *err = nil;
            NSDictionary *info = NxDumpInfo(d, keys, &err);
            NSString *mismatch = info && manifest ? NxDumpMismatch(manifest, info) : nil;
            if (info && !mismatch) { best = d, bestInfo = info, bestErr = nil; break; }
            if (!best) best = d, bestInfo = info, bestErr = info ? mismatch : err;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (scan != self->_scan) return;
            self->_dumpPath = best, self->_dumpInfo = bestInfo, self->_dumpError = bestErr;
            [self refresh];
        });
    });
}
- (void)refresh {
    NSDictionary *g = _manifest[@"game"];
    if (_manifest)
        [_packStep setDetail:[NSString stringWithFormat:@"%@ %@ · needs %@ %@", _manifest[@"port_name"], _manifest[@"port_version"],
                              g[@"name"], g[@"display_version"]] state:1];
    BOOL keysBad = _dumpPath && _keysPath && !_dumpInfo && [_dumpError containsString:@"prod.keys"];
    if (keysBad) [_keysStep setDetail:_dumpError state:-1];
    if (_dumpPath && _keysPath && (_dumpInfo || _dumpError)) {
        NSString *found = _dumpInfo ? [NSString stringWithFormat:@"%@ %@ · %@", [_dumpInfo[@"name"] length] ? _dumpInfo[@"name"] : @"Unknown game",
                                       _dumpInfo[@"display_version"], _dumpInfo[@"title_id"]] : _dumpPath.lastPathComponent;
        [_dumpStep setDetail:_dumpError ? [NSString stringWithFormat:@"%@\n%@", found, _dumpError] : found state:_dumpError ? -1 : 1];
    }
    BOOL ready = _manifest && _dumpInfo && !_dumpError && !_building;
    _build.enabled = ready;
    _build.alpha = ready ? 1 : 0.4;
    [_build setTitle:_manifest ? [NSString stringWithFormat:@"Build %@", _manifest[@"app"][@"ipa_name"]] : @"Build" forState:UIControlStateNormal];
    [self.view setNeedsLayout];
}

// ---- building -------------------------------------------------------------------------------------------------------
- (void)buildDown { [UIView animateWithDuration:0.1 animations:^{ self->_build.transform = CGAffineTransformMakeScale(0.97, 0.97); }]; }
- (void)buildUp { [UIView animateWithDuration:0.1 animations:^{ self->_build.transform = CGAffineTransformIdentity; }]; }
- (void)build {
    [self buildUp];
    if (_building) return;
    _building = YES, _progress = 0;
    _ready.hidden = YES;
    _build.hidden = YES, _track.hidden = NO;
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    _rescan.enabled = NO;
    NSString *out = [folder() stringByAppendingPathComponent:_manifest[@"app"][@"ipa_name"]];
    NSString *pack = _packPath, *dump = _dumpPath, *keys = _keysPath;
    __weak NxViewController *ws = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString *err = nil;
        BOOL ok = NxBuildIPA(pack, dump, keys, out, ^(NSString *step, double f) {
            dispatch_async(dispatch_get_main_queue(), ^{ [ws step:step fraction:f]; });
        }, &err);
        dispatch_async(dispatch_get_main_queue(), ^{ [ws built:ok ? out : nil error:err]; });
    });
}
- (void)step:(NSString *)text fraction:(double)f {
    _progress = f;
    _stepLabel.text = [text stringByAppendingString:f < 1 ? @"…" : @""];
    [UIView animateWithDuration:0.25 delay:0 options:UIViewAnimationOptionCurveEaseOut | UIViewAnimationOptionBeginFromCurrentState animations:^{
        self->_fill.frame = CGRectMake(0, 0, self->_track.bounds.size.width * f, 6);
    } completion:nil];
    [self.view setNeedsLayout];
}
- (void)built:(NSString *)ipa error:(NSString *)err {
    _building = NO;
    UIApplication.sharedApplication.idleTimerDisabled = NO;
    _rescan.enabled = YES;
    _track.hidden = YES, _build.hidden = NO;
    if (!ipa) {
        _stepLabel.text = err ?: @"The build failed.";
        _stepLabel.textColor = rgb(0xFF453A, 1);
        [self refresh];
        return;
    }
    _stepLabel.text = nil, _stepLabel.textColor = UIColor.secondaryLabelColor;
    _ipaPath = ipa;
    unsigned long long size = [[NSFileManager.defaultManager attributesOfItemAtPath:ipa error:nil] fileSize];
    _readyTitle.text = [NSString stringWithFormat:@"Ready: %@", ipa.lastPathComponent];
    _readyDetail.text = [NSString stringWithFormat:@"%@, saved in %@. Install it with SideStore, AltStore or LiveContainer.%@%@",
                         [NSByteCountFormatter stringFromByteCount:(long long)size countStyle:NSByteCountFormatterCountStyleFile], folder_in_files(),
                         [_manifest[@"after_install"] length] ? @"\n\n" : @"", _manifest[@"after_install"] ?: @""];
    _ready.hidden = NO, _ready.alpha = 0;
    [UIView animateWithDuration:0.3 animations:^{ self->_ready.alpha = 1; }];
    [self refresh];
    [self.view layoutIfNeeded];
    [_scroll scrollRectToVisible:CGRectInset(_ready.frame, 0, -20) animated:YES];
}
- (void)install {
    if (!_ipaPath) return;
    UIActivityViewController *s = [[UIActivityViewController alloc] initWithActivityItems:@[[NSURL fileURLWithPath:_ipaPath]] applicationActivities:nil];
    s.popoverPresentationController.sourceView = _install;
    s.popoverPresentationController.sourceRect = _install.bounds;
    [self presentViewController:s animated:YES completion:nil];
}
- (void)showInFiles {
    NSString *dir = _ipaPath.stringByDeletingLastPathComponent;
    NSURL *u = [NSURL URLWithString:[@"shareddocuments://" stringByAppendingString:[dir stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet]]];
    [UIApplication.sharedApplication openURL:u options:@{} completionHandler:nil];
}
@end

// ---- app ----------------------------------------------------------------------------------------------------------------
@interface NxSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic) UIWindow *window;
@end
@implementation NxSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    NxViewController *vc = [NxViewController new];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
}
@end
@interface NxAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation NxAppDelegate
@end
int main(int argc, char *argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(NxAppDelegate.class)); }
}
