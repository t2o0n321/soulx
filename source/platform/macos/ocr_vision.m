// Text recognition (OCR) with the macOS Vision framework.
//
// Vision runs entirely on-device (no network, no account) and reads arbitrary fonts, which makes it the general
// fallback for read_text()/read_number() — for a game's own glyphs (orb counts, stylised digits) template matching
// (soulx_image_find) is usually faster and surer, but Vision needs no templates at all. We hand it the script's
// current RGBA screen (or a crop of it) as a CGImage and return the recognised lines joined by '\n'.

#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import <CoreGraphics/CoreGraphics.h>
#include "soulx_mac.h"

// Recognises text in a tightly-packed RGBA8888 image. Writes at most cap-1 bytes + NUL to out (the lines joined by
// '\n', empty when nothing is found). When numbers is true the language model is turned off, which reads digit runs
// more faithfully. Returns 0 on success, -1 on error. Safe to call off the main thread; never blocks on the GUI.
int soulx_ocr_text(const SoulXImage* img, char* out, size_t cap, bool numbers) {
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!img || !img->rgba || img->w <= 0 || img->h <= 0) return -1;

    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        if (!cs) return -1;
        CFDataRef data = CFDataCreate(NULL, img->rgba, (CFIndex)img->w * img->h * 4);
        if (!data) { CGColorSpaceRelease(cs); return -1; }
        CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
        CGImageRef cg = NULL;
        if (dp) {
            cg = CGImageCreate((size_t)img->w, (size_t)img->h, 8, 32, (size_t)img->w * 4, cs,
                               kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL, false,
                               kCGRenderingIntentDefault);
            CGDataProviderRelease(dp);
        }
        CFRelease(data);
        CGColorSpaceRelease(cs);
        if (!cg) return -1;

        VNRecognizeTextRequest* req = [[VNRecognizeTextRequest alloc] initWithCompletionHandler:nil];
        req.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
        req.usesLanguageCorrection = numbers ? NO : YES;

        VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:cg options:@{}];
        NSError* err = nil;
        BOOL ok = [handler performRequests:@[ req ] error:&err];
        CGImageRelease(cg);
        if (!ok) {
            soulx_log(SOULX_LOG_DEBUG, "OCR failed: %s", err.localizedDescription.UTF8String);
            return -1;
        }

        NSMutableArray<NSString*>* lines = [NSMutableArray array];
        for (VNRecognizedTextObservation* obs in req.results) {
            VNRecognizedText* best = [[obs topCandidates:1] firstObject];
            if (best.string.length) [lines addObject:best.string];
        }
        const char* utf8 = [[lines componentsJoinedByString:@"\n"] UTF8String];
        if (utf8) strlcpy(out, utf8, cap);
        return 0;
    }
}
