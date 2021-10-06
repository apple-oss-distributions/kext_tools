/*
 * Copyright (c) 2007 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * FILE: watchvol.c
 * AUTH: Soren Spies (sspies)
 * DATE: 5 March 2006
 * DESC: watch volumes as they come, go, or are changed, fire rebuilds
 *       NOTE: everything in this file should happen on the main thread
 *
 * Here's roughly how it all works.
 * 1. sign up for diskarb notifications
 * 2. generate a data structure for each incoming comprehensible OS volume
 * 2a. set up notifications for all relevant paths on said volume
 *     [notifications <-> structures]
 * (2) uses bootcaches.plist to describe what caches a system needs.
 *     All top-level keys are assumed required (which means the mkext could
 *     get fancier in the future if an old-fashioned mkext was still okay).
 *     If keys exist that can't be understood or don't parse correctly, 
 *     we bail on watching that volume.
 *
 * 3. intelligently respond to notifications
 * 3a. set up a timer to fire so the system has time to settle
 * 3b. upon lazy firing, rebuild caches OR copy files to Apple_Boot
 * 3c. if someone tries to unmount a BootRoot volume, cancel any timer and check
 * 3d. if a locker unlocks happily, cancel any timer and check  (TODO)
 * 3e. if a locker unlocks unhappily, need to force a check of non-caches? (???)
 * 3f. we don't care if the volume is locked; additional kextcaches wait
 * (3d) has the effect that the first kextcache effectively triggers the
 *      second one which copies caches down.  It also allows us ... to be
 *      smart about things like forcing reboots if we booted from staleness.
 *
 * 4. arbitrate kextcache locks
 * 4a. keep a Mach send right to a receive right in the locker
 * 4b. detect crashes via CFMachPortInvalidaton callback
 * 4c. take success information on unlock
 * 4d. if a lock was lost, force a rebuild (XX)?
 *
 * 5. keep structures up to date
 * 5a. clean up when a volume goes away
 * 5b. disappear/appear whenever there's a change
 *
 * 6. reboot stuff: take a big lock; free it only if locker dies
 *
 * given that we read bootcaches.plist, we don't trust anything in it
 * ... but we push the checking off to kextcache, which ensures
 * (via dev_t/safecalls) that it is only operating on a single volume and 
 * not being redirected to other volumes, etc.  We have had Security review.
 *
 * To make sure mkexts have the correct owners, kextd enables owners
 * for the duration of kextcache's locks.
 *  
 */

// system includes
#include <asl.h>
#include <notify.h> // yay notify_monitor_file (well, someday in the header ;)
#include <string.h> // strerror()
#include <sysexits.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>   // " "
#include <stdlib.h> // daemon(3)
#include <signal.h>
#include <unistd.h> // e.g. execv

#include <bless.h>
#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <DiskArbitration/DiskArbitrationPrivate.h>
#include <IOKit/kext/kextmanager_types.h>

#include <IOKit/kext/OSKextPrivate.h>
#ifndef kOSKextLogCacheFlag
    #define kOSKextLogCacheFlag kOSKextLogArchiveFlag
#endif  // no kOSKextLogCacheFlag


// notifyd SPI
extern uint32_t notify_monitor_file(int token, const char *name, int flags);
extern uint32_t notify_get_event(int token, int *ev, char *buf, int *len);

// project includes
#include "kextd_main.h"                 // handleSignal()
#include "kextd_watchvol.h"             // kextd_watch_volumes
#include "bootcaches.h"                 // struct bootCaches
#include "kextd_globals.h"              // gClientUID
#include "kextd_usernotification.h"     // kextd_raise_notification
#include "kextmanager_async.h"          // lock_*_reply()


// constants
#define kAutoUpdateDelay    300
#define kWatchKeyBase       "com.apple.system.kextd.fswatch"
#define kWatchSettleTime    5
#define kMaxUpdateFailures  5   // consecutive failures
#define kMaxUpdateAttempts  25  // reset after failure->success
// XXX after 25 successful updates, touching /S/L/E becomes lame
// 6227955 dictates the failure->success reset metric 
// should instead detect 25 back-to-back attempts

// 6775045 / 5350761: basic MessageTracer logging
#define kMessageTracerDomainKey     "com.apple.message.domain"
#define kMessageTracerSignatureKey  "com.apple.message.signature"
#define kMessageTracerResultKey     "com.apple.message.result"
#define kMTCachesDomain             "com.apple.kext.caches"
#define kMTUpdatedCaches            "updatedCaches"
#define kMTBeginShutdownDelay       "beginShutdownDelay"
#define kMTEndShutdownDelay         "endShutdownDelay"


// the type: struct watchedVol's (struct bootCaches in bootroot.h)
// created/destroyed with volumes coming/going; stored in sFsysWatchDict
// use notify_set_state on our notifications to point to these objects
struct watchedVol {
    // CFUUIDRef volUUID;       // DA id (is the key in sFsysWatchDict)
    CFRunLoopTimerRef delayer;  // non-NULL if something is scheduled
    CFMachPortRef lock;         // send right to locker's port
    CFMutableArrayRef waiters;  // reply ports awaiting this volume
    int updterrs;               // bump when locker reports an error
    int updtattempts;           // # times kextcache launched (w/o err->noerr)
    uint32_t origMntFlags;      // mount flags to restore if owners were off
    Boolean isBootRoot;         // should we try to update helpers?

    CFMutableArrayRef tokens;   // notify(3) tokens
    struct bootCaches *caches;  // parsed version of bootcaches.plist

    // XX should track the PID of any launched kextcache (5736801)
};

// module-wide data
static DASessionRef            sDASession = NULL;      // learn about volumes
static DAApprovalSessionRef    sDAApproval = NULL;     // retain volumes
static CFMachPortRef           sFsysChangedPort = NULL; // let us know
static CFRunLoopSourceRef      sFsysChangedSource = NULL; // on the runloop
static CFMutableDictionaryRef  sFsysWatchDict = NULL;  // disk ids -> wstruct*s
static CFMutableDictionaryRef  sReplyPorts = NULL;  // cfports -> replyPorts
static CFMachPortRef           sRebootLock = NULL;     // sys lock for reboot
static CFMachPortRef           sRebootWaiter = NULL;   // only need one
static CFRunLoopTimerRef       sAutoUpdateDelayer = NULL; // avoid boot / movie
static Boolean                 sBootRootCheckedIn = false; // kc -U checked in?

/* There are two things that could delay on-demand updates (e.g. of mkexts)
 * 1. time / load advisory hasn't given us clearance
 * 2. kextcache -U might be doing its thing if we're booting BootRoot
 * However, if kextcache -U for some reason never calls us (or if kextd
 * restarts sometime after boot), on-demand updates would end up disabled.
 *
 * Thus we assume that kextcache -U is never going to contact us
 * if it doesn't do so within the first five minutes after boot.
 * We use a simple interlock between a delay timer and two routines
 * vol_appeared() & fsys_changed() which trigger on-demand updates.
 *
 * Updates are always performed at shutdown.
 */



// function declarations (kextd_watch_volumes, _stop in watchvol.h)

// ctor/dtors
static struct watchedVol* create_watchedVol(DADiskRef disk);
static void destroy_watchedVol(struct watchedVol *watched);
static CFMachPortRef createWatchedPort(mach_port_t mport, void *ctx);

// volume state
static void vol_appeared(DADiskRef disk, void *ctx);
static void vol_changed(DADiskRef, CFArrayRef keys, void* ctx);
static void vol_disappeared(DADiskRef, void* ctx);
static DADissenterRef is_dadisk_busy(DADiskRef, void *ctx);
static Boolean check_vol_busy(struct watchedVol *watched);

// notification processing delay scheme
static void fsys_changed(CFMachPortRef p, void *msg, CFIndex size, void *info);
static void check_now(CFRunLoopTimerRef timer, void *ctx);    // notify timer cb

// check and act
static Boolean check_rebuild(struct watchedVol*);   // true if launched

// CFMachPort invalidation callback
static void port_died(CFMachPortRef p, void *info);

// helpers for volume and reboot locking routines
static void checkAutoUpdate(CFRunLoopTimerRef t, void *ctx);
static Boolean reconsiderVolumes(mountpoint_t busyVol);
static Boolean checkAllWatched(mountpoint_t busyVol);   // true => work to do

// logging
static void logMTMessage(char *signature, char *result, char *mfmt, ...);

// additional "local" helpers are declared/defined just before use


// utility macros
#define CFRELEASE(x) if (x) { CFRelease(x); x = NULL; }


#if 0
// for testing
#define twrite(msg) write(STDERR_FILENO, msg, sizeof(msg))
static void debug_chld(int signum) __attribute__((unused))
{
    int olderrno = errno;
    int status;
    pid_t childpid;

    if (signum != SIGCHLD)
        twrite("debug_chld not registered for signal\n");
    else
    if ((childpid = waitpid(-1, &status, WNOHANG)) == -1)
        twrite("DEBUG: SIGCHLD received, but no children available?\n");
    else 
    if (!WIFEXITED(status))
        twrite("DEBUG: child quit on signal?\n");
    else
    if (WEXITSTATUS(status))
        twrite("DEBUG: child exited with unhappy status\n");
    else
        twrite("DEBUG: child exited with happy status\n");

    errno = olderrno;
}
#endif

/******************************************************************************
 * kextd_watch_volumes sets everything up (on the current runloop)
 * cleanup of the file-glabal static objects is done in kextd_stop_volwatch()
 *****************************************************************************/
int kextd_watch_volumes(int sourcePriority)
{
    int rval;
    char *errmsg;
    CFRunLoopRef rl;
    const void *keyobjs[2] = { kDADiskDescriptionMediaContentKey,
                              kDADiskDescriptionVolumePathKey };
    CFArrayRef dakeys = NULL;

    rval = EEXIST;
    errmsg = "kextd_watch_volumes() already initialized";
    if (sFsysWatchDict)     goto finish;

    rval = ELAST + 1;
    // the callbacks will want to go digging in here, so set it up first
    errmsg = "couldn't create data structures";
    // sFsysWatchDict keeps track of watched volumes with UUIDs as keys
    sFsysWatchDict = CFDictionaryCreateMutable(nil, 0,
            &kCFTypeDictionaryKeyCallBacks, NULL);  // storing watchedVol*'s
    if (!sFsysWatchDict)    goto finish;
    // We keep two ports for a client; one to for death tracking and one to
    // reply when the time comes.  sReplyPorts maps between the CF wrapper
    // for the death-tracking port and the mach_port_t replyPort.
    sReplyPorts = CFDictionaryCreateMutable(nil, 0,
            &kCFTypeDictionaryKeyCallBacks, NULL);  // storing mach_port_t's
    if (!sReplyPorts)       goto finish;

    errmsg = "error setting up ports and sources";
    rl = CFRunLoopGetCurrent();
    if (!rl)    goto finish;

    // change notifications will eventually come in through this port/source
    sFsysChangedPort = CFMachPortCreate(nil, fsys_changed, NULL, NULL);
    // we have to keep these objects so we can unschedule them later?
    if (!sFsysChangedPort)      goto finish;
    sFsysChangedSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault,
    sFsysChangedPort, sourcePriority++);
    if (!sFsysChangedSource)    goto finish;
    CFRunLoopAddSource(rl, sFsysChangedSource, kCFRunLoopDefaultMode);

    // in general, being on the runloop means we could be called ...
    // and we are thus careful about our ordering.  In practice, however,
    // we're adding to the current runloop, which means nothing can happen
    // until this routine exits (we're on the one and only thread).

    /*
     * XX need to set up a better match dictionary
     * kDADiskDescriptionMediaWritableKey = true
     * kDADiskDescriptionVolumeNetworkKey != true
     */

    // make sure we have a chance to block unmounts
    errmsg = "couldn't set up diskarb sessions";
    sDAApproval = DAApprovalSessionCreate(nil);
    if (!sDAApproval)  goto finish;
    DARegisterDiskUnmountApprovalCallback(sDAApproval,
        kDADiskDescriptionMatchVolumeMountable, is_dadisk_busy, NULL);
    DAApprovalSessionScheduleWithRunLoop(sDAApproval, rl,kCFRunLoopDefaultMode);

    // set up the disk arrival session & callbacks
    // XX need to investigate custom match dictionaries
    sDASession = DASessionCreate(nil);
    if (!sDASession)  goto finish;
    DARegisterDiskAppearedCallback(sDASession,
            kDADiskDescriptionMatchVolumeMountable, vol_appeared, NULL);
    if (!(dakeys = CFArrayCreate(nil, keyobjs, 2, &kCFTypeArrayCallBacks)))
        goto finish;
    DARegisterDiskDescriptionChangedCallback(sDASession,
            kDADiskDescriptionMatchVolumeMountable, dakeys, vol_changed, NULL);
    DARegisterDiskDisappearedCallback(sDASession,
            kDADiskDescriptionMatchVolumeMountable, vol_disappeared, NULL);

    // we're ready to rumble!
    DASessionScheduleWithRunLoop(sDASession, rl, kCFRunLoopDefaultMode);

    // 5519500: schedule a timer to re-enable autobuilds and reconsider volumes
    // should sign up for IOSystemLoadAdvisory() once it can avoid the movie
    sAutoUpdateDelayer = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + kAutoUpdateDelay, 0,
        0, sourcePriority++, checkAutoUpdate, NULL);
#pragma unused(sourcePriority)
    if (!sAutoUpdateDelayer) {
        goto finish;
    }
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), sAutoUpdateDelayer,
                        kCFRunLoopDefaultMode);
    CFRelease(sAutoUpdateDelayer);        // later self-invalidation will free

    // if (signal(SIGCHLD, SIG_IGN) == SIG_ERR)  goto finish;
    // errmsg = "couldn't set debug signal handler";
    // if (signal(SIGCHLD, debug_chld) == SIG_ERR)  goto finish;

    errmsg = NULL;
    rval = 0;

    // volume notifications should start coming in shortly

finish:
    if (rval) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "kextd_watch_volumes: %s.", errmsg);
        if (rval != EEXIST) kextd_stop_volwatch();
    }

    if (dakeys)     CFRelease(dakeys);

    return rval;
}

/******************************************************************************
 * 5519500: if appropriate, checkAutoUpdate() enables event-driven updates
 * and checks all watched volumes to see if they need updating.
 * It doesn't yet use IOSystemLoadAdvisory() since that doesn't avoid movies.
 *****************************************************************************/
static void checkAutoUpdate(CFRunLoopTimerRef timer, void *ctx)
{
    // assert(timer == sAutoUpdateDelayer)
    // one-shot timer self-invalidates
    mountpoint_t ignore;

    // XX We should work towards letting early-boot kextcache -U get a lock
    if (isBootRootActive() && sBootRootCheckedIn == false) {
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                  "Warning: no record of Boot!=Root early boot check");
    } 

    // allow vol_appeared() to proceed with check_rebuild :)
    sAutoUpdateDelayer = NULL;

    // and check all of the watched volumes for updates
    (void)checkAllWatched(ignore);
}

/******************************************************************************
 * kextd_stop_volwatch unregisters from everything and cleans up
 * - called from watch_volumes to handle partial cleanup
 *****************************************************************************/
// to help clear out sFsysWatch
static void free_dict_item(const void* key, const void *val, void *c)
{
    destroy_watchedVol((struct watchedVol*)val);
}

// public entry point to this module
void kextd_stop_volwatch()
{
    CFRunLoopRef rl;

    // runloop cleanup
    rl = CFRunLoopGetCurrent();
    if (rl && sDASession)   DASessionUnscheduleFromRunLoop(sDASession, rl,
            kCFRunLoopDefaultMode);
    if (rl && sDAApproval)  DAApprovalSessionUnscheduleFromRunLoop(sDAApproval,
            rl, kCFRunLoopDefaultMode);

    // use CFRELEASE to nullify cfrefs in case watch_volumes called again
    if (sDASession) {
        DAUnregisterCallback(sDASession, vol_disappeared, NULL);
        DAUnregisterCallback(sDASession, vol_changed, NULL);
        DAUnregisterCallback(sDASession, vol_appeared, NULL);
        CFRELEASE(sDASession);
    }

    if (sDAApproval) {
        DAUnregisterApprovalCallback(sDAApproval, is_dadisk_busy, NULL);
        CFRELEASE(sDAApproval);
    }

    if (rl && sFsysChangedSource)  CFRunLoopRemoveSource(rl, sFsysChangedSource,
            kCFRunLoopDefaultMode);
    CFRELEASE(sFsysChangedSource);
    CFRELEASE(sFsysChangedPort);

    if (sFsysWatchDict) {
        CFDictionaryApplyFunction(sFsysWatchDict, free_dict_item, NULL);
        CFRELEASE(sFsysWatchDict);
    }

    return;
}

/******************************************************************************
* destroy_watchedVol unregisters any notification tokens and frees
* pieces created in create_watchedVol
******************************************************************************/
static void destroy_watchedVol(struct watchedVol *watched)
{
    CFIndex ntokens;
    int token;      
    int errnum;

    // assert that ->delayer, and ->lock have already been cleaned up
    if (watched->tokens) {
        ntokens = CFArrayGetCount(watched->tokens);
        while(ntokens--) { 
            token = (int)(intptr_t)CFArrayGetValueAtIndex(watched->tokens,ntokens);
            // XX should take (hacky) steps to insure token is never zero?
            if (/* !token || */ (errnum = notify_cancel(token)))
                OSKextLog(/* kext */ NULL,
                    kOSKextLogErrorLevel | kOSKextLogIPCFlag,
                    "destroy_watchedVol: error %d canceling notification.", errnum);
        }
        CFRelease(watched->tokens);
    }    
    if (watched->caches)    destroyCaches(watched->caches);
    free(watched);
}

/******************************************************************************
* create_watchedVol calls readCaches and creates watch-specific necessities
******************************************************************************/
static struct watchedVol* create_watchedVol(DADiskRef disk)
{
    struct watchedVol *watched, *rval = NULL;
    char *errmsg;

    errmsg = "allocation error";
    watched = calloc(1, sizeof(*watched));
    if (!watched)   goto finish;

    errmsg = NULL;  // no bootcaches.plist, no problem

    watched->caches = readBootCachesForDADisk(disk);    // logs errors
    if (!watched->caches) {
        goto finish;
    }

    watched->isBootRoot = hasBootRootBoots(watched->caches, NULL, NULL, NULL);

    // get rid of any old bootstamps
    if (!watched->isBootRoot) {
        (void)updateStamps(watched->caches, kBCStampsUnlinkOnly);
    }

    // There will be RPS paths, booters, "misc" paths, and the exts folder.
    // For now, we'll just set the array size to 0 and let it grow.
    errmsg = "allocation error";
    watched->tokens = CFArrayCreateMutable(nil, 0, NULL);
    if (!watched->tokens)   goto finish;

    errmsg = NULL;
    rval = watched;     // success!

finish:
    if (errmsg) {
        if (watched && watched->caches && watched->caches->root[0]) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
                "%s: %s.", watched->caches->root, errmsg);
        } else {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
                "create_watchedVol(): %s.", errmsg);
        }
    }
    if (!rval && watched) {
        destroy_watchedVol(watched);
    }
    
    return rval;
}


// helper: caller must remove port from other structures (e.g. waiters queue)
static int cleanupPort(CFMachPortRef *port)
{
    mach_port_t lport;

    if (sReplyPorts)
        CFDictionaryRemoveValue(sReplyPorts, *port); // stop tracking replyPort
    CFMachPortSetInvalidationCallBack(*port, NULL);  // else port_died called
    lport = CFMachPortGetPort(*port);
    CFRelease(*port);
    *port = NULL;

    return mach_port_deallocate(mach_task_self(), lport);
}

// caller responsibile for setting up the lock and cleaning up the waiter
static int signalWaiter(CFMachPortRef waiter, int status)
{
    int rval = KERN_FAILURE;
    mach_port_t replyPort;

    // extract this client's reply port and reply
    replyPort=(mach_port_t)(intptr_t)CFDictionaryGetValue(sReplyPorts,waiter);
    CFDictionaryRemoveValue(sReplyPorts, waiter);

    if (replyPort != MACH_PORT_NULL) {
        if (waiter == sRebootWaiter) {
            mountpoint_t empty;
            rval = lock_reboot_reply(replyPort, KERN_SUCCESS, empty, status);
        } else {
            rval = lock_volume_reply(replyPort, KERN_SUCCESS, status);
        }
    }

    if (rval) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "signalWaiter failed: %s.",
            safe_mach_error_string(rval));
    }
    return rval;
} 

// sRebootWaiter must be set; sRebootLock is released if held
static void handleRebootHandoff()
{
    mountpoint_t busyVol;

    // make sure everything is clean
    // (checkAllWatched() will initiate updates if needed)
    if (checkAllWatched(busyVol) == EBUSY) {
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                "%s is still busy, delaying reboot.", busyVol);
        goto finish;
    }

    // signal the waiter 
    if (signalWaiter(sRebootWaiter, KERN_SUCCESS) == 0) {
        // on success, make the waiter the locker
        sRebootLock = sRebootWaiter;
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                "%s up to date; unblocking reboot.", busyVol);
        logMTMessage(kMTEndShutdownDelay, "success", "unblocking shutdown");
    }
    sRebootWaiter = NULL;

finish:
    return;
}

// cleans up watched->lock, checks for waiters, assigns the lock, and signals
static void handleWatchedHandoff(struct watchedVol *watched)
{
    CFMachPortRef waiter = NULL;

    // release existing lock
    if (watched->lock)
        cleanupPort(&watched->lock);
 
    // see if we have any waiters
    // XX should have a loop that can try multiple waiters
    if (watched->waiters && CFArrayGetCount(watched->waiters)) {
        waiter = (CFMachPortRef)CFArrayGetValueAtIndex(watched->waiters, 0);

        // move waiter into the pole position and remove from the array
        OSKextLog(NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag,
             "granting lock for %s to waiter %d", watched->caches->root,
             CFMachPortGetPort(waiter));
        watched->lock = waiter;     // context already set to 'watched'
        CFArrayRemoveValueAtIndex(watched->waiters, 0);

        // signal the waiter, cleaning up on failure
        if (signalWaiter(waiter, KERN_SUCCESS)) {
            // XX should loop back to try next waiter if this one failed
            cleanupPort(&watched->lock);    // deallocates former waiter
        }
    }
}

/******************************************************************************
 * vol_appeared checks whether a volume is interesting
 * (note: the first time we see a volume, it's probably not mounted yet)
 * (we rely on vol_changed to call us when the mountpoint actually appears)
 * - signs up for notifications -> creates new entries in our structures
 * - initiates an initial volume check
 *****************************************************************************/
// set up notifications for a single path
static int watch_path(char *path, mach_port_t port, struct watchedVol* watched)
{
    int rval = ELAST + 1;   // cheesy
    char key[PATH_MAX];
    int token = 0;
    int errnum;
    uint64_t state;

    // generate key, register for token, monitor, record pointer in token
    if (strlcpy(key, kWatchKeyBase, PATH_MAX) >= PATH_MAX)  goto finish;
    if (strlcat(key, path, PATH_MAX) >= PATH_MAX)  goto finish;
    if (notify_register_mach_port(key, &port, NOTIFY_REUSE, &token))
        goto finish;
    state = (intptr_t)watched;
    if (notify_set_state(token, state))  goto finish;
    if (notify_monitor_file(token, path, /* flags; 0 means "all" */ 0)) goto finish;

    CFArrayAppendValue(watched->tokens, (void*)(intptr_t)token);

    rval = 0;

finish:
    if (rval && token != -1 && (errnum = notify_cancel(token)))
    OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "watch_path: error %d canceling token.", errnum);

    return rval;
}

#define WATCH(watched, fullp, relpath, fsPort) do { \
        /* MAKEROOTPATH */ \
        COMPILE_TIME_ASSERT(sizeof(fullp) == PATH_MAX); \
        if (strlcpy(fullp, watched->caches->root, PATH_MAX) >= PATH_MAX) \
            goto finish; \
        if (strlcat(fullp, relpath, PATH_MAX) >= PATH_MAX) \
            goto finish; \
        \
        if (watch_path(fullp, fsPort, watched)) \
            goto finish; \
    } while(0)
#define kInstallCommitPath "/private/var/run/installd.commit.pid"
static void vol_appeared(DADiskRef disk, void *launchCtx)
{
    int result = 0; // for now, ignore inability to get basic data (4528851)
    mach_port_t fsPort;
    CFDictionaryRef ddesc = NULL;
    CFURLRef volURL;
    CFBooleanRef traitVal;
    CFUUIDRef volUUID;
    struct watchedVol *watched = NULL;
    Boolean launched = false;

    struct bootCaches *caches;
    int i;
    char path[PATH_MAX];

    OSKextLogCFString(NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag,
                      CFSTR("%s(%@)"), __FUNCTION__, disk);

    // get description so we can see if the disk is writable, etc
    if (!disk)      goto finish;
    ddesc = DADiskCopyDescription(disk);
    if (!ddesc)     goto finish;

    // volUUID is the key in the dictionary (might we already be watching?)
    volUUID = CFDictionaryGetValue(ddesc, kDADiskDescriptionVolumeUUIDKey);
    if (!volUUID || CFGetTypeID(volUUID) != CFUUIDGetTypeID())      goto finish;
    if ((watched = (void *)CFDictionaryGetValue(sFsysWatchDict, volUUID))) {
        // until we can use a real vol_changed() [4620558] to update
        // 'watched', avoid losing critical data for '/'
        if (0 == strcmp(watched->caches->root, "/")) {
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                "'/' re-appeared; refusing to lose existing state (8755513) "
                "at the risk of missing minor changes (4620558)");
            goto finish;
        } else {
            // X vol_changed() has already handled removing it if needed?
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                "WARNING: vol_appeared() removing pre-existing '%s' from"
                " watch table.", watched->caches->root);
            vol_disappeared(disk, NULL);
        }
    }

    // XX when DA calls vol_appeared(), the volume will always be unmounted ??
    volURL = CFDictionaryGetValue(ddesc, kDADiskDescriptionVolumePathKey);
    if (!volURL)        goto finish;    // ignore unmounted volumes

    // 7628429: ignore read-only filesystems (which might be on writable media)
    if (CFURLGetFileSystemRepresentation(volURL,false,(UInt8*)path,PATH_MAX)) {
        struct statfs sfs;
        if (statfs(path, &sfs) == -1)       goto finish;
        if (sfs.f_flags & MNT_RDONLY)       goto finish;    // ignore r-only
        if (sfs.f_flags & MNT_DONTBROWSE)   goto finish;    // respect nobrowse
    } else {
        // couldn't get a path, we don't care about this one
        goto finish;
    }

    // check description traits (XX need custom match dict - 4528851)
    // with fix for 7628429, not clear we need to check this any more
    traitVal = CFDictionaryGetValue(ddesc, kDADiskDescriptionMediaWritableKey);
    if (!traitVal || CFGetTypeID(traitVal) != CFBooleanGetTypeID()) goto finish;
    if (CFEqual(traitVal, kCFBooleanFalse))     goto finish;

    traitVal = CFDictionaryGetValue(ddesc, kDADiskDescriptionVolumeNetworkKey);
    if (!traitVal || CFGetTypeID(traitVal) != CFBooleanGetTypeID()) goto finish;
    if (CFEqual(traitVal, kCFBooleanTrue))      goto finish;


    // does it have a usable bootcaches.plist? (if not, ignore)
    if (!(watched = create_watchedVol(disk)))   goto finish;

    result = -1;    // anything after this is an error
    caches = watched->caches;
    // set up notifications on the change port
    fsPort = CFMachPortGetPort(sFsysChangedPort);
    if (fsPort == MACH_PORT_NULL)               goto finish;

    /* for path in { bootcaches.plist, installd.commit.pid, exts, kernel,
     * locSrcs, rpspaths[], booters, miscpaths[] }
     * rpspaths contains mkext, bootconfig; miscpaths the label file
     * cache paths are relative; WATCH() makes absolute */
    WATCH(watched, path, kBootCachesPath, fsPort);
    WATCH(watched, path, kInstallCommitPath, fsPort);
    WATCH(watched, path, caches->exts, fsPort);
    WATCH(watched, path, caches->kernel, fsPort);
    WATCH(watched, path, caches->locSource, fsPort);
    // XXX commenting out until 9498428 makes watching this file
    // more efficient (and probably replaces locPref with an array).
    // WATCH(watched, path, caches->locPref, fsPort);

    // loop over RPS paths
    for (i = 0; i < caches->nrps; i++) {
        WATCH(watched, path, caches->rpspaths[i].rpath, fsPort);
    }

    if (caches->efibooter.rpath[0]) {
        WATCH(watched, path, caches->efibooter.rpath, fsPort);
    }
    if (caches->ofbooter.rpath[0]) {
        WATCH(watched, path, caches->ofbooter.rpath, fsPort);
    }

    // loop over misc paths
    for (i = 0; i < caches->nmisc; i++) {
        WATCH(watched, path, caches->miscpaths[i].rpath, fsPort);
    }

    // we handled any pre-existing entry for volUUID above
    CFDictionarySetValue(sFsysWatchDict, volUUID, watched);

    // if startup delay hasn't yet passed, skip the usual checks & updates
    if (sAutoUpdateDelayer) {
        OSKextLog(NULL, kOSKextLogDetailLevel | kOSKextLogDirectoryScanFlag,
                "ignoring '%s' until boot is complete", 
                watched->caches->root);
    } else {
        launched = check_rebuild(watched);
    }

    // reconsiderVolume() uses launchCtx to get its return value
    if (launchCtx) {
        Boolean *didLaunch = launchCtx;
        *didLaunch = launched;
    }

    result = 0;           // we made it

finish:
    if (ddesc)   CFRelease(ddesc);

    if (result) {
        if (watched) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
                 "Error setting up notifications on '%s'.",
                watched->caches->root);
            destroy_watchedVol(watched);
        }
    }

    return;
}
#undef WATCH

/******************************************************************************
 * vol_changed updates our structures if the mountpoint changed
 * - includes the initial mount after a device appears 
 * - thus we only call appeared and disappeared as appropriate
 *   
 * Multiple notifications fire multiple times as disks come and go.
 * Depending on how predictable it is, we might be able to get away
 * with just using vol_changed to see when the volume is mounted.
 *****************************************************************************/

// inspired by bless/setboot.c (mostly setit())
static kern_return_t
setEFIBootDevice(CFStringRef bsdName)
{
    kern_return_t rval = KERN_FAILURE;
    char *errmsg;
    char bsdname[DEVMAXPATHSIZE];
    CFStringRef xmlString = NULL;
    io_registry_entry_t optionsNode = IO_OBJECT_NULL;

    errmsg = "unable to create objects";
    if (!CFStringGetFileSystemRepresentation(bsdName, bsdname, DEVMAXPATHSIZE))
        goto finish;
    // switch to ProgressLevel once setting NVRAM is reliable (8401249, etc)
    OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                "setting NVRAM to boot from %s", bsdname);
    if (BLCreateEFIXMLRepresentationForDevice(NULL, bsdname, NULL,&xmlString,0))
        goto finish;

    errmsg = "unable to find /options node";
    optionsNode = IORegistryEntryFromPath(kIOMasterPortDefault,
                                          kIODeviceTreePlane ":/options");
    if (IO_OBJECT_NULL == optionsNode)   goto finish;
    
    errmsg = "error setting efi-boot-device";
    rval = IORegistryEntrySetCFProperty(optionsNode,
                                        CFSTR("efi-boot-device"), xmlString);
    
finish:
    if (rval != KERN_SUCCESS) {
        OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogIPCFlag,
                  "setEFIBootDevice(): %s", errmsg);
    }

    if (optionsNode != IO_OBJECT_NULL)
        IOObjectRelease(optionsNode);
    if (xmlString)      CFRelease(xmlString);
        
    return rval;
}

static void
check_boots_set_nvram(struct watchedVol *watched)
{
    char *volbsdname = watched->caches->bsdname;
    CFArrayRef dparts, bparts;
    Boolean hasBoots;
    Boolean curSDPreferred = true;      // XXX 8011916
    char *s;
    CFStringRef firstPart;

    // logs errors
    hasBoots = hasBootRootBoots(watched->caches, &bparts, &dparts, NULL);
    if (!bparts || !dparts)     goto finish;
    s = (CFArrayGetCount(bparts) == 1) ? "" : "s";

    if (hasBoots) {
        if (!watched->isBootRoot) {
            // gained Apple_Boot partition(s)
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                        "%s now has Apple_Boot partition%s", volbsdname, s);
            watched->isBootRoot = true;
            check_rebuild(watched);

            // and if it was the root volume, re-point NVRAM
            if (0 == strcmp(watched->caches->root, "/") && curSDPreferred) {
                firstPart = CFArrayGetValueAtIndex(bparts, 0);
                (void)setEFIBootDevice(firstPart);
            }
        }
    } else { // !hasBoots
        if (watched->isBootRoot) {
            // lost Apple_Boot partitions
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                        "%s lost its Apple_Boot partition%s", volbsdname, s);

            watched->isBootRoot = false;
            (void)updateStamps(watched->caches, kBCStampsUnlinkOnly);
            // nothing new to rebuild

            // if there was only one data partition, make it the boot partition
            if (0 == strcmp(watched->caches->root, "/") && curSDPreferred &&
                    CFArrayGetCount(dparts) == 1) {
                firstPart = CFArrayGetValueAtIndex(dparts, 0);
                (void)setEFIBootDevice(firstPart);
            }
        }
    }

finish:
    if (dparts)      CFRelease(dparts);
    if (bparts)      CFRelease(bparts);
}


/******************************************************************************
* diskarb sends lots of notifications about random stuff?
* All mounts & unmounts appear to come through here so we might be
* able to stop registering vol_appeared() and vol_disappeared() with DA:
* vol_appeared() -> consider_vol(), vol_disappeared() -> vol_unmounted()
*
* <rdar://problem/4620558> Boot != Root hardening: real vol_changed() so we don't lose state across volume change
* tracks *updating* a watch instead of destroying it & recreating it.
*
* XX for unmount, is this called before or after the unmount(2)?
* XX need to investigate custom match dictionary for change notifications
*****************************************************************************/
static void vol_changed(DADiskRef disk, CFArrayRef keys, void* ctx)
{
    CFIndex i = CFArrayGetCount(keys);
    CFDictionaryRef ddesc = NULL;
    CFUUIDRef volUUID;
    struct watchedVol *watched;

    OSKextLogCFString(NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag,
                      CFSTR("%s(%@, keys[0] = %@)"), __FUNCTION__, disk,
                      CFArrayGetValueAtIndex(keys,0));

    if (!disk)     goto finish;
    ddesc = DADiskCopyDescription(disk);
    if (!ddesc)     goto finish;

    // if it doesn't have a UUID, we can't have or set up a watch for it
    volUUID = CFDictionaryGetValue(ddesc, kDADiskDescriptionVolumeUUIDKey);
    if (!volUUID)  goto finish;

    // only already-mounted OS will be watched
    watched = (void*)CFDictionaryGetValue(sFsysWatchDict, volUUID);

    // walk through the properties that have changed
    while (i--) {
        CFTypeRef key = CFArrayGetValueAtIndex(keys, i);

        // check for mount point changes (coming, going, getting a new name)
        if (CFEqual(key, kDADiskDescriptionVolumePathKey)) {
            // until 4620558 is fixed, mountpoint changes = disappear/appear
            // X we could ignore updates that didn't change the value?
            if (watched)
                vol_disappeared(disk, ctx);  // errors to waiters, etc
            if (CFDictionaryGetValue(ddesc, key))
                vol_appeared(disk, ctx);    // already resists changing '/'
        }

        // check for media changes => Apple_Boot -> NVRAM updates
        else if (CFEqual(key, kDADiskDescriptionMediaContentKey)) {
            if (!watched) {
                // maybe it should be; vol_appeared() checks current state
                vol_appeared(disk, ctx);
            } else {
                // updates isBootRoot, sets NVRAM to a new boot device if needed
                check_boots_set_nvram(watched);
            }
        }

        // something else
        else {
            OSKextLogCFString(NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag,
                CFSTR("vol_changed: %@: uninteresting key"), key);
        }
    }

finish:
    if (ddesc)  CFRelease(ddesc);
}

/******************************************************************************
 * vol_disappeared removes entries from the relevant structures
 * - handles forced removal by invalidating the lock
 * - vol_disappeared() called by vol_changed() for unmounts/changes
 *****************************************************************************/
static void vol_disappeared(DADiskRef disk, void* ctx)
{
    // we used to report errors, but we got weird requests (4528851)
    CFDictionaryRef ddesc = NULL;
    CFUUIDRef volUUID;
    struct watchedVol *watched;

    OSKextLogCFString(NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag,
                      CFSTR("%s(%@)"), __FUNCTION__, disk);

    // if it's not a disk we're watching, we don't need to stop watching it
    if (!disk)      goto finish;
    ddesc = DADiskCopyDescription(disk);
    if (!ddesc)     goto finish;
    volUUID = CFDictionaryGetValue(ddesc, kDADiskDescriptionVolumeUUIDKey);
    if (!volUUID)   goto finish;
    watched = (void*)CFDictionaryGetValue(sFsysWatchDict, volUUID);
    if (!watched)   goto finish;

    if (0 == strcmp(watched->caches->root, "/")) {
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                  "[8755513] vol_disappeared() refusing to stop watching '/'");
        goto finish;
    }

    // if the volume is locked, log a warning
    if (watched->lock) {
        OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                  "WARNING: '%s' is locked; abandoning helper process%s",
                  watched->caches->root, watched->waiters ? "es" : "");
    } else {
        OSKextLog(NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag,
                  "ending volume watch of %s", watched->caches->root);
    }

    // take it off the watch list
    CFDictionaryRemoveValue(sFsysWatchDict, volUUID);

    // and in case some action was in progress
    if (watched->delayer) {
        CFRunLoopTimerInvalidate(watched->delayer);     // refcount->0
        watched->delayer = NULL;
    }
    // see if any lockers are waiting 
    // (off the list of watched vols so no new requests can come in)
    if (watched->waiters) {
        CFIndex i = CFArrayGetCount(watched->waiters);
        CFMachPortRef waiter;

        while(i--) {
            waiter = (CFMachPortRef)CFArrayGetValueAtIndex(watched->waiters,i);
            signalWaiter(waiter, ENOENT);
            cleanupPort(&waiter);
        }
        CFRelease(watched->waiters);    // should remove all elements
    }

    // no need to toggle owners since the volume is gone

    destroy_watchedVol(watched);    // cancels notifications

finish:
    if (ddesc)  CFRelease(ddesc);

    return;
}

/******************************************************************************
 * is_dadisk_busy lets diskarb know if we'd rather nothing changed
 * note: dissenter callback is called when root initiates an unmount,
 * but the result is ignored.
 *****************************************************************************/
static DADissenterRef is_dadisk_busy(DADiskRef disk, void *ctx)
{
    int result = 0;     // ignore weird requests for now (4528851)
    DADissenterRef rval = NULL;
    CFDictionaryRef ddesc = NULL;
    CFUUIDRef volUUID;
    struct watchedVol *watched;

    // XX change to kOSKextLogDetailLevel
    OSKextLogCFString(NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag,
                      CFSTR("%s(%@)"), __FUNCTION__, disk);

    if (!disk)      goto finish;
    ddesc = DADiskCopyDescription(disk);
    if (!ddesc)     goto finish;
    volUUID = CFDictionaryGetValue(ddesc, kDADiskDescriptionVolumeUUIDKey);
    if (!volUUID)   goto finish;

    result = -1;
    watched = (void*)CFDictionaryGetValue(sFsysWatchDict, volUUID);
    if (!watched) {
        // it might have become worth watching while we weren't :?
        vol_appeared(disk, NULL);
        watched = (void*)CFDictionaryGetValue(sFsysWatchDict, volUUID);
    }
    // 5537105 don't prevent unlocked non-BootRoot volumes from unmounting
    if (watched) {
        // once 5736801 is fixed, we'll be able to kill kextcache
        if (watched->lock || (watched->isBootRoot && check_vol_busy(watched))) {
            // since we log this, count it as an error so future successes are logged
            if (watched->updterrs == 0)
                watched->updterrs++;
            if (watched->caches)
                OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogCacheFlag,
                    "%s busy; denying eject (%d tries left)", watched->caches->root,
                    kMaxUpdateAttempts - watched->updtattempts);
            rval = DADissenterCreate(nil, kDAReturnBusy, CFSTR("kextmanager busy"));
            if (!rval)      goto finish;
        }
        // Would it be polite to cancel our watch here?
        // We'd need a chance to start again if the eject is ultimately denied.
        // It would open a window when we weren't watching but it is similar
        // to the window before we start watching?
    }
    
    result = 0;

finish:
    if (result) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "is_dadisk_busy had trouble answering diskarb.");
    }
    // else OSKextLog(/* kext */ NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag, "returning dissenter %p", rval);
    if (ddesc)  CFRelease(ddesc);

    return rval;    // caller releases dissenter if non-null
}

/******************************************************************************
 * check_vol_busy
 * - busy if locked
 * - check_rebuild to check once more (return code indicates if it did anything)
 *****************************************************************************/
static Boolean check_vol_busy(struct watchedVol *watched)
{
    Boolean busy = (watched->lock != NULL);

    if (busy)    goto finish;

    // if not locked and not over error limits, call check_rebuild
    if (watched->updterrs < kMaxUpdateFailures &&
            watched->updtattempts < kMaxUpdateAttempts) {
        busy = check_rebuild(watched);
    } else {
        // over limits; log an error
        OSKextLog(/* kext */ NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
            "%s: giving up; kextcache hit max %s", watched->caches->root,
            watched->updterrs >= kMaxUpdateFailures ? "failures" : "update attempts");
    }

finish:
    return busy;
}


/******************************************************************************
 * fsys_changed gets the mach messages from notifyd
 * - schedule a timer (urgency detected elsewhere calls direct, canceling timer)
 *****************************************************************************/
static void fsys_changed(CFMachPortRef p, void *m, CFIndex size, void *info)
{
    uint64_t nstate;
    struct watchedVol *watched;
    int notify_status = NOTIFY_STATUS_OK;
    int token;
    FILE *f;
    mach_msg_empty_rcv_t *msg = (mach_msg_empty_rcv_t*)m;

    char path[PATH_MAX];
    struct stat sb;

    // msg_id==token -> notify_get_state() -> watchedVol*
    // XX if (token == 0, perhaps a force-rebuild message?)
    token = msg->header.msgh_id;
    notify_status = notify_get_state(token, &nstate);
    if (NOTIFY_STATUS_OK != notify_status) {
        OSKextLogCFString(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            CFSTR("notify_get_state() failed for token %d: status %d."),
            token, notify_status);
        goto finish;
    }

    // XX should call notify_get_event() here to consume events?
    // how to know when this notification's events have been consumed?
    // filter out useless (generally spurious) events (esp on unmount :P)
    watched = (struct watchedVol*)(intptr_t)nstate;
    if (!watched) {
        OSKextLogCFString(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            CFSTR("NULL entry for notify token %d."),
            token);
        goto finish;
    }

    // ignore unwatched volumes (notification should have been canceled?)
    if (!CFDictionaryGetCountOfValue(sFsysWatchDict, watched)) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "Invalid token/volume: %d, %p.", token, watched);
        goto finish;
    }

    // The goal is to schedule a timer to do the update later, but if
    // the installer is installing, then we'll ignore this change.
    // Changes to the .pid file will call us again.
    if (strlcpy(path,watched->caches->root,sizeof(path)) >= sizeof(path))
        goto finish;
    if (strlcat(path, kInstallCommitPath, sizeof(path)) >= sizeof(path))
        goto finish;
    if ((f = fopen(path, "r"))) {
        pid_t pid;
        int nmatched = fscanf(f, "%d", &pid);
        fclose(f);
        if (nmatched == 1) {
            if (0 == kill(pid, 0)) {
                OSKextLog(NULL,kOSKextLogDetailLevel|kOSKextLogFileAccessFlag,
                          "%s: installer active, ignoring changes",
                          watched->caches->root);
                goto finish;
            }
        }
    }

    // Check for new bootcaches.plist content
    if (strlcpy(path,watched->caches->root,sizeof(path)) >= sizeof(path))
        goto finish;
    if (strlcat(path, kBootCachesPath, sizeof(path)) >= sizeof(path))
        goto finish;
    if (stat(path, &sb) != 0) {
        if (errno != ENOENT) {
            OSKextLogCFString(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
                CFSTR("stat failed for %s: %s."), path, strerror(errno));
        }
        // no longer a bootcaches.plist, bail (w/o canceling watch)
        goto finish;
    }

    if (watched->caches->bcTime.tv_sec != sb.st_mtimespec.tv_sec ||
        watched->caches->bcTime.tv_nsec !=sb.st_mtimespec.tv_nsec) {

        // change is afoot in bootcaches.plist
        char * volRoot = watched->caches->root;
        DADiskRef disk = NULL;

        OSKextLog(/* kext */ NULL,
            kOSKextLogBasicLevel | kOSKextLogFileAccessFlag,
            "%s: bootcaches.plist changed.", volRoot);

        // invalidate current watched information
        disk = createDiskForMount(sDASession, volRoot);
        if (!disk) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogFileAccessFlag | kOSKextLogIPCFlag,
                 "Unable to create DADisk.");
            goto finish;
        }

        // X: doesn't work for '/', but users' bootcaches.plist shouldn't change
        // except from the InstallOS with their '/' as /Volumes/Mac HD. :)
        vol_disappeared(disk, NULL);    // destroys watched (incl sb)
        vol_appeared(disk, NULL);       // checks if rebuild needed
        CFRelease(disk);

    } else if (sAutoUpdateDelayer == NULL) {
        // check whether the startup delay has passed
        // (XX could let 'touch' work before the startup delay has passed?)
        // and prepare to call check_now in a few seconds
        CFRunLoopTimerContext tc = { 0, watched, NULL, NULL, NULL };
        CFAbsoluteTime firetime = CFAbsoluteTimeGetCurrent() + kWatchSettleTime;

        // cancel any existing timer
        if (watched->delayer) {
            CFRunLoopTimerInvalidate(watched->delayer);
        }

        watched->delayer=CFRunLoopTimerCreate(nil,firetime,0,0,0,check_now,&tc);
        if (!watched->delayer) {
            OSKextLogCFString(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                 CFSTR("fsys_changed() unable to create timer for watched volume."));
            goto finish;
        }

        CFRunLoopAddTimer(CFRunLoopGetCurrent(), watched->delayer,
            kCFRunLoopDefaultMode);
        CFRelease(watched->delayer);  // later auto-invalidation will free
    }

finish:
    return;
}

/******************************************************************************
 * check_now, called after the timer expires, calls check_rebuild() 
 * It does not look at updterrs because if something changed, we're willing
 * to look at it again.
 *****************************************************************************/
void check_now(CFRunLoopTimerRef timer, void *info)
{
    struct watchedVol *watched = (struct watchedVol*)info;
    // OSKextLog(/* kext */ NULL, kOSKextLogDebugLevel | kOSKextLogGeneralFlag, "DEBUG: check_now(%p): entry", info);

    // is the volume still being watched?
    if (watched && CFDictionaryGetCountOfValue(sFsysWatchDict, watched)) {
        watched->delayer = NULL;        // timer is no longer pending
        (void)check_rebuild(watched);   // don't care what it did
    } else {
        OSKextLog(/* kext */ NULL,
            kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
            "%p's timer fired when it should have been Invalid", watched);
    }
}

/******************************************************************************
 * check_rebuild uses needUpdates() to stat everything -> rebuilds as necessary
 * - kextcache -u used to do all the work (rebuild mkext, boot's etc)
 *
 * XX if kextcache is broken (e.g. a copy of 'false'), updterrs is never
 * incremented and an an infinite reboot stall could result.  updtattempts
 * caps the total number of automatic update attempts.  The locking mechanism
 * serializes (and effectively throttles) the kextcache processes which should
 * prevent a transient failure condition from preventing multiple meaningful
 * attempts to update the volume.
 *****************************************************************************/
static Boolean check_rebuild(struct watchedVol *watched)
{
    Boolean launched = false;
    Boolean rebuild = false;

    // if we came in some other way and there's a timer pending, cancel it
    if (watched->delayer) {  
        CFRunLoopTimerInvalidate(watched->delayer);  // runloop holds last ref
        watched->delayer = NULL;
    }

    // make sure this volume isn't out of control with updates
    if (watched->updtattempts > kMaxUpdateAttempts) {
        OSKextLog(/* kext */ NULL, kOSKextLogWarningLevel | kOSKextLogIPCFlag,
            "%s: kextcache has had enough tries; not launching any more",
            watched->caches->root);
        goto finish;
    }


    // stat stuff to see if a rebuild is needed
    // (it was 'goto' or ever-deeper nesting)
    if ((rebuild = check_kext_boot_cache_file(watched->caches,
        watched->caches->kext_boot_cache_file->rpath,
        watched->caches->kernel))) {

        goto dorebuild;
    }

    // updates isBootRoot and modifies NVRAM if needed
    check_boots_set_nvram(watched);

    if (watched->isBootRoot) {
        rebuild = check_csfde(watched->caches) ||
                  check_loccache(watched->caches) ||
                  needUpdates(watched->caches, NULL, NULL, NULL,
                        kOSKextLogProgressLevel | kOSKextLogFileAccessFlag);

        if (rebuild)
            goto dorebuild;
    }

dorebuild:
    if (rebuild) {
        if (launch_rebuild_all(watched->caches->root, false, false) > 0) {
            launched = true;
            watched->updtattempts++;
        } else {
            watched->updterrs++;
            OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogIPCFlag,
                "Error launching kextcache -u.");
        }
    }

    if (0 == strcmp(watched->caches->root, "/") &&
            plistCachesNeedRebuild(gKernelArchInfo)) {

        handleSignal(SIGHUP);
        // xxx - why are you not just calling rescanExtensions()?
        // X someday SIGHUP may call back to rebuild_caches() to force update
    }

finish:
    return launched;
}


// ---- locking services (prototyped via MiG and kextmanager[_mig].defs) ----

/******************************************************************************
 * kextmanager_lock_reboot ensures "all clean" (used by shutdown(8), reboot(8))
 *****************************************************************************/
// iterator helper locking for locked or should-be-locked volumes
static void check_locked(const void *key, const void *val, void *ctx)
{
    struct watchedVol *watched = (struct watchedVol*)val;
    // pointer to the mountpoint_t at the other end
    char *busyVol = ctx;

    // report this one if:
    // it's already locked or if it needs a rebuild
    if (check_vol_busy(watched)) {
        strlcpy(busyVol, watched->caches->root, sizeof(mountpoint_t));
    }
}

// create a CFMachPort with invalidation -> port_died
static CFMachPortRef
createWatchedPort(mach_port_t mport, void *ctx)
{
    CFMachPortRef rval = NULL;
    int result = ELAST + 1;
    CFRunLoopSourceRef invalidator;
    CFMachPortContext mp_ctx = { 0, ctx, 0, };
    CFRunLoopRef rl = CFRunLoopGetCurrent();

    if (!(rval = CFMachPortCreateWithPort(nil, mport, NULL, &mp_ctx, false)))
        goto finish;
    invalidator = CFMachPortCreateRunLoopSource(nil, rval, 0);
    if (!invalidator)       goto finish;
    CFMachPortSetInvalidationCallBack(rval, port_died);
    CFRunLoopAddSource(rl, invalidator, kCFRunLoopDefaultMode);
    CFRelease(invalidator); // owned by the runloop now

    result = 0;

finish:
    if (result && rval) {
        CFRelease(rval);
        rval = NULL;
    }

    return rval;
}

static Boolean
checkAllWatched(mountpoint_t busyVol)
{
    int result;

    // if we've contacted diskarb, scan the dictionary for locked items
    busyVol[0] = '\0';
    if (sFsysWatchDict) {
        CFDictionaryApplyFunction(sFsysWatchDict, check_locked, busyVol);
    }
    if (busyVol[0] == '\0') {
        result = 0;     // you got it!
    } else {
        // busyVol (at least) was locked, try again later
        result = EBUSY;
    }

    return result;
}

kern_return_t _kextmanager_lock_reboot(mach_port_t p, mach_port_t replyPort,
    mach_port_t client, int waitForLock, mountpoint_t busyVol, int *busyStatus)
{
    kern_return_t rval = KERN_FAILURE;
    int result = ELAST + 1;
    // OSKextLog(/* kext */ NULL, kOSKextLogDebugLevel | kOSKextLogGeneralFlag, "DEBUG: _lock_reboot(..%d/%d..)...", client, replyPort);

    if (!busyStatus) {
        result = EINVAL;
        rval = KERN_SUCCESS;    // for MiG
        goto finish;
    }
    
    if (gClientUID != 0) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "Non-root doesn't need to lock for reboot.");
        result = EPERM;
        rval = KERN_SUCCESS;    // for MiG
        goto finish;
    }
    
    // shutdown/reboot proceed on result == EALREADY
    if (sRebootLock) {
        result = EALREADY;
        rval = KERN_SUCCESS;    // for MiG
        OSKextLog(/* kext */ NULL,
            kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                "Warning: Reboot lock request while reboot in progress.");
        goto finish;
    }

    // check all the volumes we are watching and 
    // if any new volumes have become eligible
    if (checkAllWatched(busyVol) || reconsiderVolumes(busyVol)) {
        result = EBUSY;
        rval = KERN_SUCCESS;    // for MiG
    } else {
        // great, this guy gets to take the uber reboot lock
        if (!(sRebootLock = createWatchedPort(client, &sRebootLock)))
            goto finish;
        result = 0;             // success
        rval = KERN_SUCCESS;    // for MiG
    }

    // should we reply now or later?
    if (waitForLock && result == EBUSY && sRebootWaiter == NULL) {
        // client will block until we reply with lock or failure
        // [&sRebootLock is context for all interested in the reboot lock]
        if (!(sRebootWaiter = createWatchedPort(client, &sRebootLock)))
            goto finish;

        // stash reply port so we can get it later (X someday dual-use port?)
        CFDictionarySetValue(sReplyPorts, sRebootWaiter, (void*)(intptr_t)replyPort);
        rval = MIG_NO_REPLY;    // for MiG; no result
    } else {
        // we reply to the client if: a. failure other than EBUSY,
        // b. the client won't wait, or c. we've no way to track him.
        rval = KERN_SUCCESS;        // MiG will return to the caller
    }

finish:
    if (rval == KERN_SUCCESS) {
        if (busyStatus) *busyStatus = result;
    } else if (rval != MIG_NO_REPLY) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "Error %d locking for reboot.", rval);
    }

    // pop up a dialog if reboot is going to stall
    if (result == EBUSY && waitForLock) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogWarningLevel | kOSKextLogFileAccessFlag | kOSKextLogIPCFlag,
            "'%s' updating, delaying reboot.", busyVol);

        // 6775045 / 5350761: basic MessageTracer logging
        logMTMessage(kMTBeginShutdownDelay, "failure", 
                "kext caches need update at shutdown; delaying");
    }

    return rval;
}

/******************************************************************************
 * _kextmanager_lock_volume locks volumes for kextcache
 * - vol_uuid is in CFUUIDBytes
 *****************************************************************************/
kern_return_t _kextmanager_lock_volume(mach_port_t p, mach_port_t replyPort,
    mach_port_t client, uuid_t vol_uuid, int waitForLock, int *lockStatus)
{
    kern_return_t rval = KERN_FAILURE;
    int result;
    CFUUIDBytes uuidBytes;
    CFUUIDRef volUUID;
    struct watchedVol *watched = NULL;
    struct statfs sfs;
    Boolean createdLock = false;

    OSKextLog(/* kext */ NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag, 
              "%s(..%d..)...", __FUNCTION__, client);

    if (!lockStatus) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "kextmanager_lock_volume requires non-NULL lockStatus.");
        return KERN_INVALID_ARGUMENT;    // just return
    }

    if (gClientUID != 0 /*watched->fsinfo->f_owner ?*/) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "Non-root doesn't need to lock or unlock volumes.");
        result = EPERM;
        rval = KERN_SUCCESS;    // for MiG
        goto finish;
    }

    // if BootRoot, first lock should be kextcache -U checking in
    if (isBootRootActive() && sBootRootCheckedIn == false) {
        // record the check-in so checkAutoUpdate() can proceed
        sBootRootCheckedIn = true;

        // XX could eliminate 5642331 race by touch'ing /S/L/E here

        // if checkAutoUpdate waited for sBootRootCheckedIn, we'd kick it here

        // if kextd restarted, this might not be kextcache -U
        // so we fall through to granting the lock
    }

    // still initializing, sorry (XX someday could allow client to wait)
    // 5519500: init no longer delayed so only kextcache -U might hit this
    if (!sFsysWatchDict) {
        result = EAGAIN;
        rval = KERN_SUCCESS;    // for MiG
        goto finish;
    }

    // rebooting -> no more update locks!
    if (sRebootLock) {
        OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogIPCFlag,
                  "reboot in progress -> denying volume lock request");
        result = ENOLCK;
        rval = KERN_SUCCESS;    // for MiG
        goto finish;
    }

    memcpy(&uuidBytes.byte0, vol_uuid, sizeof(uuid_t));
    volUUID = CFUUIDCreateFromUUIDBytes(nil, uuidBytes);
    if (!volUUID) {
        result = ENOMEM;
        goto finish;
    }
    watched = (void*)CFDictionaryGetValue(sFsysWatchDict, volUUID);
    // would release volUUID here except we want to log it ...
    // if kextd isn't watching, locking is less critical (->Basic)
    if (!watched) {
        OSKextLogCFString(NULL, kOSKextLogBasicLevel | kOSKextLogIPCFlag,
                  CFSTR("not watching %@ -> no volume lock to grant"), volUUID);
        CFRelease(volUUID);
        result = ENOENT;
        rval = KERN_SUCCESS;    // for MiG
        goto finish;
    } else {    // clarify no double release
        CFRelease(volUUID);
    }

    // if not locked, grant the lock
    if (watched->lock == NULL) {
        // create lock
        if (!(watched->lock = createWatchedPort(client, watched))) {
            rval = KERN_FAILURE;        // also the default above
            goto finish;
        }
        createdLock = true;
        // try to enable owners if not currently honored (XX ignores failure)
        if (statfs(watched->caches->root, &sfs) == 0) {
            uint32_t mntgoal = sfs.f_flags;
            mntgoal &= ~(MNT_IGNORE_OWNERSHIP);
            if (sfs.f_flags != mntgoal) {
                (void)updateMount(watched->caches->root, mntgoal);  // logs
                watched->origMntFlags = sfs.f_flags;
            }
        }
        OSKextLog(NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag,
             "granting lock for %s to %d", watched->caches->root,client);
        result = 0;             // success; lock granted
        rval = KERN_SUCCESS;    // for MiG
    } else {
        // lock can't be granted; let the client wait if willing
        if (waitForLock) {
            rval = MIG_NO_REPLY;    // for MiG; no result
        } else {
            result = EBUSY;         // for client
            rval = KERN_SUCCESS;    // for MiG
        }
    }

    // if we're not replying yet, add client to the wait queue
    if (rval == MIG_NO_REPLY) {
        CFMachPortRef waiter;

        // create waiter array (of CFMachPortRefs) if needed
        if (!watched->waiters) {
            watched->waiters=CFArrayCreateMutable(0,1,&kCFTypeArrayCallBacks);
            if (!watched->waiters) {
                rval = KERN_FAILURE;
                goto finish;
            }
        }

        // create waiter and insert into array
        if (!(waiter = createWatchedPort(client, watched))) {
            rval = KERN_FAILURE;
            goto finish;
        }

        // store waiter, replyPort; cleanupPort releases waiter create above
        CFArrayAppendValue(watched->waiters, waiter);
        CFDictionarySetValue(sReplyPorts, waiter, (void*)(intptr_t)replyPort);

        // success: rval remains MIG_NO_REPLY
    }


finish:
    // circa 8679674, creating the lock => rval = success
    // but this ensures future code won't destroy an existing lock on error
    if (rval != KERN_SUCCESS && createdLock && watched->lock) {
        cleanupPort(&watched->lock);
    }

    if (rval == KERN_SUCCESS) {
        *lockStatus = result;
    } else if (rval == MIG_NO_REPLY) {
        // not replying yet
    } else if (watched) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "Error %d locking '%s'.", rval, watched->caches->root);
    } else {
        OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "Error %d attempting to lock an un-watched volume.", rval);
    }

    return rval;
}

/******************************************************************************
 * _kextmanager_unlock_volume unlocks for clients (i.e. kextcache)
 *****************************************************************************/
kern_return_t _kextmanager_unlock_volume(mach_port_t p, mach_port_t client,
    uuid_t vol_uuid, int exitstatus)
{
    kern_return_t rval = KERN_FAILURE;
    CFUUIDRef volUUID = NULL;
    struct watchedVol *watched = NULL;
    CFUUIDBytes uuidBytes;
    // OSKextLog(/* kext */ NULL, kOSKextLogDebugLevel | kOSKextLogGeneralFlag, "DEBUG: _kextmanager_unlock_volume()...");

    // since we don't need the extra send right added by MiG (XX why?)
    if (mach_port_deallocate(mach_task_self(), client))  goto finish;

    if (gClientUID != 0 /*watched->fsinfo->f_owner ?*/) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "Non-root doesn't need to lock or unlock volumes.");
        rval = KERN_SUCCESS;
        goto finish;
    }

    // make sure we're set up
    if (!sFsysWatchDict)    goto finish;

    memcpy(&uuidBytes.byte0, vol_uuid, sizeof(uuid_t));
    volUUID = CFUUIDCreateFromUUIDBytes(nil, uuidBytes);
    if (!volUUID)           goto finish;
    watched = (void*)CFDictionaryGetValue(sFsysWatchDict, volUUID);
    if (!watched)           goto finish;

    if (!watched->lock) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "'%s' isn't locked.", watched->caches->root);
        goto finish;
    }
    
    if (client != CFMachPortGetPort(watched->lock)) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "%d didn't lock '%s'.", client, watched->caches->root);
        goto finish;
    }

    // update error accounting
    if (exitstatus == EX_OK) {
        logMTMessage(kMTUpdatedCaches, "success",
                     "kextcache updated kernel boot caches");
        if (watched->updterrs > 0) {
            // previous error logs -> put a reassuring message in the log
            OSKextLog(NULL, kOSKextLogDebugLevel | kOSKextLogCacheFlag,
                "kextcache (%d) succeeded with '%s'.",
                client, watched->caches->root);
            watched->updterrs = 0;
            watched->updtattempts = 0;
        }
    } else {
        // not okay
        watched->updterrs++;
        OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogCacheFlag,
            "kextcache error while updating %s (error count: %d)",
            watched->caches->root, watched->updterrs);
    }

    OSKextLog(NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag,
             "%d unlocked %s", client, watched->caches->root);
 
    // disable owners if we enabled them for the locker (updateMount() logs)
    if (watched->origMntFlags) {
        (void)updateMount(watched->caches->root, watched->origMntFlags);
        watched->origMntFlags = 0;
    }

    // if kextcache failed, handleRebootHandoff() will fire off another
    // but only if updterrs is less than the failure limit
    handleWatchedHandoff(watched);
    if (!sRebootLock && sRebootWaiter)
        handleRebootHandoff();

    // once upon a time, we thought we could save five seconds here
    // instead, we will just call kextcache -u and it will build the mkext

    rval = KERN_SUCCESS;

finish:
    if (volUUID)    CFRelease(volUUID);
    if (rval && watched) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "Couldn't unlock '%s'.", watched->caches->root);
    }

    return rval;
}

#if 0
{
mach_port_urefs_t refs = 0xff;
OSKextLog(/* kext */ NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag, "DEBUG: mach_port_get_refs(..%d, send..): %x -> %x ref(s).", client,
mach_port_get_refs(mach_task_self(), client, MACH_PORT_RIGHT_SEND, &refs),refs);
}
#endif

/******************************************************************************
* port_died() tells us when a tracked send right goes away.
* We track send rights (on the client ports passed to us) as long as we
* have resources allocated to those clients.  If they die, we get notified
* that the send right went away and then we clean up the associated resource.
*
* This function should only be called when shutdown/reboot exits before kextd
* or when a kextcache process is terminated against its will.
*
* If the client explicitly deallocates its *receive* right / port while we are
* tracking the corresponding send right, port_died() is also called, though
* kextcache should unlock the volume before doing that.
*****************************************************************************/
static void port_died(CFMachPortRef cfport, void *info)
{
    mach_port_t mport = cfport ? CFMachPortGetPort(cfport) : MACH_PORT_NULL;
    struct watchedVol* watched;
    // OSKextLog(/* kext */ NULL, kOSKextLogDebugLevel | kOSKextLogIPCFlag, "DEBUG: port_died(%p/%d): entry.", cfport, mport);

    // all watched-associated ports should have context
    if (!info || !cfport) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "port_died() fatal error: invalid data.");
        goto finish;
    }


    // only means of release for reboot lock
    if (info == &sRebootLock) {
        if (cfport == sRebootLock) {
            // reboot/shutdown happened to exit before kextd
            // XX start timer now or when reboot lock granted?
            cleanupPort(&sRebootLock);
        } else if (cfport == sRebootWaiter) {
            cleanupPort(&sRebootWaiter);        // gave up waiting
        } else {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogIPCFlag,
                "Improperly tracked shutdown/reboot process died.");
        }
        goto finish;
    }


    // else ... handle volume lockers and waiters

    watched = (struct watchedVol*)info;

    // vol_disappeared() removes the volume from sFsysWatchDict before
    // cleaning up all the waiters, so unless the runloop somehow jumped
    // over here from the midst of that callout, no ports affiliated
    // with missing watchVol*'s should be dying.  Even multiple
    // reboot waiters would all have the same context and the mach
    // port check above would catch them.
    if (CFDictionaryGetCountOfValue(sFsysWatchDict, watched) == 0) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "Warning: missing context for deallocated helper port.");
        cleanupPort(&cfport);
        goto finish;
    }

    // watched points to valid data ... was it the locker?
    if (watched->lock && mport == CFMachPortGetPort(watched->lock)) {
        // try to disable owners if we enabled them for the locker
        if (watched->origMntFlags) {
            (void)updateMount(watched->caches->root, watched->origMntFlags);
            watched->origMntFlags = 0;
        }
        
        // if locked, is anyone waiting?
        if (watched->lock) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogIPCFlag,
                "%d (kextcache) exited without unlocking '%s'.",
                CFMachPortGetPort(watched->lock), watched->caches->root);
            watched->updterrs++;
            handleWatchedHandoff(watched);      // cleans up watched->lock
            if (!sRebootLock && sRebootWaiter)
                handleRebootHandoff();
        }
        // if we were storing the worker pid, we might clean it up here
        // see also handleSignalInRunloop() over in kextd_main.c

    } else {    // it must have been a waiter
        CFIndex i;
        if (!watched->waiters) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogWarningLevel | kOSKextLogIPCFlag,
                "Warning: presumed waiter died, but no waiters.");
            goto finish;
        }

        for (i = CFArrayGetCount(watched->waiters); i-- > 0;) {
            CFMachPortRef waiter; 

            waiter = (CFMachPortRef)CFArrayGetValueAtIndex(watched->waiters,i);
            if (mport == CFMachPortGetPort(waiter)) {
                cleanupPort(&waiter);       // --retainCount
                CFArrayRemoveValueAtIndex(watched->waiters, i); // release
                goto finish;      // success
            }
        }

        OSKextLog(/* kext */ NULL,
            kOSKextLogWarningLevel | kOSKextLogIPCFlag,
            "Warning: %s: unknown helper exited.", watched->caches->root);
    }

finish:
    return;
}


/******************************************************************************
 * reconsiderVolumes() iterates the mount list, checking to see if any
 * local mounts have become interesting.  It uses
 * reconsiderVolume() which calls vol_appeared on any we aren't yet watching.
 * If any newly added one needed an update, busyVol is set to its mountpoint.
 * Used after kAutoUpdateDelay to initialize things and to detect OS copies
 * at shutdown time.
 *****************************************************************************/
static Boolean reconsiderVolume(mountpoint_t volToCheck)
{
    int result;
    Boolean rval = false;
    DADiskRef disk = NULL;
    CFDictionaryRef dadesc = NULL;
    CFUUIDRef volUUID;
    struct watchedVol *watched;

    // if it's unknown to diskarb, we can't do much with it (no error)
    result = 0;
    disk = createDiskForMount(sDASession, volToCheck);
    if (!disk)      goto finish;

    result = -1;
    dadesc = DADiskCopyDescription(disk);
    if (!dadesc)    goto finish;
    volUUID = CFDictionaryGetValue(dadesc, kDADiskDescriptionVolumeUUIDKey);
    if (!volUUID)   goto finish;

    watched = (void*)CFDictionaryGetValue(sFsysWatchDict, volUUID);
    if (!watched) {
        // if not watched, check to see if it now has a known OS
        vol_appeared(disk, &rval);      // handles any updates, etc
    } else {
        // if watched, let vol_changed check on its Apple_Boot status
        const void *objs[1] = { kDADiskDescriptionMediaContentKey };
        CFArrayRef  keys;

        keys = CFArrayCreate(nil,objs,1,&kCFTypeArrayCallBacks);
        if (!keys)      goto finish;

        vol_changed(disk, keys, NULL);
        CFRelease(keys);
    }

    result = 0;     // boolean rval is set by call to vol_appeared()

finish:
    if (disk)       CFRelease(disk);
    if (dadesc)     CFRelease(dadesc);
    if (result) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
            "Error reconsidering volume %s.", volToCheck);
    }

    return rval;
}

static Boolean reconsiderVolumes(mountpoint_t busyVol)
{
    Boolean rval = false;
    char *errmsg = NULL;
    int nfsys, i;
    size_t bufsz;
    struct statfs *mounts = NULL;

    // if not set up ...
    if (!sDASession)        goto finish;

    errmsg = "Error while getting mount list.";
    if (-1 == (nfsys = getfsstat(NULL, 0, MNT_NOWAIT))) goto finish;
    bufsz = nfsys * sizeof(struct statfs);
    if (!(mounts = malloc(bufsz)))                  goto finish;
    if (-1 == getfsstat(mounts, bufsz, MNT_NOWAIT)) goto finish;

    errmsg = NULL;  // let reconsiderVolume() take it from here
    for (i = 0; i < nfsys; i++) {
        struct statfs *sfs = &mounts[i];

        if (sfs->f_flags & MNT_LOCAL && strcmp(sfs->f_fstypename, "devfs")) {
            if (reconsiderVolume(sfs->f_mntonname) && !rval) {
                // only capture first volume, but check them all
                strlcpy(busyVol, sfs->f_mntonname, sizeof(mountpoint_t));
                rval = true;
            }
        }
    }

    errmsg = NULL;

finish:
    if (errmsg) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
            "%s", errmsg);
    }
    if (mounts)     free(mounts);

    return rval;
}

/*******************************************************************************
* updateRAIDSet() -- Something on a RAID set has changed, so we may need to
* update its boot partition info.
*******************************************************************************/
#define RAID_MATCH_SIZE   (2)

void updateRAIDSet(
    CFNotificationCenterRef center,
    void * observer,
    CFStringRef name,
    const void * object,
    CFDictionaryRef userInfo)
{
    char * errorMessage = NULL;
    CFStringRef matchingKeys[RAID_MATCH_SIZE] = {
        CFSTR("RAID"),
        CFSTR("UUID") };
    CFTypeRef matchingValues[RAID_MATCH_SIZE] = {
        (CFTypeRef)kCFBooleanTrue,
        (CFTypeRef)object };
    CFDictionaryRef matchPropertyDict = NULL;
    CFMutableDictionaryRef matchingDict = NULL;
    io_service_t theRAIDSet = MACH_PORT_NULL;
    DADiskRef dadisk = NULL;
    CFDictionaryRef dadesc = NULL;
    CFUUIDRef volUUID;          // part of dadesc; not released
    struct watchedVol * watched = NULL;  // do not free

    // nothing to do if we're not watching yet
    if (!sFsysWatchDict)    goto finish;    

    errorMessage = "No RAID set named in RAID set changed notification.";
    if (!object) {
        goto finish;
    }

    errorMessage = "Unable to create matching dictionary for RAID set.";
    matchPropertyDict = CFDictionaryCreate(kCFAllocatorDefault,
        (const void **)&matchingKeys,
        (const void **)&matchingValues,
        RAID_MATCH_SIZE,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!matchPropertyDict) {
        goto finish;
    }

    matchingDict = CFDictionaryCreateMutable(kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!matchingDict) {
        goto finish;
    }
    CFDictionarySetValue(matchingDict, CFSTR(kIOPropertyMatchKey), 
        matchPropertyDict);

    errorMessage = NULL;    // maybe the RAID just went away
    theRAIDSet  = IOServiceGetMatchingService(kIOMasterPortDefault,
        matchingDict);
    matchingDict = NULL;  // IOServiceGetMatchingService() consumes reference!
    if (!theRAIDSet) {
        goto finish;
    }

    errorMessage = "Unable to get DiskArb info for raid set object.";
    dadisk = DADiskCreateFromIOMedia(nil, sDASession, theRAIDSet);
    if (!dadisk)    goto finish;
    dadesc = DADiskCopyDescription(dadisk);
    if (!dadesc)    goto finish;
    volUUID = CFDictionaryGetValue(dadesc, kDADiskDescriptionVolumeUUIDKey);
    if (!volUUID)   goto finish;

    watched = (void*)CFDictionaryGetValue(sFsysWatchDict, volUUID);
    if (watched) {
        (void)launch_rebuild_all(watched->caches->root, true /* force rebuild */, false);
    }

    errorMessage = NULL;

finish:
    if (errorMessage) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "%s", errorMessage);
    }
    if (dadesc)             CFRelease(dadesc);
    if (dadisk)             CFRelease(dadisk);
    if (theRAIDSet)         IOObjectRelease(theRAIDSet);
    if (matchingDict)       CFRelease(matchingDict);
    if (matchPropertyDict)  CFRelease(matchPropertyDict);

    return;
}

/*******************************************************************************
* updateCoreStorageVolume() -- Something on a CoreStorage logical volume has
* changed, so we may need to update its boot partition info.
*******************************************************************************/
#define CSLV_MATCH_SIZE   (2)

void updateCoreStorageVolume(
   CFNotificationCenterRef center,
   void * observer,
   CFStringRef name,
   const void * object,
   CFDictionaryRef userInfo)
{
    char * errorMessage = NULL;
    CFStringRef matchingKeys[CSLV_MATCH_SIZE] = {
        CFSTR("CoreStorage"),
        CFSTR("UUID") };
    CFTypeRef matchingValues[CSLV_MATCH_SIZE] = {
        (CFTypeRef)kCFBooleanTrue,
        (CFTypeRef)object };
    CFDictionaryRef matchPropertyDict = NULL;
    CFMutableDictionaryRef matchingDict = NULL;
    io_service_t theLogicalVolume = MACH_PORT_NULL;
    DADiskRef dadisk = NULL;
    CFDictionaryRef dadesc = NULL;
    CFUUIDRef volUUID;          // part of dadesc; not released
    struct watchedVol * watched = NULL;  // do not free

    // nothing to do if we're not watching yet
    if (!sFsysWatchDict)    goto finish;    

    errorMessage = "No logical volume named in corestorage changed notification.";
    if (!object) {
        goto finish;
    }

    errorMessage = "Unable to create matching dictionary for the CoreStorage volume.";
    matchPropertyDict = CFDictionaryCreate(kCFAllocatorDefault,
        (const void **)&matchingKeys,
        (const void **)&matchingValues,
        CSLV_MATCH_SIZE,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!matchPropertyDict) {
        goto finish;
    }

    matchingDict = CFDictionaryCreateMutable(kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!matchingDict) {
        goto finish;
    }
    CFDictionarySetValue(matchingDict, CFSTR(kIOPropertyMatchKey), 
        matchPropertyDict);

    errorMessage = NULL;    // maybe the volume just went away
    theLogicalVolume  = IOServiceGetMatchingService(kIOMasterPortDefault,
        matchingDict);
    matchingDict = NULL;  // IOServiceGetMatchingService() consumes reference!
    if (!theLogicalVolume) {
        goto finish;
    }

    errorMessage = "Unable to get DiskArb info for corestorage logical volume object.";
    dadisk = DADiskCreateFromIOMedia(nil, sDASession, theLogicalVolume);
    if (!dadisk)    goto finish;
    dadesc = DADiskCopyDescription(dadisk);
    if (!dadesc)    goto finish;
    volUUID = CFDictionaryGetValue(dadesc, kDADiskDescriptionVolumeUUIDKey);
    if (!volUUID)   goto finish;

    watched = (void*)CFDictionaryGetValue(sFsysWatchDict, volUUID);
    if (watched) {
        (void)launch_rebuild_all(watched->caches->root, false, false);
    }

    errorMessage = NULL;

finish:
    if (errorMessage) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "%s", errorMessage);
    }
    if (dadesc)             CFRelease(dadesc);
    if (dadisk)             CFRelease(dadisk);
    if (theLogicalVolume)   IOObjectRelease(theLogicalVolume);
    if (matchingDict)       CFRelease(matchingDict);
    if (matchPropertyDict)  CFRelease(matchPropertyDict);

    return;
}


/*******************************************************************************
* logMTMessage() - log MessageTracer message
*******************************************************************************/
static void logMTMessage(char *signature, char *result, char *mfmt, ...)
{
    va_list ap;
    aslmsg amsg = asl_new(ASL_TYPE_MSG);

    if (!amsg) {
        OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                  "asl_new() failed; MessageTracer message not logged");
        return;
    }

    asl_set(amsg, kMessageTracerDomainKey, kMTCachesDomain);
    asl_set(amsg, kMessageTracerSignatureKey, signature);
    // note: MT 'result' defaults to failure
    asl_set(amsg, kMessageTracerResultKey, result);

    // send it
    va_start(ap, mfmt);
    asl_vlog(NULL /* default handle */, amsg, ASL_LEVEL_NOTICE, mfmt, ap);
    va_end(ap);

    asl_free(amsg);
}
