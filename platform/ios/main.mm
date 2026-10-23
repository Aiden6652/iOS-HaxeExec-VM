// SPDX-License-Identifier: MIT
// iOS 宿主：把 x86_64 Windows EXE 跑在 ARM64 上。
//
// 和 hxvm-cli 用的是同一套 core，区别只在这一层：
//   · CADisplayLink 驱动每一帧（来宾 present 一次 = 一帧结束，所以帧是自然对齐的）
//   · CAMetalLayer 负责最终上屏
//   · 音频/视频留给 MobileVLCKit（接口已经开好，见 SetMediaBackend）
//
// 当前状态：能把 EXE 跑起来、能看到来宾输出、能驱动帧循环。
// 绘制命令的转发（把 SDL2/GL 调用转到 Metal）还没做，所以画面是空的。

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <UIKit/UIKit.h>

#include <string>
#include <vector>

#include "hxvm/log.hpp"
#include "hxvm/process.hpp"
#include "hxvm/platform/summary.hpp"
#include "shims/media_backend.hpp"

// 一帧最多给来宾多少条指令。正常情况下来宾会在 present 时提前停下，
// 这个预算只是防止它陷进死循环把主线程堵死。
static const size_t kFrameInstrBudget = 20 * 1000 * 1000;

static hxvm::Process *g_process = nullptr;
static NSUInteger g_frames = 0;
static CFTimeInterval g_lastReport = 0;

// ─────────────────────────────────────────────────────────────────────
@interface HXVMView : UIView
@property(nonatomic, strong) CAMetalLayer *metalLayer;
@end

@implementation HXVMView

+ (Class)layerClass {
    return [CAMetalLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        self.metalLayer = (CAMetalLayer *)self.layer;
        self.metalLayer.device = MTLCreateSystemDefaultDevice();
        self.metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        self.metalLayer.contentsScale = [UIScreen mainScreen].nativeScale;
        self.metalLayer.opaque = YES;
        self.backgroundColor = [UIColor blackColor];
    }
    return self;
}

@end

// ─────────────────────────────────────────────────────────────────────
@interface HXAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) HXVMView *vmView;
@property(nonatomic, strong) CADisplayLink *link;
@property(nonatomic, strong) UILabel *hud;
@end

@implementation HXAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.vmView = [[HXVMView alloc] initWithFrame:self.window.bounds];
    self.vmView.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.window addSubview:self.vmView];

    // 帧率 / 状态小字，调试期有用
    self.hud = [[UILabel alloc] initWithFrame:CGRectMake(8, 28, 320, 44)];
    self.hud.textColor = [UIColor greenColor];
    self.hud.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    self.hud.text = @"hxvm: 启动中";
    [self.window addSubview:self.hud];

    self.window.rootViewController = [self rootController];
    [self.window makeKeyAndVisible];

    [self bootGuest];
    return YES;
}

- (UIViewController *)rootController {
    UIViewController *vc = [[UIViewController alloc] init];
    vc.view = self.vmView;
    return vc;
}

// 找到 Documents/Game 里的 EXE
- (NSString *)findGuestExe {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                        NSUserDomainMask, YES)
                         .firstObject;
    NSString *gameDir = [docs stringByAppendingPathComponent:@"Game"];

    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray *items = [fm contentsOfDirectoryAtPath:gameDir error:nil];
    for (NSString *name in items) {
        if ([[name lowercaseString] hasSuffix:@".exe"]) {
            return [gameDir stringByAppendingPathComponent:name];
        }
    }
    return nil;
}

- (void)bootGuest {
    NSString *exe = [self findGuestExe];
    if (!exe) {
        self.hud.text = @"未找到 EXE：请放到 文件 App / HXVM / Game / 目录下";
        self.hud.textColor = [UIColor redColor];
        NSLog(@"[hxvm] Documents/Game 下没有 .exe");
        return;
    }

    NSString *gameDir = [exe stringByDeletingLastPathComponent];
    NSLog(@"[hxvm] 加载 %@，资源根 %@", exe, gameDir);

    // 宿主后端：来宾一 present 就结束这一帧
    hxvm::RenderBackend rb;
    rb.present = []() {
        if (g_process) g_process->RequestFrameDone();
    };
    hxvm::SetRenderBackend(rb);

    g_process = new hxvm::Process();
    std::vector<std::string> argv;
    if (!g_process->Init([exe UTF8String], [gameDir UTF8String], argv)) {
        self.hud.text = @"加载失败（见控制台日志）";
        self.hud.textColor = [UIColor redColor];
        NSLog(@"[hxvm] 加载失败");
        return;
    }

    NSLog(@"[hxvm] 映像就绪，开始驱动帧循环");

    // 60Hz 驱动：每一帧跑到来宾 present 为止
    self.link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame:)];
    self.link.preferredFramesPerSecond = 60;
    [self.link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
}

- (void)frame:(CADisplayLink *)link {
    if (!g_process || g_process->exited()) {
        [self.link invalidate];
        self.link = nil;
        self.hud.text = @"来宾已退出";
        return;
    }

    const hxvm::u64 t0 = hxvm::plat::MonotonicNanos();
    g_process->RunSlice(kFrameInstrBudget);
    const double ms = (hxvm::plat::MonotonicNanos() - t0) / 1e6;

    g_frames++;
    if (link.timestamp - g_lastReport > 1.0) {
        g_lastReport = link.timestamp;
        self.hud.text = [NSString stringWithFormat:@"帧耗时 %.1f ms / 预算内", ms];
    }
}

@end

// ─────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil,
                                 NSStringFromClass([HXAppDelegate class]));
    }
}
