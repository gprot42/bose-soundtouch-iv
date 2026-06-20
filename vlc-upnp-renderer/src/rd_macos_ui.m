/*
 * rd_macos_ui.m — main-run-loop dispatch for renderer menu updates
 */
#import <CoreFoundation/CoreFoundation.h>
#import <dispatch/dispatch.h>
#import <pthread.h>

#include "../include/rd_macos_ui.h"

void rd_macos_run_on_ui(rd_ui_fn fn, void *userdata)
{
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        fn(userdata);
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

void rd_macos_run_on_ui_sync(rd_ui_fn fn, void *userdata)
{
    if (pthread_main_np() != 0)
    {
        fn(userdata);
        return;
    }

    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        fn(userdata);
        dispatch_semaphore_signal(done);
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}