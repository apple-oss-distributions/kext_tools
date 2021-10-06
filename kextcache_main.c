/*
 * Copyright (c) 2006, 2012 Apple Computer, Inc. All rights reserved.
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
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFBundlePriv.h>
#include <errno.h>
#include <libc.h>
#include <libgen.h>     // dirname()
#include <sys/types.h>
#include <sys/mman.h>
#include <fts.h>
#include <paths.h>
#include <mach-o/arch.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/swap.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_types.h>
#include <mach/machine/vm_param.h>
#include <mach/kmod.h>
#include <notify.h>
#include <stdlib.h>
#include <unistd.h>             // sleep(3)
#include <sys/types.h>
#include <sys/stat.h>
#include <Security/SecKeychainPriv.h>

#include <DiskArbitration/DiskArbitrationPrivate.h>
#include <IOKit/IOTypes.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOKitServer.h>
#include <IOKit/IOCFUnserialize.h>
#include <IOKit/IOCFSerialize.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <libkern/OSByteOrder.h>

#include <IOKit/kext/OSKext.h>
#include <IOKit/kext/OSKextPrivate.h>
#include <IOKit/kext/macho_util.h>
#include <bootfiles.h>

#include <IOKit/pwr_mgt/IOPMLib.h>

#include "kextcache_main.h"
#if !NO_BOOT_ROOT
#include "bootcaches.h"
#include "bootroot_internal.h"
#endif /* !NO_BOOT_ROOT */
#include "mkext1_file.h"
#include "compression.h"
#include "security.h"

// constants
#define MKEXT_PERMS             (0644)

/* The timeout we use when waiting for the system to get to a low load state.
 * We're shooting for about 10 minutes, but we don't want to collide with
 * everyone else who wants to do work 10 minutes after boot, so we just pick
 * a number in that ballpark.
 */
#define kOSKextSystemLoadTimeout        (8 * 60)
#define kOSKextSystemLoadPauseTime      (30)

/*******************************************************************************
* Program Globals
*******************************************************************************/
const char * progname = "(unknown)";

CFMutableDictionaryRef       sNoLoadKextAlertDict = NULL;
CFMutableDictionaryRef       sInvalidSignedKextAlertDict = NULL;
//CFMutableDictionaryRef       sUnsignedKextAlertDict = NULL;
CFMutableDictionaryRef       sExcludedKextAlertDict = NULL;
CFMutableDictionaryRef       sRevokedKextAlertDict = NULL;

/*******************************************************************************
* Utility and callback functions.
*******************************************************************************/
// put/take helpers
static void waitForIOKitQuiescence(void);
static void waitForGreatSystemLoad(void);

#define kMaxArchs 64
#define kRootPathLen 256

static u_int usecs_from_timeval(struct timeval *t);
static void timeval_from_usecs(struct timeval *t, u_int usecs);
static void timeval_difference(struct timeval *dst, 
                               struct timeval *a, struct timeval *b);
static Boolean isValidKextSigningTargetVolume(CFURLRef theURL);
static Boolean wantsFastLibCompressionForTargetVolume(CFURLRef theURL);
static void _appendIfNewest(CFMutableArrayRef theArray, OSKextRef theKext);

#if 1 // 17821398
#include "safecalls.h"

static Boolean needsPrelinkedKernelCopy(KextcacheArgs * toolArgs);
static Boolean wantsPrelinkedKernelCopy(CFURLRef theVolRootURL);

#define k_kernelcacheFilePath \
"/System/Library/Caches/com.apple.kext.caches/Startup/kernelcache"

#define kPrelinkedKernelsPath "/System/Library/PrelinkedKernels"
#define k_prelinkedkernelFilePath kPrelinkedKernelsPath "/prelinkedkernel"

#endif
static Boolean isRootVolURL(CFURLRef theURL);


/*******************************************************************************
*******************************************************************************/
int main(int argc, char * const * argv)
{
    KextcacheArgs       toolArgs;
    ExitStatus          result          = EX_SOFTWARE;
    Boolean             fatal           = false;

   /*****
    * Find out what the program was invoked as.
    */
    progname = rindex(argv[0], '/');
    if (progname) {
        progname++;   // go past the '/'
    } else {
        progname = (char *)argv[0];
    }

   /* Set the OSKext log callback right away.
    */
    OSKextSetLogOutputFunction(&tool_log);

   /*****
    * Check if we were spawned by kextd, set up straightaway
    * for service log filtering, and hook up to ASL.
    */
    if (getenv("KEXTD_SPAWNED")) {
        OSKextSetLogFilter(kDefaultServiceLogFilter | kOSKextLogKextOrGlobalMask,
            /* kernel? */ false);
        OSKextSetLogFilter(kDefaultServiceLogFilter | kOSKextLogKextOrGlobalMask,
            /* kernel? */ true);
        tool_openlog("com.apple.kextcache");
    }

    if (isDebugSetInBootargs()) {
#if 0 // default to more logging when running with debug boot-args
        OSKextLogSpec   logFilter = kOSKextLogDetailLevel |
                                    kOSKextLogVerboseFlagsMask |
                                    kOSKextLogKextOrGlobalMask;
        OSKextSetLogFilter(logFilter, /* kernel? */ false);
        OSKextSetLogFilter(logFilter, /* kernel? */ true);
#endif
       
        /* show command and all arguments...
         */
        int     i;
        int     myBufSize = 0;
        char *  mybuf;
        
        for (i = 0; i < argc; i++) {
            myBufSize += strlen(argv[i]) + 1;
        }
        mybuf = malloc(myBufSize);
        if (mybuf) {
            mybuf[0] = 0x00;
            for (i = 0; i < argc; i++) {
                if (strlcat(mybuf, argv[i], myBufSize) >= myBufSize) {
                    break;
                }
                if (strlcat(mybuf, " ", myBufSize) >= myBufSize) {
                    break;
                }
            }
            OSKextLog(NULL,
                      kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                      "%s",
                      mybuf);
            free(mybuf);
        }
    }
    
   /*****
    * Process args & check for permission to load.
    */
    result = readArgs(&argc, &argv, &toolArgs);
    if (result != EX_OK) {
        if (result == kKextcacheExitHelp) {
            result = EX_OK;
        }
        goto finish;
    }

   /*****
    * Now that we have a custom verbose level set by options,
    * check the filter kextd passed in and combine them.
    */
    checkKextdSpawnedFilter(/* kernel? */ false);
    checkKextdSpawnedFilter(/* kernel? */ true);
    
    result = checkArgs(&toolArgs);
    if (result != EX_OK) {
        goto finish;
    }

   /* From here on out the default exit status is ok.
    */
    result = EX_OK;

    /* Reduce our priority and throttle I/O, then wait for a good time to run.
     */
    if (toolArgs.lowPriorityFlag) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogDetailLevel | kOSKextLogGeneralFlag,
            "Running in low-priority background mode.");

        setpriority(PRIO_PROCESS, getpid(), 20); // run at really low priority
        setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_PROCESS, IOPOL_THROTTLE);

        /* When building the prelinked kernel, we try to wait for a good time
         * to do work.  We can't do this for an mkext yet because we don't
         * have a way to know if we're blocking reboot.
         */
        if (toolArgs.prelinkedKernelPath) {
            waitForGreatSystemLoad();
        }
    }

   /* The whole point of this program is to update caches, so let's not
    * try to read any (we'll briefly turn this back on when checking them).
    */
    OSKextSetUsesCaches(false);

#if !NO_BOOT_ROOT
   /* If it's a Boot!=root update or -invalidate invocation, call
    * checkUpdateCachesAndBoots() with the previously-programmed flags
    * and then jump to exit.  These operations don't combine with
    * more manual cache-building operations.
    */
    if (toolArgs.updateVolumeURL) {
        char volPath[PATH_MAX];

        // go ahead and do the update
        result = doUpdateVolume(&toolArgs);

        // then check for '-Boot -U /' during Safe Boot
        if ((toolArgs.updateOpts & kBRUEarlyBoot) &&
            (toolArgs.updateOpts & kBRUExpectUpToDate) &&
            OSKextGetActualSafeBoot() &&
            CFURLGetFileSystemRepresentation(toolArgs.updateVolumeURL,
                                             true, (UInt8*)volPath, PATH_MAX)
            && 0 == strcmp(volPath, "/")) {
            
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogArchiveFlag,
                      "Safe boot mode detected; rebuilding caches.");

            // ensure kextd's caches get rebuilt later (16803220)
            (void)utimes(kSystemExtensionsDir, NULL);
            (void)utimes(kLibraryExtensionsDir, NULL);
        }
        goto finish;
    }
#endif /* !NO_BOOT_ROOT */

    /* If we're uncompressing the prelinked kernel, take care of that here
     * and exit.
     */
    if (toolArgs.prelinkedKernelPath && !CFArrayGetCount(toolArgs.argURLs) &&
        (toolArgs.compress || toolArgs.uncompress)) 
    {
        result = compressPrelinkedKernel(toolArgs.volumeRootURL,
                                         toolArgs.prelinkedKernelPath,
                                         /* compress */ toolArgs.compress);
        goto finish;
    }

   /*****
    * Read the kexts we'll be working with; first the set of all kexts, then
    * the repository and named kexts for use with mkext-creation flags.
    */
    if (toolArgs.printTestResults) {
        OSKextSetRecordsDiagnostics(kOSKextDiagnosticsFlagAll);
    }
    toolArgs.allKexts = OSKextCreateKextsFromURLs(kCFAllocatorDefault, toolArgs.argURLs);
    if (!toolArgs.allKexts || !CFArrayGetCount(toolArgs.allKexts)) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "No kernel extensions found.");
        result = EX_SOFTWARE;
        goto finish;
    }

    toolArgs.repositoryKexts = OSKextCreateKextsFromURLs(kCFAllocatorDefault,
        toolArgs.repositoryURLs);
    toolArgs.namedKexts = OSKextCreateKextsFromURLs(kCFAllocatorDefault,
        toolArgs.namedKextURLs);
    if (!toolArgs.repositoryKexts || !toolArgs.namedKexts) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "Error reading extensions.");
        result = EX_SOFTWARE;
        goto finish;
    }

    if (result != EX_OK) {
        goto finish;
    }
    
    if (toolArgs.needLoadedKextInfo) {
        result = getLoadedKextInfo(&toolArgs);
        if (result != EX_OK) {
            goto finish;
        }
    }

    // xxx - we are potentially overwriting error results here
    if (toolArgs.updateSystemCaches) {
        result = updateSystemPlistCaches(&toolArgs);
        // don't goto finish on error here, we might be able to create
        // the other caches
    }

    if (toolArgs.mkextPath) {
        result = createMkext(&toolArgs, &fatal);
        if (fatal) {
            goto finish;
        }
    }

    if (toolArgs.prelinkedKernelPath) {
       /* If we're updating the system prelinked kernel, make sure we aren't
        * Safe Boot, or dire consequences shall result.
        */
        if (toolArgs.needDefaultPrelinkedKernelInfo &&
            OSKextGetActualSafeBoot()) {

            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag, 
                "Can't update the system prelinked kernel during safe boot.");
            result = EX_OSERR;
            goto finish;
        }

       /* Create/update the prelinked kernel as explicitly requested, or
        * for the running kernel.
        */
        result = createPrelinkedKernel(&toolArgs);
        if (result != EX_OK) {
            goto finish;
        }

    }

finish:

   /* We're actually not going to free anything else because we're exiting!
    */
    exit(result);

    SAFE_RELEASE(toolArgs.kextIDs);
    SAFE_RELEASE(toolArgs.argURLs);
    SAFE_RELEASE(toolArgs.repositoryURLs);
    SAFE_RELEASE(toolArgs.namedKextURLs);
    SAFE_RELEASE(toolArgs.allKexts);
    SAFE_RELEASE(toolArgs.repositoryKexts);
    SAFE_RELEASE(toolArgs.namedKexts);
    SAFE_RELEASE(toolArgs.loadedKexts);
    SAFE_RELEASE(toolArgs.kernelFile);
    SAFE_RELEASE(toolArgs.symbolDirURL);
    SAFE_FREE(toolArgs.mkextPath);
    SAFE_FREE(toolArgs.prelinkedKernelPath);
    SAFE_FREE(toolArgs.kernelPath);

    return result;
}

/*******************************************************************************
*******************************************************************************/
ExitStatus readArgs(
    int            * argc,
    char * const  ** argv,
    KextcacheArgs  * toolArgs)
{
    ExitStatus   result         = EX_USAGE;
    ExitStatus   scratchResult  = EX_USAGE;
    CFStringRef  scratchString  = NULL;  // must release
    CFNumberRef  scratchNumber  = NULL;  // must release
    CFURLRef     scratchURL     = NULL;  // must release
    size_t       len            = 0;
    uint32_t     i              = 0;
    int          optchar        = 0;
    int          longindex      = -1;
    struct stat  sb;

    bzero(toolArgs, sizeof(*toolArgs));
    
   /*****
    * Allocate collection objects.
    */
    if (!createCFMutableSet(&toolArgs->kextIDs, &kCFTypeSetCallBacks)             ||
        !createCFMutableArray(&toolArgs->argURLs, &kCFTypeArrayCallBacks)         ||
        !createCFMutableArray(&toolArgs->repositoryURLs, &kCFTypeArrayCallBacks)  ||
        !createCFMutableArray(&toolArgs->namedKextURLs, &kCFTypeArrayCallBacks)   ||
        !createCFMutableArray(&toolArgs->targetArchs, NULL)) {

        OSKextLogMemError();
        result = EX_OSERR;
        exit(result);
    }

    /*****
    * Process command line arguments.
    */
    while ((optchar = getopt_long_only(*argc, *argv,
        kOptChars, sOptInfo, &longindex)) != -1) {

        SAFE_RELEASE_NULL(scratchString);
        SAFE_RELEASE_NULL(scratchNumber);
        SAFE_RELEASE_NULL(scratchURL);

        /* When processing short (single-char) options, there is no way to
         * express optional arguments.  Instead, we suppress missing option
         * argument errors by adding a leading ':' to the option string.
         * When getopt detects a missing argument, it will return a ':' so that
         * we can screen for options that are not required to have an argument.
         */
        if (optchar == ':') {
            switch (optopt) {
                case kOptPrelinkedKernel:
                    optchar = optopt;
                    break;
                default:
                    OSKextLog(/* kext */ NULL,
                        kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                        "%s: option requires an argument -- -%c.", 
                        progname, optopt);
                    break;
            }
        }

       /* Catch a -m before the switch and redirect it to the latest supported
        * mkext version, so we don't have to duplicate the code block.
        */
        if (optchar == kOptMkext) {
            optchar = 0;
            longopt = kLongOptMkext;
        }

       /* Catch a -e/-system-mkext and redirect to -system-prelinked-kernel.
        */
        if (optchar == kOptSystemMkext) {
            optchar = 0;
            longopt = kLongOptSystemPrelinkedKernel;
        }

        switch (optchar) {
  
            case kOptArch:
                if (!addArchForName(toolArgs, optarg)) {
                    OSKextLog(/* kext */ NULL,
                        kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                        "Unknown architecture %s.", optarg);
                    goto finish;
                }
                toolArgs->explicitArch = true;
                break;
  
            case kOptBundleIdentifier:
                scratchString = CFStringCreateWithCString(kCFAllocatorDefault,
                   optarg, kCFStringEncodingUTF8);
                if (!scratchString) {
                    OSKextLogMemError();
                    result = EX_OSERR;
                    goto finish;
                }
                CFSetAddValue(toolArgs->kextIDs, scratchString);
                break;
  
            case kOptPrelinkedKernel:
                scratchResult = readPrelinkedKernelArgs(toolArgs, *argc, *argv,
                    /* isLongopt */ longindex != -1);
                if (scratchResult != EX_OK) {
                    result = scratchResult;
                    goto finish;
                }
                break;
  
   
#if !NO_BOOT_ROOT
            case kOptForce:
                toolArgs->updateOpts |= kBRUForceUpdateHelpers;
                break;
#endif /* !NO_BOOT_ROOT */
  
            case kOptLowPriorityFork:
                toolArgs->lowPriorityFlag = true;
                break;
    
            case kOptHelp:
                usage(kUsageLevelFull);
                result = kKextcacheExitHelp;
                goto finish;
    
            case kOptRepositoryCaches:
                OSKextLog(/* kext */ NULL,
                    kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                    "-%c is no longer used; ignoring.",
                    kOptRepositoryCaches);
                break;
    
            case kOptKernel:
                if (toolArgs->kernelPath) {
                    OSKextLog(/* kext */ NULL,
                        kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                        "Warning: kernel file already specified; using last.");
                } else {
                    toolArgs->kernelPath = malloc(PATH_MAX);
                    if (!toolArgs->kernelPath) {
                        OSKextLogMemError();
                        result = EX_OSERR;
                        goto finish;
                    }
                }

                len = strlcpy(toolArgs->kernelPath, optarg, PATH_MAX);
                if (len >= PATH_MAX) {
                    OSKextLog(/* kext */ NULL,
                        kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                        "Error: kernel filename length exceeds PATH_MAX");
                    goto finish;
                }
                break;
    
            case kOptLocalRoot:
                toolArgs->requiredFlagsRepositoriesOnly |=
                    kOSKextOSBundleRequiredLocalRootFlag;
                break;
    
            case kOptLocalRootAll:
                toolArgs->requiredFlagsAll |=
                    kOSKextOSBundleRequiredLocalRootFlag;
                break;
    
            case kOptNetworkRoot:
                toolArgs->requiredFlagsRepositoriesOnly |=
                    kOSKextOSBundleRequiredNetworkRootFlag;
                break;
  
            case kOptNetworkRootAll:
                toolArgs->requiredFlagsAll |=
                    kOSKextOSBundleRequiredNetworkRootFlag;
                break;
  
            case kOptAllLoaded:
                toolArgs->needLoadedKextInfo = true;
                break;
  
            case kOptSafeBoot:
                toolArgs->requiredFlagsRepositoriesOnly |=
                    kOSKextOSBundleRequiredSafeBootFlag;
                break;
  
            case kOptSafeBootAll:
                toolArgs->requiredFlagsAll |=
                    kOSKextOSBundleRequiredSafeBootFlag;
                break;
  
            case kOptTests:
                toolArgs->printTestResults = true;
                break;
 
#if !NO_BOOT_ROOT
            case kOptInvalidate:
                if (toolArgs->updateVolumeURL) {
                    OSKextLog(/* kext */ NULL,
                              kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                              "Warning: invalidate volume already specified; using last.");
                    SAFE_RELEASE_NULL(toolArgs->updateVolumeURL);
                }
                // sanity check that the volume exists
                if (stat(optarg, &sb)) {
                    OSKextLog(NULL,kOSKextLogWarningLevel|kOSKextLogFileAccessFlag,
                              "%s - %s.", optarg, strerror(errno));
                    result = EX_NOINPUT;
                    goto finish;
                }
                
                scratchURL = CFURLCreateFromFileSystemRepresentation(
                                                        kCFAllocatorDefault,
                                                        (const UInt8 *)optarg,
                                                        strlen(optarg),
                                                        true);
                if (!scratchURL) {
                    OSKextLogStringError(/* kext */ NULL);
                    result = EX_OSERR;
                    goto finish;
                }
                toolArgs->updateVolumeURL = CFRetain(scratchURL);
                toolArgs->updateOpts |= kBRUInvalidateKextcache;
                break;

            case kOptUpdate:
            case kOptCheckUpdate:
                if (toolArgs->updateVolumeURL) {
                    OSKextLog(/* kext */ NULL,
                        kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                        "Warning: update volume already specified; using last.");
                    SAFE_RELEASE_NULL(toolArgs->updateVolumeURL);
                }
                // sanity check that the volume exists
                if (stat(optarg, &sb)) {
                    OSKextLog(NULL,kOSKextLogWarningLevel|kOSKextLogFileAccessFlag,
                              "%s - %s.", optarg, strerror(errno));
                    result = EX_NOINPUT;
                    goto finish;
                }

                scratchURL = CFURLCreateFromFileSystemRepresentation(
                    kCFAllocatorDefault,
                    (const UInt8 *)optarg, strlen(optarg), true);
                if (!scratchURL) {
                    OSKextLogStringError(/* kext */ NULL);
                    result = EX_OSERR;
                    goto finish;
                }
                toolArgs->updateVolumeURL = CFRetain(scratchURL);
                if (optchar == kOptCheckUpdate) {
                    toolArgs->updateOpts |= kBRUExpectUpToDate;
                    toolArgs->updateOpts |= kBRUCachesAnyRoot;
                }
                break;
#endif /* !NO_BOOT_ROOT */
          
            case kOptQuiet:
                beQuiet();
                break;

            case kOptVerbose:
                scratchResult = setLogFilterForOpt(*argc, *argv,
                    /* forceOnFlags */ kOSKextLogKextOrGlobalMask);
                if (scratchResult != EX_OK) {
                    result = scratchResult;
                    goto finish;
                }
                break;

            case kOptNoAuthentication:
                toolArgs->skipAuthentication = true;
                break;

            case 0:
                switch (longopt) {
                    case kLongOptMkext1:
                    case kLongOptMkext2:
                    // note kLongOptMkext == latest supported version 
                        if (toolArgs->mkextPath) {
                            OSKextLog(/* kext */ NULL,
                                kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                                "Warning: output mkext file already specified; using last.");
                        } else {
                            toolArgs->mkextPath = malloc(PATH_MAX);
                            if (!toolArgs->mkextPath) {
                                OSKextLogMemError();
                                result = EX_OSERR;
                                goto finish;
                            }
                        }

                        len = strlcpy(toolArgs->mkextPath, optarg, PATH_MAX);
                        if (len >= PATH_MAX) {
                            OSKextLog(/* kext */ NULL,
                                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                                "Error: mkext filename length exceeds PATH_MAX");
                            goto finish;
                        }
                        
                        if (longopt == kLongOptMkext1) {
                            toolArgs->mkextVersion = 1;
                        } else if (longopt == kLongOptMkext2) {
                            toolArgs->mkextVersion = 2;
                        } else {
                            OSKextLog(/* kext */ NULL,
                                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                                "Intenral error.");
                        }
                        break;

                    case kLongOptVolumeRoot:
                        if (toolArgs->volumeRootURL) {
                            OSKextLog(/* kext */ NULL,
                                kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                                "Warning: volume root already specified; using last.");
                            SAFE_RELEASE_NULL(toolArgs->volumeRootURL);
                        }

                        scratchURL = CFURLCreateFromFileSystemRepresentation(
                            kCFAllocatorDefault,
                            (const UInt8 *)optarg, strlen(optarg), true);
                        if (!scratchURL) {
                            OSKextLogStringError(/* kext */ NULL);
                            result = EX_OSERR;
                            goto finish;
                        }

                        toolArgs->volumeRootURL = CFRetain(scratchURL);
                        break;


                    case kLongOptSystemCaches:
                        toolArgs->updateSystemCaches = true;
                        setSystemExtensionsFolders(toolArgs);
                        break;

                    case kLongOptCompressed:
                        toolArgs->compress = true;
                        break;

                    case kLongOptUncompressed:
                        toolArgs->uncompress = true;
                        break;

                    case kLongOptSymbols:
                        if (toolArgs->symbolDirURL) {
                            OSKextLog(/* kext */ NULL,
                                kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                                "Warning: symbol directory already specified; using last.");
                            SAFE_RELEASE_NULL(toolArgs->symbolDirURL);
                        }

                        scratchURL = CFURLCreateFromFileSystemRepresentation(
                            kCFAllocatorDefault,
                            (const UInt8 *)optarg, strlen(optarg), true);
                        if (!scratchURL) {
                            OSKextLogStringError(/* kext */ NULL);
                            result = EX_OSERR;
                            goto finish;
                        }

                        toolArgs->symbolDirURL = CFRetain(scratchURL);
                        toolArgs->generatePrelinkedSymbols = true;
                        break;

                    case kLongOptSystemPrelinkedKernel:
                        scratchResult = setPrelinkedKernelArgs(toolArgs,
                            /* filename */ NULL);
                        if (scratchResult != EX_OK) {
                            result = scratchResult;
                            goto finish;
                        }
                        toolArgs->needLoadedKextInfo = true;
                        toolArgs->requiredFlagsRepositoriesOnly |=
                            kOSKextOSBundleRequiredLocalRootFlag;
                        break;

                    case kLongOptAllPersonalities:
                        toolArgs->includeAllPersonalities = true;
                        break;

                    case kLongOptNoLinkFailures:
                        toolArgs->noLinkFailures = true;
                        break;

                    case kLongOptStripSymbols:
                        toolArgs->stripSymbols = true;
                        break;

#if !NO_BOOT_ROOT
                    case kLongOptInstaller:
                        toolArgs->updateOpts |= kBRUHelpersOptional;
                        toolArgs->updateOpts |= kBRUForceUpdateHelpers;
                        break;
                    case kLongOptCachesOnly:
                        toolArgs->updateOpts |= kBRUCachesOnly;
                        break;
                    case kLongOptEarlyBoot:
                        toolArgs->updateOpts |= kBRUEarlyBoot;
                        break;
#endif /* !NO_BOOT_ROOT */

                    default:
                       /* Because we use ':', getopt_long doesn't print an error message.
                        */
                        OSKextLog(/* kext */ NULL,
                            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                            "unrecognized option %s", (*argv)[optind-1]);
                        goto finish;
                        break;
                }
                break;

            default:
               /* Because we use ':', getopt_long doesn't print an error message.
                */
                OSKextLog(/* kext */ NULL,
                    kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                    "unrecognized option %s", (*argv)[optind-1]);
                goto finish;
                break;

        }
        
       /* Reset longindex, because getopt_long_only() is stupid and doesn't.
        */
        longindex = -1;
    }

   /* Update the argc & argv seen by main() so that boot<>root calls
    * handle remaining args.
    */
    *argc -= optind;
    *argv += optind;

   /*****
    * If we aren't doing a boot<>root update, record the kext & directory names
    * from the command line. (If we are doing a boot<>root update, remaining
    * command line args are processed later.)
    */
    if (!toolArgs->updateVolumeURL) {
        for (i = 0; i < *argc; i++) {
            SAFE_RELEASE_NULL(scratchURL);
            SAFE_RELEASE_NULL(scratchString);

            scratchURL = CFURLCreateFromFileSystemRepresentation(
                kCFAllocatorDefault,
                (const UInt8 *)(*argv)[i], strlen((*argv)[i]), true);
            if (!scratchURL) {
                OSKextLogMemError();
                result = EX_OSERR;
                goto finish;
            }
            CFArrayAppendValue(toolArgs->argURLs, scratchURL);

            scratchString = CFURLCopyPathExtension(scratchURL);
            if (scratchString && CFEqual(scratchString, CFSTR("kext"))) {
                CFArrayAppendValue(toolArgs->namedKextURLs, scratchURL);
            } else {
                CFArrayAppendValue(toolArgs->repositoryURLs, scratchURL);
            }
        }
    }

    result = EX_OK;

finish:
    SAFE_RELEASE(scratchString);
    SAFE_RELEASE(scratchNumber);
    SAFE_RELEASE(scratchURL);

    if (result == EX_USAGE) {
        usage(kUsageLevelBrief);
    }
    return result;
}

/*******************************************************************************
*******************************************************************************/
ExitStatus readPrelinkedKernelArgs(
    KextcacheArgs * toolArgs,
    int             argc,
    char * const  * argv,
    Boolean         isLongopt)
{
    char * filename = NULL;  // do not free

    if (optarg) {
        filename = optarg;
    } else if (isLongopt && optind < argc) {
        filename = argv[optind];
        optind++;
    }
    
    if (filename && !filename[0]) {
        filename = NULL;
    }

    return setPrelinkedKernelArgs(toolArgs, filename);
}

/*******************************************************************************
*******************************************************************************/
ExitStatus setPrelinkedKernelArgs(
    KextcacheArgs * toolArgs,
    char          * filename)
{
    ExitStatus          result          = EX_USAGE;
 
    if (toolArgs->prelinkedKernelPath) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
            "Warning: prelinked kernel already specified; using last.");
    } else {
        toolArgs->prelinkedKernelPath = malloc(PATH_MAX);
        if (!toolArgs->prelinkedKernelPath) {
            OSKextLogMemError();
            result = EX_OSERR;
            goto finish;
        }
    }

   /* If we don't have a filename we construct a default one, automatically
    * add the system extensions folders, and note that we're using default
    * info.
    */
    if (!filename) {
#if NO_BOOT_ROOT
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "Error: prelinked kernel filename required");
        goto finish;
#else
        if (!setDefaultPrelinkedKernel(toolArgs)) {
            goto finish;
        }
        toolArgs->needDefaultPrelinkedKernelInfo = true;
        setSystemExtensionsFolders(toolArgs);
#endif /* NO_BOOT_ROOT */
    } else {
        size_t len = strlcpy(toolArgs->prelinkedKernelPath, filename, PATH_MAX);
        if (len >= PATH_MAX) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                "Error: prelinked kernel filename length exceeds PATH_MAX");
            goto finish;
        }
    }
    result = EX_OK;
finish:
   return result;
}


#if !NO_BOOT_ROOT
#ifndef kIOPMAssertNoIdleSystemSleep
#define kIOPMAssertNoIdleSystemSleep \
            kIOPMAssertionTypePreventUserIdleSystemSleep
#endif
ExitStatus doUpdateVolume(KextcacheArgs *toolArgs)
{
    ExitStatus rval;                    // no goto's in this function
    int result;                         // errno-type value
    IOReturn pmres = kIOReturnError;    // init against future re-flow
    IOPMAssertionID awakeForUpdate;     // valid if pmres == 0

    // unless -F is passed, keep machine awake for for duration
    // (including waiting for any volume locks with kextd)
    if (toolArgs->lowPriorityFlag == false) {
        pmres = IOPMAssertionCreateWithName(kIOPMAssertNoIdleSystemSleep,
                            kIOPMAssertionLevelOn,
                            CFSTR("com.apple.kextmanager.update"),
                            &awakeForUpdate);
        if (pmres) {
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                      "Warning: couldn't block sleep during cache update");
        }

    }

    result = checkUpdateCachesAndBoots(toolArgs->updateVolumeURL,
                                       toolArgs->updateOpts);
    // translate known errno -> sysexits(3) value
    switch (result) {
        case ENOENT:
        case EFTYPE: rval = EX_OSFILE; break;
        default: rval = result;
    }

    if (toolArgs->lowPriorityFlag == false && pmres == 0) {
        // drop assertion
        if (IOPMAssertionRelease(awakeForUpdate))
            OSKextLog(NULL, kOSKextLogWarningLevel | kOSKextLogGeneralFlag,
                      "Warning: error re-enabling sleep after cache update");
    }

    return rval;
}

/*******************************************************************************
*******************************************************************************/
Boolean setDefaultKernel(KextcacheArgs * toolArgs)
{
#if DEV_KERNEL_SUPPORT
    Boolean      addSuffix = FALSE;
#endif
    size_t       length = 0;
    struct stat  statBuf;
        
    if (!toolArgs->kernelPath) {
        toolArgs->kernelPath = malloc(PATH_MAX);
        if (!toolArgs->kernelPath) {
            OSKextLogMemError();
            return FALSE;
        }
    }
    
    while( true ) {
        
        // use KernelPath from /usr/standalone/bootcaches.plist
        if (getKernelPathForURL(toolArgs->volumeRootURL,
                                toolArgs->kernelPath,
                                PATH_MAX) == FALSE) {
            // no bootcaches.plist?  Forced to hardwire...
            strlcpy(toolArgs->kernelPath, "/System/Library/Kernels/kernel",
                    PATH_MAX);
        }
        
#if DEV_KERNEL_SUPPORT
        // for Apple Internal builds try to default to dev kernel
        // /System/Library/Kernels/kernel.development
        addSuffix = useDevelopmentKernel(toolArgs->kernelPath);
        if (addSuffix) {
            if (strlen(toolArgs->kernelPath) + strlen(kDefaultKernelSuffix) + 1 < PATH_MAX) {
                strlcat(toolArgs->kernelPath,
                        kDefaultKernelSuffix,
                        PATH_MAX);
            }
            else {
                addSuffix = FALSE;
            }
        }
#endif
       
        if (statPath(toolArgs->kernelPath, &statBuf) == EX_OK) {
            break;
        }
      
        OSKextLog(/* kext */ NULL,
                  kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                  "Error: invalid kernel path '%s'",
                  toolArgs->kernelPath);
        return FALSE;
    } // while...

    TIMESPEC_TO_TIMEVAL(&toolArgs->kernelTimes[0], &statBuf.st_atimespec);
    TIMESPEC_TO_TIMEVAL(&toolArgs->kernelTimes[1], &statBuf.st_mtimespec);

#if DEV_KERNEL_SUPPORT
    if (toolArgs->prelinkedKernelPath &&
        toolArgs->needDefaultPrelinkedKernelInfo &&
        addSuffix) {
        // we are using default kernelcache name so add .development suffix
        length = strlcat(toolArgs->prelinkedKernelPath,
                         kDefaultKernelSuffix,
                         PATH_MAX);
        if (length >= PATH_MAX) {
            OSKextLog(/* kext */ NULL,
                      kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                      "Error: kernelcache filename length exceeds PATH_MAX");
            return FALSE;
        }
    }
#endif
  
    return TRUE;
}



/*******************************************************************************
*******************************************************************************/
Boolean setDefaultPrelinkedKernel(KextcacheArgs * toolArgs)
{
    Boolean      result              = FALSE;
    const char * prelinkedKernelFile = NULL;
    size_t       length              = 0;
    
        prelinkedKernelFile =
            _kOSKextCachesRootFolder "/" _kOSKextStartupCachesSubfolder "/"
            _kOSKextPrelinkedKernelBasename;

    length = strlcpy(toolArgs->prelinkedKernelPath, 
        prelinkedKernelFile, PATH_MAX);
    if (length >= PATH_MAX) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "Error: prelinked kernel filename length exceeds PATH_MAX");
        goto finish;
    }
    
    result = TRUE;

finish:
    return result;
}
#endif /* !NO_BOOT_ROOT */

/*******************************************************************************
*******************************************************************************/
void setSystemExtensionsFolders(KextcacheArgs * toolArgs)
{
    CFArrayRef sysExtensionsFolders = OSKextGetSystemExtensionsFolderURLs();

    CFArrayAppendArray(toolArgs->argURLs,
        sysExtensionsFolders, RANGE_ALL(sysExtensionsFolders));
    CFArrayAppendArray(toolArgs->repositoryURLs,
        sysExtensionsFolders, RANGE_ALL(sysExtensionsFolders));

    return;
}

/*******************************************************************************
*******************************************************************************/
#include <servers/bootstrap.h>    // bootstrap mach ports

static void
waitForIOKitQuiescence(void)
{
    kern_return_t   kern_result = 0;
    mach_timespec_t waitTime = { 40, 0 };

    // if kextd is not running yet (early boot) then IOKitWaitQuiet will
    // always time out.  So go ahead and bail out if there is no kextd.
    if ( isKextdRunning() == FALSE ) {
        return;
    }

    OSKextLog(/* kext */ NULL,
        kOSKextLogProgressLevel | kOSKextLogIPCFlag,
        "Waiting for I/O Kit to quiesce.");

    kern_result = IOKitWaitQuiet(kIOMasterPortDefault, &waitTime);
    if (kern_result == kIOReturnTimeout) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogIPCFlag,
            "IOKitWaitQuiet() timed out.");
    } else if (kern_result != kOSReturnSuccess) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "IOKitWaitQuiet() failed - %s.",
            safe_mach_error_string(kern_result));
    }
}

/*******************************************************************************
* Wait for the system to report that it's a good time to do work.  We define a
* good time to be when the IOSystemLoadAdvisory API returns a combined level of 
* kIOSystemLoadAdvisoryLevelGreat, and we'll wait up to kOSKextSystemLoadTimeout
* seconds for the system to enter that state before we begin our work.  If there
* is an error in this function, we just return and get started with the work.
*******************************************************************************/
static void
waitForGreatSystemLoad(void)
{
    struct timeval currenttime;
    struct timeval endtime;
    struct timeval timeout;
    fd_set readfds;
    fd_set tmpfds;
    uint64_t systemLoadAdvisoryState            = 0;
    uint32_t notifyStatus                       = 0;
    uint32_t usecs                              = 0;
    int systemLoadAdvisoryFileDescriptor        = 0;    // closed by notify_cancel()
    int systemLoadAdvisoryToken                 = 0;    // must notify_cancel()
    int currentToken                            = 0;    // do not notify_cancel()
    int myResult;

    bzero(&currenttime, sizeof(currenttime));
    bzero(&endtime, sizeof(endtime));
    bzero(&timeout, sizeof(timeout));

    OSKextLog(/* kext */ NULL,
        kOSKextLogProgressLevel | kOSKextLogGeneralFlag,
        "Waiting for low system load.");

    /* Register for SystemLoadAdvisory notifications */

    notifyStatus = notify_register_file_descriptor(kIOSystemLoadAdvisoryNotifyName, 
        &systemLoadAdvisoryFileDescriptor, 
        /* flags */ 0, &systemLoadAdvisoryToken);
    if (notifyStatus != NOTIFY_STATUS_OK) {
        goto finish;
    }

    OSKextLog(/* kext */ NULL,
        kOSKextLogDebugLevel | kOSKextLogGeneralFlag,
        "Received initial system load status %llu", systemLoadAdvisoryState);

    /* If it's a good time, we'll just return */

    notifyStatus = notify_get_state(systemLoadAdvisoryToken, &systemLoadAdvisoryState);
    if (notifyStatus != NOTIFY_STATUS_OK) {
        goto finish;
    }

    if (systemLoadAdvisoryState == kIOSystemLoadAdvisoryLevelGreat) {
        goto finish;
    }

    /* Set up the select timers */

    myResult = gettimeofday(&currenttime, NULL);
    if (myResult < 0) {
        goto finish;
    }

    endtime = currenttime;
    endtime.tv_sec += kOSKextSystemLoadTimeout;

    timeval_difference(&timeout, &endtime, &currenttime);
    usecs = usecs_from_timeval(&timeout);

    FD_ZERO(&readfds);
    FD_SET(systemLoadAdvisoryFileDescriptor, &readfds);

    /* Check SystemLoadAdvisory notifications until it's a great time to
     * do work or we hit the timeout.
     */  

    while (usecs) {
        /* Wait for notifications or the timeout */

        FD_COPY(&readfds, &tmpfds);
        myResult = select(systemLoadAdvisoryFileDescriptor + 1, 
            &tmpfds, NULL, NULL, &timeout);
        if (myResult < 0) {
            goto finish;
        }

        /* Set up the next timeout */

        myResult = gettimeofday(&currenttime, NULL);
        if (myResult < 0) {
            goto finish;
        }

        timeval_difference(&timeout, &endtime, &currenttime);
        usecs = usecs_from_timeval(&timeout);

        /* Check the system load state */

        if (!FD_ISSET(systemLoadAdvisoryFileDescriptor, &tmpfds)) {
            continue;
        }

        myResult = (int)read(systemLoadAdvisoryFileDescriptor,
            &currentToken, sizeof(currentToken));
        if (myResult < 0) {
            goto finish;
        }

        /* The token is written in network byte order. */
        currentToken = ntohl(currentToken);

        if (currentToken != systemLoadAdvisoryToken) {
            continue;
        }

        notifyStatus = notify_get_state(systemLoadAdvisoryToken, 
            &systemLoadAdvisoryState);
        if (notifyStatus != NOTIFY_STATUS_OK) {
            goto finish;
        }

        OSKextLog(/* kext */ NULL,
            kOSKextLogDebugLevel | kOSKextLogGeneralFlag,
            "Received updated system load status %llu", systemLoadAdvisoryState);

        if (systemLoadAdvisoryState == kIOSystemLoadAdvisoryLevelGreat) {
            break;
        }
    }

    OSKextLog(/* kext */ NULL,
        kOSKextLogDebugLevel | kOSKextLogGeneralFlag,
        "Pausing for another %d seconds to avoid work contention",
        kOSKextSystemLoadPauseTime);

    /* We'll wait a random amount longer to avoid colliding with 
     * other work that is waiting for a great time.
     */
    sleep(kOSKextSystemLoadPauseTime);

    OSKextLog(/* kext */ NULL,
        kOSKextLogDebugLevel | kOSKextLogGeneralFlag,
        "System load is low.  Proceeding.\n");
finish:
    if (systemLoadAdvisoryToken) {
        notify_cancel(systemLoadAdvisoryToken);
    }
    return;
}

/*******************************************************************************
*******************************************************************************/
static u_int
usecs_from_timeval(struct timeval *t)
{
    u_int usecs = 0;

    if (t) {
        usecs = (unsigned int)((t->tv_sec * 1000) + t->tv_usec);
    }

    return usecs;
}

/*******************************************************************************
*******************************************************************************/
static void
timeval_from_usecs(struct timeval *t, u_int usecs)
{
    if (t) {
        if (usecs > 0) {
            t->tv_sec = usecs / 1000;
            t->tv_usec = usecs % 1000;
        } else {
            bzero(t, sizeof(*t));
        }
    }
}

/*******************************************************************************
* dst = a - b
*******************************************************************************/
static void
timeval_difference(struct timeval *dst, struct timeval *a, struct timeval *b)
{
    u_int ausec = 0, busec = 0, dstusec = 0;

    if (dst) {
        ausec = usecs_from_timeval(a);
        busec = usecs_from_timeval(b);

        if (ausec > busec) {
            dstusec = ausec - busec;
        }

        timeval_from_usecs(dst, dstusec);
    }
}

#if !NO_BOOT_ROOT
/*******************************************************************************
*******************************************************************************/
void setDefaultArchesIfNeeded(KextcacheArgs * toolArgs)
{
   /* If no arches were explicitly specified, use the architecture of the 
    * running kernel.
    */
    if (toolArgs->explicitArch) {
        return;
    }

    CFArrayRemoveAllValues(toolArgs->targetArchs);   
    addArch(toolArgs, OSKextGetRunningKernelArchitecture());
    
    return;
}
#endif /* !NO_BOOT_ROOT */

/*******************************************************************************
********************************************************************************/
void addArch(
    KextcacheArgs * toolArgs,
    const NXArchInfo  * arch)
{
    if (CFArrayContainsValue(toolArgs->targetArchs, 
        RANGE_ALL(toolArgs->targetArchs), arch)) 
    {
        return;
    }
    
    CFArrayAppendValue(toolArgs->targetArchs, arch);
}

/*******************************************************************************
*******************************************************************************/
const NXArchInfo * addArchForName(
    KextcacheArgs     * toolArgs,
    const char    * archname)
{
    const NXArchInfo * result = NULL;
    
    result = NXGetArchInfoFromName(archname);
    if (!result) {
        goto finish;
    }

    addArch(toolArgs, result);
    
finish:
    return result;
}

/*******************************************************************************
*******************************************************************************/
void checkKextdSpawnedFilter(Boolean kernelFlag)
{
    const char * environmentVariable  = NULL;  // do not free
    char       * environmentLogFilterString = NULL;  // do not free

    if (kernelFlag) {
        environmentVariable = "KEXT_LOG_FILTER_KERNEL";
    } else {
        environmentVariable = "KEXT_LOG_FILTER_USER";
    }

    environmentLogFilterString = getenv(environmentVariable);

   /*****
    * If we have environment variables for a log spec, take the greater
    * of the log levels and OR together the flags from the environment's &
    * this process's command-line log specs. This way the most verbose setting
    * always applies.
    *
    * Otherwise, set the environment variable in case we spawn children.
    */
    if (environmentLogFilterString) {
        OSKextLogSpec toolLogSpec  = OSKextGetLogFilter(kernelFlag);
        OSKextLogSpec kextdLogSpec = (unsigned int)strtoul(environmentLogFilterString, NULL, 16);
        
        OSKextLogSpec toolLogLevel  = toolLogSpec & kOSKextLogLevelMask;
        OSKextLogSpec kextdLogLevel = kextdLogSpec & kOSKextLogLevelMask;
        OSKextLogSpec comboLogLevel = MAX(toolLogLevel, kextdLogLevel);
        
        OSKextLogSpec toolLogFlags  = toolLogSpec & kOSKextLogFlagsMask;
        OSKextLogSpec kextdLogFlags = kextdLogSpec & kOSKextLogFlagsMask;
        OSKextLogSpec comboLogFlags = toolLogFlags | kextdLogFlags |
            kOSKextLogKextOrGlobalMask;
        
        OSKextSetLogFilter(comboLogLevel | comboLogFlags, kernelFlag);
    } else {
        char logSpecBuffer[16];  // enough for a 64-bit hex value

        snprintf(logSpecBuffer, sizeof(logSpecBuffer), "0x%x",
            OSKextGetLogFilter(kernelFlag));
        setenv(environmentVariable, logSpecBuffer, /* overwrite */ 1);
    }
    
    return;
}

/*******************************************************************************
*******************************************************************************/
ExitStatus checkArgs(KextcacheArgs * toolArgs)
{
    ExitStatus  result  = EX_USAGE;
    Boolean expectUpToDate = toolArgs->updateOpts & kBRUExpectUpToDate;

    if (!toolArgs->mkextPath && !toolArgs->prelinkedKernelPath &&
        !toolArgs->updateVolumeURL && !toolArgs->updateSystemCaches) 
    {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "No work to do; check options and try again.");
        goto finish;
    }
    
    if (toolArgs->volumeRootURL && !toolArgs->mkextPath &&
        !toolArgs->prelinkedKernelPath) 
    {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "Use -%s only when creating an mkext archive or prelinked kernel.",
            kOptNameVolumeRoot);
        goto finish;
    }

    if (!toolArgs->updateVolumeURL && !CFArrayGetCount(toolArgs->argURLs) &&
        !toolArgs->compress && !toolArgs->uncompress) 
    {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "No kexts or directories specified.");
        goto finish;
    }

    if (!toolArgs->compress && !toolArgs->uncompress) {
        toolArgs->compress = true;
    } else if (toolArgs->compress && toolArgs->uncompress) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "Both -%s and -%s specified; using -%s.",
            kOptNameCompressed, kOptNameUncompressed, kOptNameCompressed);
        toolArgs->compress = true;
        toolArgs->uncompress = false;
    }
    
#if !NO_BOOT_ROOT
    if ((toolArgs->updateOpts & kBRUForceUpdateHelpers)
            && (toolArgs->updateOpts & kBRUCachesOnly)) {
        OSKextLog(NULL, kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                  "-%s (%-c) and %-s are mutually exclusive",
                  kOptNameForce, kOptForce, kOptNameCachesOnly);
        goto finish;
    }
    if (toolArgs->updateOpts & kBRUForceUpdateHelpers) {
        if (expectUpToDate || !toolArgs->updateVolumeURL) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                "-%s (-%c) is allowed only with -%s (-%c).",
                kOptNameForce, kOptForce, kOptNameUpdate, kOptUpdate);
            goto finish;
        }
    }
    if (toolArgs->updateOpts & kBRUEarlyBoot) {
        if (!toolArgs->updateVolumeURL) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                "-%s requires -%c.",
                kOptNameEarlyBoot, kOptCheckUpdate);
            goto finish;
        }
    }
    if (toolArgs->updateOpts & kBRUCachesOnly) {
        if (expectUpToDate || !toolArgs->updateVolumeURL) {
            OSKextLog(/* kext */ NULL,
                      kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                      "-%s is allowed only with -%s (-%c).",
                      kOptNameCachesOnly, kOptNameUpdate, kOptUpdate);
            goto finish;
        }
    }
#endif /* !NO_BOOT_ROOT */

    if (toolArgs->updateVolumeURL) {
        if (toolArgs->mkextPath || toolArgs->prelinkedKernelPath) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                "Can't create mkext or prelinked kernel when updating volumes.");
        }
    }

#if !NO_BOOT_ROOT
    setDefaultArchesIfNeeded(toolArgs);
#endif /* !NO_BOOT_ROOT */

   /* 11860417 - we now support multiple extensions directories, get access and
    * mod times from extensions directory with the most current mode date.
    */
    if (toolArgs->extensionsDirTimes[1].tv_sec == 0 &&
        CFArrayGetCount(toolArgs->repositoryURLs)) {
        result = getLatestTimesFromCFURLArray(toolArgs->repositoryURLs,
                                              toolArgs->extensionsDirTimes);
        if (result != EX_OK) {
            OSKextLog(NULL,
                      kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                      "%s: Can't get mod times", __FUNCTION__);
            goto finish;
        }
    }
    
#if !NO_BOOT_ROOT
    if (toolArgs->needDefaultPrelinkedKernelInfo && !toolArgs->kernelPath) {       
        if (!setDefaultKernel(toolArgs)) {
            result = EX_USAGE;
            goto finish;
        }
    }
#endif /* !NO_BOOT_ROOT */
    
    if (toolArgs->prelinkedKernelPath && CFArrayGetCount(toolArgs->argURLs)) {
        struct stat     myStatBuf;
        
        if (!toolArgs->kernelPath) {
            if (!setDefaultKernel(toolArgs)) {
                OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                "No kernel specified for prelinked kernel generation.");
                result = EX_USAGE;
                goto finish;
            }
        }
        result = statPath(toolArgs->kernelPath, &myStatBuf);
        if (result != EX_OK) {
            goto finish;
        }
        TIMESPEC_TO_TIMEVAL(&toolArgs->kernelTimes[0], &myStatBuf.st_atimespec);
        TIMESPEC_TO_TIMEVAL(&toolArgs->kernelTimes[1], &myStatBuf.st_mtimespec);
    }
   
   /* Updating system caches requires no additional kexts or repositories,
    * and must run as root.
    */
    if (toolArgs->needDefaultPrelinkedKernelInfo ||
            toolArgs->updateSystemCaches) {

        if (CFArrayGetCount(toolArgs->namedKextURLs) || CFSetGetCount(toolArgs->kextIDs) ||
            !CFEqual(toolArgs->repositoryURLs, OSKextGetSystemExtensionsFolderURLs())) {
            
            OSKextLog(/* kext */ NULL,
                    kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                    "Custom kexts and repository directories are not allowed "
                    "when updating system kext caches.");
            result = EX_USAGE;
            goto finish;

        }

        if (geteuid() != 0) {
            OSKextLog(/* kext */ NULL,
                    kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                    "You must be running as root to update system kext caches.");
            result = EX_NOPERM;
            goto finish;
        }
    }

    result = EX_OK;

finish:
    if (result == EX_USAGE) {
        usage(kUsageLevelBrief);
    }
    return result;
}

/*******************************************************************************
*******************************************************************************/
ExitStatus 
getLoadedKextInfo(
    KextcacheArgs *toolArgs)
{
    ExitStatus  result                  = EX_SOFTWARE;
    CFArrayRef  requestedIdentifiers    = NULL; // must release

    /* Let I/O Kit settle down before we poke at it.
     */
    
    (void) waitForIOKitQuiescence();

    /* Get the list of requested bundle IDs from the kernel and find all of
     * the associated kexts.
     */

    requestedIdentifiers = OSKextCopyAllRequestedIdentifiers();
    if (!requestedIdentifiers) {
        goto finish;
    }

    toolArgs->loadedKexts = OSKextCopyKextsWithIdentifiers(requestedIdentifiers);
    if (!toolArgs->loadedKexts) {
        goto finish;
    }

    result = EX_OK;

finish:
    SAFE_RELEASE(requestedIdentifiers);

    return result;
}

#pragma mark System Plist Caches

/*******************************************************************************
*******************************************************************************/
ExitStatus updateSystemPlistCaches(KextcacheArgs * toolArgs)
{
    ExitStatus         result               = EX_OSERR;
    ExitStatus         directoryResult      = EX_OK;  // flipped to error as needed
    CFArrayRef         systemExtensionsURLs = NULL;   // do not release
    CFArrayRef         kexts                = NULL;   // must release
    CFURLRef           folderURL            = NULL;   // do not release
    char               folderPath[PATH_MAX] = "";
    const NXArchInfo * startArch            = OSKextGetArchitecture();
    CFArrayRef         directoryValues      = NULL;   // must release
    CFArrayRef         personalities        = NULL;   // must release
    CFIndex            count, i;

   /* We only care about updating info for the system extensions folders.
    */
    systemExtensionsURLs = OSKextGetSystemExtensionsFolderURLs();
    if (!systemExtensionsURLs) {
        OSKextLogMemError();
        result = EX_OSERR;
        goto finish;
    }

    kexts = OSKextCreateKextsFromURLs(kCFAllocatorDefault, systemExtensionsURLs);
    if (!kexts) {
        goto finish;
    }

   /* Update the global personalities & property-value caches, each per arch.
    */
    for (i = 0; i < CFArrayGetCount(toolArgs->targetArchs); i++) {
        const NXArchInfo * targetArch = 
            CFArrayGetValueAtIndex(toolArgs->targetArchs, i);

        SAFE_RELEASE_NULL(personalities);

       /* Set the active architecture for scooping out personalities and such.
        */        
        if (!OSKextSetArchitecture(targetArch)) {
            goto finish;
        }

        personalities = OSKextCopyPersonalitiesOfKexts(kexts);
        if (!personalities) {
            goto finish;
        }

        if (!_OSKextWriteCache(systemExtensionsURLs, CFSTR(kIOKitPersonalitiesKey),
            targetArch, _kOSKextCacheFormatIOXML, personalities)) {

            goto finish;
        }

       /* Loginwindow asks us for this property so let's spare lots of I/O
        * by caching it. This read function call updates the caches for us;
        * we don't use the output.
        */
        if (!readSystemKextPropertyValues(CFSTR(kOSBundleHelperKey), targetArch,
                /* forceUpdate? */ true, /* values */ NULL)) {

            goto finish;
        }
    }

   /* Update per-directory caches. This is just KextIdentifiers any more.
    */
    count = CFArrayGetCount(systemExtensionsURLs);
    for (i = 0; i < count; i++) {

        folderURL = CFArrayGetValueAtIndex(systemExtensionsURLs, i);

        if (!CFURLGetFileSystemRepresentation(folderURL, /* resolveToBase */ true,
                    (UInt8 *)folderPath, sizeof(folderPath))) {

            OSKextLogStringError(/* kext */ NULL);
            goto finish;
        }
        if (EX_OK != updateDirectoryCaches(toolArgs, folderURL)) {
            directoryResult = EX_OSERR;
        } else {
            OSKextLog(/* kext */ NULL,
                kOSKextLogBasicLevel | kOSKextLogGeneralFlag,
                "Directory caches updated for %s.", folderPath);
        }
    }

    if (directoryResult == EX_OK) {
        result = EX_OK;
    }

finish:
    SAFE_RELEASE(kexts);
    SAFE_RELEASE(directoryValues);
    SAFE_RELEASE(personalities);

    OSKextSetArchitecture(startArch);

    return result;
}

/*******************************************************************************
*******************************************************************************/
ExitStatus updateDirectoryCaches(
        KextcacheArgs * toolArgs,
        CFURLRef        folderURL)
{
    ExitStatus         result           = EX_OK;  // optimistic!
    CFArrayRef         kexts            = NULL;   // must release

    kexts = OSKextCreateKextsFromURL(kCFAllocatorDefault, folderURL);
    if (!kexts) {
        result = EX_OSERR;
        goto finish;
    }

    if (!_OSKextWriteIdentifierCacheForKextsInDirectory(
                kexts, folderURL, /* force? */ true)) {
        result = EX_OSERR;
        goto finish;
    }

    result = EX_OK;

finish:
    SAFE_RELEASE(kexts);
    return result;
}

#pragma mark Misc Stuff

/*******************************************************************************
*******************************************************************************/
/*******************************************************************************
*******************************************************************************/
/* Open Firmware (PPC only) has an upper limit of 16MB on file transfers,
 * so we'll limit ourselves just beneath that.
 */
#define kOpenFirmwareMaxFileSize (16 * 1024 * 1024)

ExitStatus createMkext(
    KextcacheArgs * toolArgs,
    Boolean       * fatalOut)
{
    struct timeval    extDirsTimes[2];
    ExitStatus        result         = EX_SOFTWARE;
    CFMutableArrayRef archiveKexts   = NULL;  // must release
    CFMutableArrayRef mkexts         = NULL;  // must release
    CFDataRef         mkext          = NULL;  // must release
    const NXArchInfo *targetArch     = NULL;  // do not free
    int               i;

#if !NO_BOOT_ROOT
    /* Try a lock on the volume for the mkext being updated.
     * The lock prevents kextd from starting up a competing kextcache.
     */
    if (!getenv("_com_apple_kextd_skiplocks")) {
        // xxx - updateBoots + related should return only sysexit-type values, not errno
        result = takeVolumeForPath(toolArgs->mkextPath);
        if (result != EX_OK) {
            goto finish;
        }
    }
#endif /* !NO_BOOT_ROOT */

    if (!createCFMutableArray(&mkexts, &kCFTypeArrayCallBacks)) {
        OSKextLogMemError();
        result = EX_OSERR;
        *fatalOut = true;
        goto finish;
    }

    if (!createCFMutableArray(&archiveKexts, &kCFTypeArrayCallBacks)) {
        OSKextLogMemError();
        result = EX_OSERR;
        goto finish;
    }

    for (i = 0; i < CFArrayGetCount(toolArgs->targetArchs); i++) {
        targetArch = CFArrayGetValueAtIndex(toolArgs->targetArchs, i);

        SAFE_RELEASE_NULL(mkext);
        if (!OSKextSetArchitecture(targetArch)) {
            OSKextLog(/* kext */ NULL,
                    kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                    "Can't set architecture %s to create mkext.",
                    targetArch->name);
            result = EX_OSERR;
            goto finish;
        }

       /*****
        * Figure out which kexts we're actually archiving.
        */
        result = filterKextsForCache(toolArgs, archiveKexts,
                targetArch, fatalOut);
        if (result != EX_OK || *fatalOut) {
            goto finish;
        }

        if (!CFArrayGetCount(archiveKexts)) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogWarningLevel | kOSKextLogArchiveFlag,
                "No kexts found for architecture %s; skipping architecture.",
                targetArch->name);
            continue;
        }

        if (toolArgs->mkextVersion == 2) {
            mkext = OSKextCreateMkext(kCFAllocatorDefault, archiveKexts,
                    toolArgs->volumeRootURL,
                    kOSKextOSBundleRequiredNone, toolArgs->compress);
        } else if (toolArgs->mkextVersion == 1) {
            mkext = createMkext1ForArch(targetArch, archiveKexts,
                    toolArgs->compress);
        }
        if (!mkext) {
            // OSKextCreateMkext() logs an error
            result = EX_OSERR;
            goto finish;
        }
        if (targetArch == NXGetArchInfoFromName("ppc")) {
            if (CFDataGetLength(mkext) > kOpenFirmwareMaxFileSize) {
                OSKextLog(/* kext */ NULL,
                        kOSKextLogErrorLevel | kOSKextLogArchiveFlag,
                        "PPC archive is too large for Open Firmware; aborting.");
                result = EX_SOFTWARE;
                *fatalOut = true;
                goto finish;
            }
        }
        CFArrayAppendValue(mkexts, mkext);
    }

    if (!CFArrayGetCount(mkexts)) {
        OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogArchiveFlag,
                "No mkext archives created.");
        goto finish;
    }

    /* Get access and mod times of the extensions directory with most
     * recent mod time.  We now support multiple extensions directories.
     */
    if (toolArgs->extensionsDirTimes[1].tv_sec != 0) {
        result = getLatestTimesFromCFURLArray(toolArgs->repositoryURLs,
                                              extDirsTimes);
        if (result != EX_OK) {
            goto finish;
        }
        
        /* see if an extensions dir has been changed since we started */
        if (timercmp(&toolArgs->extensionsDirTimes[1], &extDirsTimes[1], !=)) {
            OSKextLog(/* kext */ NULL,
                      kOSKextLogErrorLevel | kOSKextLogGeneralFlag | kOSKextLogFileAccessFlag,
                      "An extensions dir has changed since starting; "
                      "not saving cache file");
            result = kKextcacheExitStale;
            goto finish;
        }
        /* bump kexts modtime by 1 second */
        extDirsTimes[1].tv_sec++;
    }

    result = writeFatFile(toolArgs->mkextPath, mkexts, toolArgs->targetArchs,
                          MKEXT_PERMS,
                          (toolArgs->extensionsDirTimes[1].tv_sec != 0) ? extDirsTimes : NULL);
    if (result != EX_OK) {
        goto finish;
    }

    result = EX_OK;
    OSKextLog(/* kext */ NULL,
        kOSKextLogBasicLevel | kOSKextLogGeneralFlag | kOSKextLogArchiveFlag,
        "Created mkext archive %s.", toolArgs->mkextPath);

finish:
    SAFE_RELEASE(archiveKexts);
    SAFE_RELEASE(mkexts);
    SAFE_RELEASE(mkext);

#if !NO_BOOT_ROOT
    putVolumeForPath(toolArgs->mkextPath, result);
#endif /* !NO_BOOT_ROOT */

    return result;
}

/*******************************************************************************
*******************************************************************************/
ExitStatus
getFileURLModTimePlusOne(
    CFURLRef            fileURL,
    struct timeval      *origModTime,
    struct timeval      cacheFileTimes[2])
{
    ExitStatus   result          = EX_SOFTWARE;
    char         path[PATH_MAX];

    if (!CFURLGetFileSystemRepresentation(fileURL, /* resolveToBase */ true,
            (UInt8 *)path, sizeof(path))) 
    {
        OSKextLogStringError(/* kext */ NULL);
        goto finish;
    }
    result = getFilePathModTimePlusOne(path, origModTime, cacheFileTimes);

finish:
    return result;
}

/*******************************************************************************
*******************************************************************************/
ExitStatus
getFilePathModTimePlusOne(
    const char        * filePath,
    struct timeval    * origModTime,
    struct timeval      cacheFileTimes[2])
{
    ExitStatus          result          = EX_SOFTWARE;

    result = getFilePathTimes(filePath, cacheFileTimes);
    if (result != EX_OK) {
        goto finish;
    }

    /* If asked, check to see if mod time has changed */
    if (origModTime != NULL) {
        if (timercmp(origModTime, &cacheFileTimes[1], !=)) {
            OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag | kOSKextLogFileAccessFlag,
            "Source item %s has changed since starting; "
                      "not saving cache file", filePath);
            result = kKextcacheExitStale;
            goto finish;
        }
    }

    /* bump modtime by 1 second */
    cacheFileTimes[1].tv_sec++;
    result = EX_OK;
finish:
    return result;
}

/*******************************************************************************
*******************************************************************************/
typedef struct {
    KextcacheArgs     * toolArgs;
    CFMutableArrayRef   kextArray;
    Boolean             error;
} FilterIDContext;

void filterKextID(const void * vValue, void * vContext)
{
    CFStringRef       kextID  = (CFStringRef)vValue;
    FilterIDContext * context = (FilterIDContext *)vContext;
    OSKextRef       theKext = OSKextGetKextWithIdentifier(kextID);

   /* This should really be a fatal error but embedded counts on
    * having optional kexts specified by identifier.
    */
    if (!theKext) {
        char kextIDCString[KMOD_MAX_NAME];
        
        CFStringGetCString(kextID, kextIDCString, sizeof(kextIDCString),
            kCFStringEncodingUTF8);
        OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                "Can't find kext with optional identifier %s; skipping.", kextIDCString);
#if 0
        OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                "Error - can't find kext with identifier %s.", kextIDCString);
        context->error = TRUE;
#endif /* 0 */
        goto finish;
    }

    if (kextMatchesFilter(context->toolArgs, theKext, 
            context->toolArgs->requiredFlagsAll) &&
        !CFArrayContainsValue(context->kextArray,
            RANGE_ALL(context->kextArray), theKext)) 
    {
        CFArrayAppendValue(context->kextArray, theKext);
    }

finish:
    return;    
}


/*******************************************************************************
*******************************************************************************/
ExitStatus filterKextsForCache(
        KextcacheArgs     * toolArgs,
        CFMutableArrayRef   kextArray,
        const NXArchInfo  * arch,
        Boolean           * fatalOut)
{
    ExitStatus          result        = EX_SOFTWARE;
    CFMutableArrayRef   firstPassArray = NULL;
    OSKextRequiredFlags requiredFlags;
    CFIndex             count, i;
    Boolean             kextSigningOnVol = false;

    if (!createCFMutableArray(&firstPassArray, &kCFTypeArrayCallBacks)) {
        OSKextLogMemError();
        goto finish;
    }
    
    kextSigningOnVol = isValidKextSigningTargetVolume(toolArgs->volumeRootURL);
    
   /*****
    * Apply filters to select the kexts.
    *
    * If kexts have been specified by identifier, those are the only kexts we are going to use.
    * Otherwise run through the repository and named kexts and see which ones match the filter.
    */
    if (CFSetGetCount(toolArgs->kextIDs)) {
        FilterIDContext context;

        context.toolArgs = toolArgs;
        context.kextArray = firstPassArray;
        context.error = FALSE;
        CFSetApplyFunction(toolArgs->kextIDs, filterKextID, &context);

        if (context.error) {
            goto finish;
        }

    } else {

       /* Set up the required flags for repository kexts. If any are set from
        * the command line, toss in "Root" and "Console" too.
        */
        requiredFlags = toolArgs->requiredFlagsRepositoriesOnly |
            toolArgs->requiredFlagsAll;
        if (requiredFlags) {
            requiredFlags |= kOSKextOSBundleRequiredRootFlag |
                kOSKextOSBundleRequiredConsoleFlag;
        }

        count = CFArrayGetCount(toolArgs->repositoryKexts);
        for (i = 0; i < count; i++) {

            OSKextRef theKext = (OSKextRef)CFArrayGetValueAtIndex(
                    toolArgs->repositoryKexts, i);

            if (!kextMatchesFilter(toolArgs, theKext, requiredFlags)) {

                char kextPath[PATH_MAX];

                if (!CFURLGetFileSystemRepresentation(OSKextGetURL(theKext),
                    /* resolveToBase */ false, (UInt8 *)kextPath, sizeof(kextPath))) 
                {
                    strlcpy(kextPath, "(unknown)", sizeof(kextPath));
                }

                if (toolArgs->mkextPath) {
                    OSKextLog(/* kext */ NULL,
                        kOSKextLogStepLevel | kOSKextLogArchiveFlag,
                        "%s does not match OSBundleRequired conditions; omitting.",
                        kextPath);
                } else if (toolArgs->prelinkedKernelPath) {
                    OSKextLog(/* kext */ NULL,
                        kOSKextLogStepLevel | kOSKextLogArchiveFlag,
                        "%s is not demanded by OSBundleRequired conditions.",
                        kextPath);
                }
                continue;
            }

            if (!CFArrayContainsValue(firstPassArray, RANGE_ALL(firstPassArray), theKext)) {
                _appendIfNewest(firstPassArray, theKext);
            }
        }

       /* Set up the required flags for named kexts. If any are set from
        * the command line, toss in "Root" and "Console" too.
        */
        requiredFlags = toolArgs->requiredFlagsAll;
        if (requiredFlags) {
            requiredFlags |= kOSKextOSBundleRequiredRootFlag |
                kOSKextOSBundleRequiredConsoleFlag;
        }

        count = CFArrayGetCount(toolArgs->namedKexts);
        for (i = 0; i < count; i++) {
            OSKextRef theKext = (OSKextRef)CFArrayGetValueAtIndex(
                    toolArgs->namedKexts, i);

            if (!kextMatchesFilter(toolArgs, theKext, requiredFlags)) {

                char kextPath[PATH_MAX];

                if (!CFURLGetFileSystemRepresentation(OSKextGetURL(theKext),
                    /* resolveToBase */ false, (UInt8 *)kextPath, sizeof(kextPath))) 
                {
                    strlcpy(kextPath, "(unknown)", sizeof(kextPath));
                }

                if (toolArgs->mkextPath) {
                    OSKextLog(/* kext */ NULL,
                        kOSKextLogStepLevel | kOSKextLogArchiveFlag,
                        "%s does not match OSBundleRequired conditions; omitting.",
                        kextPath);
                } else if (toolArgs->prelinkedKernelPath) {
                    OSKextLog(/* kext */ NULL,
                        kOSKextLogStepLevel | kOSKextLogArchiveFlag,
                        "%s is not demanded by OSBundleRequired conditions.",
                        kextPath);
                }
                continue;
            }

            if (!CFArrayContainsValue(firstPassArray, RANGE_ALL(firstPassArray), theKext)) {
                _appendIfNewest(firstPassArray, theKext);
            }
        }
    }

   /*****
    * Take all the kexts that matched the filters above and check them for problems.
    */
    CFArrayRemoveAllValues(kextArray);

    count = CFArrayGetCount(firstPassArray);
    if (count) {
        Boolean earlyBoot = false;
        
        if (callSecKeychainMDSInstall() != 0) {
            // this should never fail, so bail if it does.
            goto finish;
        }
        // not perfect, but we check to see if kextd is running to determine
        // if we are in early boot.
        earlyBoot = (isKextdRunning() == false);
        OSKextIsInExcludeList(NULL, false); // prime the exclude list cache
        isInExceptionList(NULL, NULL, false); // prime the exception list cache
        for (i = count - 1; i >= 0; i--) {
            OSStatus  sigResult;
            char kextPath[PATH_MAX];
            OSKextRef theKext = (OSKextRef)CFArrayGetValueAtIndex(
                    firstPassArray, i);

            if (!CFURLGetFileSystemRepresentation(OSKextGetURL(theKext),
                /* resolveToBase */ false, (UInt8 *)kextPath, sizeof(kextPath))) 
            {
                strlcpy(kextPath, "(unknown)", sizeof(kextPath));
            }

            /* Skip kexts we have no interest in for the current arch.
             */
            if (!OSKextSupportsArchitecture(theKext, arch)) {
                OSKextLog(/* kext */ NULL,
                    kOSKextLogStepLevel | kOSKextLogArchiveFlag,
                    "%s doesn't support architecture '%s'; skipping.", kextPath,
                    arch->name);
                continue;
            }

            if (!OSKextIsValid(theKext)) {
                // xxx - should also use kOSKextLogArchiveFlag?
                OSKextLog(/* kext */ NULL,
                    kOSKextLogErrorLevel | kOSKextLogArchiveFlag |
                    kOSKextLogValidationFlag | kOSKextLogGeneralFlag, 
                    "%s is not valid; omitting.", kextPath);
                if (toolArgs->printTestResults) {
                    OSKextLogDiagnostics(theKext, kOSKextDiagnosticsFlagAll);
                }
                continue;
            }

            if (!toolArgs->skipAuthentication && !OSKextIsAuthentic(theKext)) {
                OSKextLog(/* kext */ NULL,
                    kOSKextLogErrorLevel | kOSKextLogArchiveFlag |
                    kOSKextLogAuthenticationFlag | kOSKextLogGeneralFlag, 
                    "%s has incorrect permissions; omitting.", kextPath);
                if (toolArgs->printTestResults) {
                    OSKextLogDiagnostics(theKext, kOSKextDiagnosticsFlagAll);
                }
                continue;
            }

            if (OSKextIsInExcludeList(theKext, true)) {
                /* send alert about kext and message trace it
                 */
                addKextToAlertDict(&sExcludedKextAlertDict, theKext);
                messageTraceExcludedKext(theKext);
                OSKextLog(/* kext */ NULL,
                          kOSKextLogErrorLevel | kOSKextLogArchiveFlag |
                          kOSKextLogValidationFlag | kOSKextLogGeneralFlag, 
                          "%s is in exclude list; omitting.", kextPath);
                if (toolArgs->printTestResults) {
                    OSKextLogDiagnostics(theKext, kOSKextDiagnosticsFlagAll);
                }
                continue;
            }

            if (!OSKextResolveDependencies(theKext)) {
                OSKextLog(/* kext */ NULL,
                        kOSKextLogWarningLevel | kOSKextLogArchiveFlag |
                        kOSKextLogDependenciesFlag | kOSKextLogGeneralFlag, 
                        "%s is missing dependencies (including anyway; "
                        "dependencies may be available from elsewhere)", kextPath);
                if (toolArgs->printTestResults) {
                    OSKextLogDiagnostics(theKext, kOSKextDiagnosticsFlagAll);
                }
            }
 
            if (kextSigningOnVol
                && (sigResult = checkKextSignature(theKext, true, earlyBoot)) != 0 ) {
                
                if (isInvalidSignatureAllowed()) {
                    OSKextLogCFString(NULL,
                                      kOSKextLogErrorLevel | kOSKextLogLoadFlag,
                                      CFSTR("kext-dev-mode allowing invalid signature %ld 0x%02lX for kext %s"),
                                      (long)sigResult, (long)sigResult, kextPath);
                }
                else {
                    OSKextLog(/* kext */ NULL,
                              kOSKextLogErrorLevel | kOSKextLogArchiveFlag |
                              kOSKextLogAuthenticationFlag | kOSKextLogGeneralFlag,
                              "%s has invalid signature; omitting.",
                              kextPath);
                    if (toolArgs->printTestResults) {
                        OSKextLogDiagnostics(theKext, kOSKextDiagnosticsFlagAll);
                    }
                    continue;
                }
            }
            
            if (!CFArrayContainsValue(kextArray, RANGE_ALL(kextArray), theKext)) {
                CFArrayAppendValue(kextArray, theKext);
            }
        } // for loop...
    } // count > 0

    if (CFArrayGetCount(kextArray)) {
        recordKextLoadListForMT(kextArray);
    }

    result = EX_OK;

finish:
   return result;
}


/* Append the kext if it is the newest bundle / version.  Remove older if found.
 */
static void _appendIfNewest(CFMutableArrayRef theArray, OSKextRef theKext)
{
    CFStringRef     theBundleID;            // do not release
    CFStringRef     theBundleVersion;       // do not release
    OSKextVersion   theKextVersion = -1;
    CFIndex         myCount, i;
    
    theBundleID = OSKextGetIdentifier(theKext);
    theBundleVersion = OSKextGetValueForInfoDictionaryKey(
                                                          theKext,
                                                          kCFBundleVersionKey );
    if (theBundleVersion == NULL) {
        return;
    }
    theKextVersion = OSKextParseVersionCFString(theBundleVersion);
    if (theKextVersion == -1) {
        return;
    }
    
    myCount = CFArrayGetCount(theArray);
    for (i = 0; i < myCount; i++) {
        OSKextRef       myKext;             // do not release
        CFStringRef     myBundleID;         // do not release
        CFStringRef     myBundleVersion;    // do not release
        OSKextVersion   myKextVersion = -1;
        
        myKext = (OSKextRef) CFArrayGetValueAtIndex(theArray, i);
        myBundleID = OSKextGetIdentifier(myKext);
        
        if ( CFStringCompare(myBundleID, theBundleID, 0) == kCFCompareEqualTo ) {
            myBundleVersion = OSKextGetValueForInfoDictionaryKey(
                                                                 myKext,
                                                                 kCFBundleVersionKey );
            if (myBundleVersion == NULL)  continue;
            myKextVersion = OSKextParseVersionCFString(myBundleVersion);
            if (myKextVersion > 0 && myKextVersion > theKextVersion ) {
                // already have newer version of this kext, do not add it
                OSKextLogCFString(NULL,
                                  kOSKextLogDebugLevel | kOSKextLogArchiveFlag,
                                  CFSTR("%s: found newer, skipping %@"),
                                  __func__, theKext);
                return;
            }
            if (myKextVersion > 0 && myKextVersion == theKextVersion ) {
                // already have same version of this kext, do not add it
                OSKextLogCFString(NULL,
                                  kOSKextLogDebugLevel | kOSKextLogArchiveFlag,
                                  CFSTR("%s: found dup, skipping %@"),
                                  __func__, theKext);
                return;
            }
            if (myKextVersion > 0 && myKextVersion < theKextVersion ) {
                // found older version of this kext, remove it and add this one
                OSKextLogCFString(NULL,
                                  kOSKextLogDebugLevel | kOSKextLogArchiveFlag,
                                  CFSTR("%s: found older, removing %@"),
                                  __func__, myKext);
                CFArrayRemoveValueAtIndex(theArray, i);
                break;
            }
        }
    }
 
    CFArrayAppendValue(theArray, theKext);
    return;
}

/* We only want to check code signatures for volumes running 10.9 or
 * later version of OS (which means a Kernelcache v1.3 or later)
 */
static Boolean isValidKextSigningTargetVolume(CFURLRef theVolRootURL)
{
    Boolean             myResult          = false;
    CFDictionaryRef     myDict            = NULL;   // must release
    CFDictionaryRef     postBootPathsDict = NULL;   // do not release
    
    myDict = copyBootCachesDictForURL(theVolRootURL);
    if (myDict) {
        postBootPathsDict = (CFDictionaryRef)
            CFDictionaryGetValue(myDict, kBCPostBootKey);
            
        if (postBootPathsDict &&
            CFGetTypeID(postBootPathsDict) == CFDictionaryGetTypeID()) {
                
            if (CFDictionaryContainsKey(postBootPathsDict, kBCKernelcacheV3Key)) {
                myResult = true;
            }
        }
    }
    SAFE_RELEASE(myDict);
        
    return(myResult);
}

/* Make sure target volume can support fast (lzvn) compression, as well as current runtime library environment */

static Boolean wantsFastLibCompressionForTargetVolume(CFURLRef theVolRootURL)
{
    Boolean             myResult          = false;
    CFDictionaryRef     myDict            = NULL;   // must release
    CFDictionaryRef     postBootPathsDict = NULL;   // do not release
    CFDictionaryRef     kernelCacheDict   = NULL;   // do not release
    
    myDict = copyBootCachesDictForURL(theVolRootURL);
    if (myDict) {
        postBootPathsDict = (CFDictionaryRef)
            CFDictionaryGetValue(myDict, kBCPostBootKey);
        
        if (postBootPathsDict &&
            CFGetTypeID(postBootPathsDict) == CFDictionaryGetTypeID()) {
            
            kernelCacheDict = (CFDictionaryRef)
                CFDictionaryGetValue(postBootPathsDict, kBCKernelcacheV3Key);
            
            if (kernelCacheDict &&
                CFGetTypeID(kernelCacheDict) == CFDictionaryGetTypeID()) {
                CFStringRef     myTempStr;      // do not release
                
                myTempStr = (CFStringRef)
                CFDictionaryGetValue(kernelCacheDict,
                                     kBCPreferredCompressionKey);
                
                if (myTempStr && CFGetTypeID(myTempStr) == CFStringGetTypeID()) {
                    if (CFStringCompare(myTempStr, CFSTR("lzvn"), 0) == kCFCompareEqualTo) {
                        myResult = true;
                    }
                }
            } // kernelCacheDict
        } // postBootPathsDict
    } // myDict
    
    SAFE_RELEASE(myDict);
    
    /* We may not be able to generate FastLib-compressed files */
    if (myResult && !supportsFastLibCompression()) {
        myResult = false;
    }
    
    return(myResult);    
}

/*******************************************************************************
*******************************************************************************/
Boolean
kextMatchesFilter(
    KextcacheArgs             * toolArgs,
    OSKextRef                   theKext,
    OSKextRequiredFlags         requiredFlags)
{
    Boolean result = false;
    Boolean needLoadedKextInfo = toolArgs->needLoadedKextInfo &&
        (OSKextGetArchitecture() == OSKextGetRunningKernelArchitecture());

    if (needLoadedKextInfo) {
        result = (requiredFlags && OSKextMatchesRequiredFlags(theKext, requiredFlags)) ||
            CFArrayContainsValue(toolArgs->loadedKexts, RANGE_ALL(toolArgs->loadedKexts), theKext);
    } else {
        result = OSKextMatchesRequiredFlags(theKext, requiredFlags);
    }

    return result;
}

/*******************************************************************************
 * Creates a list of architectures to generate prelinked kernel slices for by
 * selecting the requested architectures for which the kernel has a slice.
 * Warns when a requested architecture does not have a corresponding kernel
 * slice.
 *******************************************************************************/
ExitStatus
createPrelinkedKernelArchs(
    KextcacheArgs     * toolArgs,
    CFMutableArrayRef * prelinkArchsOut)
{
    ExitStatus          result          = EX_OSERR;
    CFMutableArrayRef   kernelArchs     = NULL;  // must release
    CFMutableArrayRef   prelinkArchs    = NULL;  // must release
    const NXArchInfo  * targetArch      = NULL;  // do not free
    u_int               i               = 0;
    
    result = readFatFileArchsWithPath(toolArgs->kernelPath, &kernelArchs);
    if (result != EX_OK) {
        goto finish;
    }

    prelinkArchs = CFArrayCreateMutableCopy(kCFAllocatorDefault,
        /* capacity */ 0, toolArgs->targetArchs);
    if (!prelinkArchs) {
        OSKextLogMemError();
        result = EX_OSERR;
        goto finish;
    }

    for (i = 0; i < CFArrayGetCount(prelinkArchs); ++i) {
        targetArch = CFArrayGetValueAtIndex(prelinkArchs, i);
        if (!CFArrayContainsValue(kernelArchs, 
            RANGE_ALL(kernelArchs), targetArch)) 
        {
            OSKextLog(/* kext */ NULL,
                kOSKextLogWarningLevel | kOSKextLogArchiveFlag,
                "Kernel file %s does not contain requested arch: %s",
                toolArgs->kernelPath, targetArch->name);
            CFArrayRemoveValueAtIndex(prelinkArchs, i);
            i--;
            continue;
        }
    }

    *prelinkArchsOut = (CFMutableArrayRef) CFRetain(prelinkArchs);
    result = EX_OK;

finish:
    SAFE_RELEASE(kernelArchs);
    SAFE_RELEASE(prelinkArchs);

    return result;
}

/*******************************************************************************
 * If the existing prelinked kernel has a valid timestamp, this reads the slices
 * out of that prelinked kernel so we don't have to regenerate them.
 *******************************************************************************/
ExitStatus
createExistingPrelinkedSlices(
    KextcacheArgs     * toolArgs,
    CFMutableArrayRef * existingSlicesOut,
    CFMutableArrayRef * existingArchsOut)
{
    struct timeval      existingFileTimes[2];
    struct timeval      prelinkFileTimes[2];
    ExitStatus          result  = EX_SOFTWARE;

   /* If we aren't updating the system prelinked kernel, then we don't want
    * to reuse any existing slices.
    */
    if (!toolArgs->needDefaultPrelinkedKernelInfo) {
        result = EX_OK;
        goto finish;
    }

    bzero(&existingFileTimes, sizeof(existingFileTimes));
    bzero(&prelinkFileTimes, sizeof(prelinkFileTimes));

    result = getFilePathTimes(toolArgs->prelinkedKernelPath,
        existingFileTimes);
    if (result != EX_OK) {
        goto finish;
    }

    result = getExpectedPrelinkedKernelModTime(toolArgs, 
        prelinkFileTimes, NULL);
    if (result != EX_OK) {
        goto finish;
    }

    /* We are testing that the existing prelinked kernel still has a valid
     * timestamp by comparing it to the timestamp we are going to use for
     * the new prelinked kernel. If they are equal, we can reuse slices
     * from the existing prelinked kernel.
     */
    if (!timevalcmp(&existingFileTimes[1], &prelinkFileTimes[1], ==)) {
        result = EX_SOFTWARE;
        goto finish;
    }

    result = readMachOSlices(toolArgs->prelinkedKernelPath, 
        existingSlicesOut, existingArchsOut, NULL, NULL);
    if (result != EX_OK) {
        existingSlicesOut = NULL;
        existingArchsOut = NULL;
        goto finish;
    }

    result = EX_OK;

finish:
    return result;
}

/*******************************************************************************
*******************************************************************************/
ExitStatus
createPrelinkedKernel(
    KextcacheArgs     * toolArgs)
{
    ExitStatus          result              = EX_OSERR;
    struct timeval      prelinkFileTimes[2];
    CFMutableArrayRef   generatedArchs      = NULL;  // must release
    CFMutableArrayRef   generatedSymbols    = NULL;  // must release
    CFMutableArrayRef   existingArchs       = NULL;  // must release
    CFMutableArrayRef   existingSlices      = NULL;  // must release
    CFMutableArrayRef   prelinkArchs        = NULL;  // must release
    CFMutableArrayRef   prelinkSlices       = NULL;  // must release
    CFDataRef           prelinkSlice        = NULL;  // must release
    CFDictionaryRef     sliceSymbols        = NULL;  // must release
    const NXArchInfo  * targetArch          = NULL;  // do not free
    Boolean             updateModTime       = false;
    u_int               numArchs            = 0;
    u_int               i                   = 0;
    int                 j                   = 0;

    bzero(&prelinkFileTimes, sizeof(prelinkFileTimes));
        
#if !NO_BOOT_ROOT
    /* Try a lock on the volume for the prelinked kernel being updated.
     * The lock prevents kextd from starting up a competing kextcache.
     */
    if (!getenv("_com_apple_kextd_skiplocks")) {
        // xxx - updateBoots * related should return only sysexit-type values, not errno
        result = takeVolumeForPath(toolArgs->prelinkedKernelPath);
        if (result != EX_OK) {
            goto finish;
        }
    }
#endif /* !NO_BOOT_ROOT */

    result = createPrelinkedKernelArchs(toolArgs, &prelinkArchs);
    if (result != EX_OK) {
        goto finish;
    }
    numArchs = (u_int)CFArrayGetCount(prelinkArchs);

    /* If we're generating symbols, we'll regenerate all slices.
     */
    if (!toolArgs->symbolDirURL) {
        result = createExistingPrelinkedSlices(toolArgs,
            &existingSlices, &existingArchs);
        if (result != EX_OK) {
            SAFE_RELEASE_NULL(existingSlices);
            SAFE_RELEASE_NULL(existingArchs);
        }
    }

    prelinkSlices = CFArrayCreateMutable(kCFAllocatorDefault,
        numArchs, &kCFTypeArrayCallBacks);
    generatedSymbols = CFArrayCreateMutable(kCFAllocatorDefault, 
        numArchs, &kCFTypeArrayCallBacks);
    generatedArchs = CFArrayCreateMutable(kCFAllocatorDefault, 
        numArchs, NULL);
    if (!prelinkSlices || !generatedSymbols || !generatedArchs) {
        OSKextLogMemError();
        result = EX_OSERR;
        goto finish;
    }

    for (i = 0; i < numArchs; i++) {
        targetArch = CFArrayGetValueAtIndex(prelinkArchs, i);

        SAFE_RELEASE_NULL(prelinkSlice);
        SAFE_RELEASE_NULL(sliceSymbols);

       /* We always create a new prelinked kernel for the current
        * running architecture if asked, but we'll reuse existing slices
        * for other architectures if possible.
        */
        if (existingArchs && 
            targetArch != OSKextGetRunningKernelArchitecture())
        {
            j = (int)CFArrayGetFirstIndexOfValue(existingArchs,
                RANGE_ALL(existingArchs), targetArch);
            if (j != -1) {
                prelinkSlice = CFArrayGetValueAtIndex(existingSlices, j);
                CFArrayAppendValue(prelinkSlices, prelinkSlice);
                prelinkSlice = NULL;
                OSKextLog(/* kext */ NULL,
                    kOSKextLogDebugLevel | kOSKextLogArchiveFlag,
                    "Using existing prelinked slice for arch %s",
                    targetArch->name);
                continue;
            }
        }

        OSKextLog(/* kext */ NULL,
            kOSKextLogDebugLevel | kOSKextLogArchiveFlag,
            "Generating a new prelinked slice for arch %s",
            targetArch->name);

        result = createPrelinkedKernelForArch(toolArgs, &prelinkSlice,
            &sliceSymbols, targetArch);
        if (result != EX_OK) {
            goto finish;
        }

        CFArrayAppendValue(prelinkSlices, prelinkSlice);
        CFArrayAppendValue(generatedSymbols, sliceSymbols);
        CFArrayAppendValue(generatedArchs, targetArch);
    }

    result = getExpectedPrelinkedKernelModTime(toolArgs,
        prelinkFileTimes, &updateModTime);
    if (result != EX_OK) {
        goto finish;
    }

#if 0
    OSKextLogCFString(NULL,
                      kOSKextLogGeneralFlag | kOSKextLogErrorLevel,
                      CFSTR("%s: writing to %s"),
                      __func__, toolArgs->prelinkedKernelPath);
    if (updateModTime) {
        OSKextLogCFString(NULL,
                          kOSKextLogGeneralFlag | kOSKextLogErrorLevel,
                          CFSTR("%s: setting mod time to %ld"),
                          __func__, prelinkFileTimes[1].tv_sec);
    }
#endif

    result = writeFatFile(toolArgs->prelinkedKernelPath, prelinkSlices,
        prelinkArchs, MKEXT_PERMS, 
        (updateModTime) ? prelinkFileTimes : NULL);
    if (result != EX_OK) {
        goto finish;
    }
    
#if 1 // 17821398
    if (needsPrelinkedKernelCopy(toolArgs)) {
        CFMutableStringRef  myNewString = NULL;
        CFStringRef         myTempStr = NULL;
        CFIndex             myReplacedCount = 0;
        
        /* convert "kernelcache" to "prelinkedkernel", adjusting paths too.
         * We do best effort to make a copy, but failure is not fatal at this 
         * point.
         */
        do {
            myTempStr = CFStringCreateWithFileSystemRepresentation(
                                                nil,
                                                toolArgs->prelinkedKernelPath);
            if (myTempStr == NULL) break;
 
            myNewString = CFStringCreateMutableCopy(kCFAllocatorDefault,
                                                    0,
                                                    myTempStr);
            if (myNewString == NULL) break;
     
            /* We will replace:
             * "/System/Library/Caches/com.apple.kext.caches/Startup/kernelcache"
             * with:
             * "/System/Library/PrelinkedKernels/prelinkedkernel"
             * which will preserve any ".SUFFIX" like ".development"
             */
            myReplacedCount = CFStringFindAndReplace(
                        myNewString,
                        CFSTR(k_kernelcacheFilePath),     // find this string
                        CFSTR(k_prelinkedkernelFilePath), // replace with this
                        CFRangeMake(0, CFStringGetLength(myNewString)),
                        0);

            if (myReplacedCount == 1) {
                ExitStatus      myErr;
                char            tempbuf[PATH_MAX];
                
                if (CFStringGetFileSystemRepresentation(myNewString,
                                                        tempbuf,
                                                        sizeof(tempbuf)) == false) {
                    break;
                }
                
                /* now write another copy of prelinked kernel to new location at
                 * "/System/Library/PrelinkedKernels"
                 */
                myErr = writeFatFile(&tempbuf[0], prelinkSlices,
                                     prelinkArchs, MKEXT_PERMS,
                                     (updateModTime) ? prelinkFileTimes : NULL);
                if (myErr == EX_OK) {
                    OSKextLog(/* kext */ NULL,
                              kOSKextLogGeneralFlag | kOSKextLogBasicLevel,
                              "Created prelinked kernel copy \"%s\"",
                              tempbuf);
                }
            } // myReplacedCount
        } while(0);
        
        SAFE_RELEASE(myNewString);
        SAFE_RELEASE(myTempStr);
    }
#endif
    
    if (toolArgs->symbolDirURL) {
        result = writePrelinkedSymbols(toolArgs->symbolDirURL, 
            generatedSymbols, generatedArchs);
        if (result != EX_OK) {
            goto finish;
        }
    }
    
    OSKextLog(/* kext */ NULL,
              kOSKextLogGeneralFlag | kOSKextLogBasicLevel,
              "Created prelinked kernel \"%s\"",
              toolArgs->prelinkedKernelPath);
    if (toolArgs->kernelPath) {
        OSKextLog(/* kext */ NULL,
                  kOSKextLogGeneralFlag | kOSKextLogBasicLevel,
                  "Created prelinked kernel using \"%s\"",
                  toolArgs->kernelPath);
    }

    result = EX_OK;

finish:
    if (isKextdRunning() && isRootVolURL(toolArgs->volumeRootURL)) {
        // <rdar://problem/20688847> only post notifications if kextcache was
        // targeting the root volume
        if (sNoLoadKextAlertDict) {
            /* notify kextd that we have some nonsigned kexts going into the
             * kernel cache.
             */
            postNoteAboutKexts(CFSTR("No Load Kext Notification"),
                               sNoLoadKextAlertDict );
        }
        
        if (sRevokedKextAlertDict) {
            /* notify kextd that we have some kexts with revoked certificate.
             */
            postNoteAboutKexts(CFSTR("Revoked Cert Kext Notification"),
                               sRevokedKextAlertDict );
        }
        
        if (sInvalidSignedKextAlertDict) {
            /* notify kextd that we have some invalid signed kexts going into the
             * kernel cache.
             */
            postNoteAboutKexts(CFSTR("Invalid Signature Kext Notification"),
                               sInvalidSignedKextAlertDict);
        }
        
        if (sExcludedKextAlertDict) {
            /* notify kextd that we have some excluded kexts going into the
             * kernel cache.
             */
            postNoteAboutKexts(CFSTR("Excluded Kext Notification"),
                               sExcludedKextAlertDict);
        }
    }

    SAFE_RELEASE(generatedArchs);
    SAFE_RELEASE(generatedSymbols);
    SAFE_RELEASE(existingArchs);
    SAFE_RELEASE(existingSlices);
    SAFE_RELEASE(prelinkArchs);
    SAFE_RELEASE(prelinkSlices);
    SAFE_RELEASE(prelinkSlice);
    SAFE_RELEASE(sliceSymbols);

#if !NO_BOOT_ROOT
    putVolumeForPath(toolArgs->prelinkedKernelPath, result);
#endif /* !NO_BOOT_ROOT */

    return result;
}

/* NOTE -> Null URL means no /Volumes/XXX prefix was used, also a null string
 * in the URL is also treated as root volume
 */
static Boolean isRootVolURL(CFURLRef theURL)
{
    Boolean     result = false;
    char        volRootBuf[PATH_MAX];
    
    if (theURL == NULL) {
        result = true;
        goto finish;
    }
    
    volRootBuf[0] = 0x00;
    if (CFURLGetFileSystemRepresentation(theURL,
                                         true,
                                         (UInt8 *)volRootBuf,
                                         sizeof(volRootBuf)) == false) {
        // this should not happen, but just in case...
        volRootBuf[0] = 0x00;
    }
    if (strlen(volRootBuf) < 2) {
        // will count a null string also as root vole
        if (volRootBuf[0] == 0x00 || volRootBuf[0] == '/') {
            result = true;
        }
    }
    
finish:
    return(result);
    
}

/*******************************************************************************
*******************************************************************************/
ExitStatus createPrelinkedKernelForArch(
    KextcacheArgs       * toolArgs,
    CFDataRef           * prelinkedKernelOut,
    CFDictionaryRef     * prelinkedSymbolsOut,
    const NXArchInfo    * archInfo)
{
    ExitStatus result = EX_OSERR;
    CFMutableArrayRef prelinkKexts = NULL;
    CFDataRef kernelImage = NULL;
    CFDataRef prelinkedKernel = NULL;
    uint32_t flags = 0;
    Boolean fatalOut = false;
    Boolean kernelSupportsKASLR = false;
    macho_seek_result machoResult;
    const UInt8 * kernelStart;
    const UInt8 * kernelEnd;
    
    /* Retrieve the kernel image for the requested architecture.
     */
    kernelImage = readMachOSliceForArch(toolArgs->kernelPath, archInfo, /* checkArch */ TRUE);
    if (!kernelImage) {
        OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogArchiveFlag |  kOSKextLogFileAccessFlag,
                "Failed to read kernel file.");
        goto finish;
    }

    /* Set the architecture in the OSKext library */
    if (!OSKextSetArchitecture(archInfo)) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
            "Can't set architecture %s to create prelinked kernel.",
            archInfo->name);
        result = EX_OSERR;
        goto finish;
    }

   /*****
    * Figure out which kexts we're actually archiving.
    * This uses toolArgs->allKexts, which must already be created.
    */
    prelinkKexts = CFArrayCreateMutable(kCFAllocatorDefault, 0, 
        &kCFTypeArrayCallBacks);
    if (!prelinkKexts) {
        OSKextLogMemError();
        result = EX_OSERR;
        goto finish;
    }

    result = filterKextsForCache(toolArgs, prelinkKexts,
            archInfo, &fatalOut);
    if (result != EX_OK || fatalOut) {
        goto finish;
    }

    result = EX_OSERR;

    if (!CFArrayGetCount(prelinkKexts)) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogArchiveFlag,
            "No kexts found for architecture %s.",
            archInfo->name);
        goto finish;
    }

   /* Create the prelinked kernel from the given kernel and kexts */

    flags |= (toolArgs->noLinkFailures) ? kOSKextKernelcacheNeedAllFlag : 0;
    flags |= (toolArgs->skipAuthentication) ? kOSKextKernelcacheSkipAuthenticationFlag : 0;
    flags |= (toolArgs->printTestResults) ? kOSKextKernelcachePrintDiagnosticsFlag : 0;
    flags |= (toolArgs->includeAllPersonalities) ? kOSKextKernelcacheIncludeAllPersonalitiesFlag : 0;
    flags |= (toolArgs->stripSymbols) ? kOSKextKernelcacheStripSymbolsFlag : 0;
        
    kernelStart = CFDataGetBytePtr(kernelImage);
    kernelEnd = kernelStart + CFDataGetLength(kernelImage) - 1;
    machoResult = macho_find_dysymtab(kernelStart, kernelEnd, NULL);
    /* this kernel supports KASLR if there is a LC_DYSYMTAB load command */
    kernelSupportsKASLR = (machoResult == macho_seek_result_found);
    if (kernelSupportsKASLR) {
        flags |= kOSKextKernelcacheKASLRFlag;
    }
        
    prelinkedKernel = OSKextCreatePrelinkedKernel(kernelImage, prelinkKexts,
        toolArgs->volumeRootURL, flags, prelinkedSymbolsOut);
    if (!prelinkedKernel) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogArchiveFlag,
            "Failed to generate prelinked kernel.");
        result = EX_OSERR;
        goto finish;
    }

   /* Compress the prelinked kernel if needed */

    if (toolArgs->compress) {
        Boolean     wantsFastLib = wantsFastLibCompressionForTargetVolume(toolArgs->volumeRootURL);
        uint32_t    compressionType = wantsFastLib ? COMP_TYPE_FASTLIB : COMP_TYPE_LZSS;
        
        *prelinkedKernelOut = compressPrelinkedSlice(compressionType,
                                                     prelinkedKernel,
                                                     kernelSupportsKASLR);
    } else {
        *prelinkedKernelOut = CFRetain(prelinkedKernel);
    }
    
    if (!*prelinkedKernelOut) {
        goto finish;
    }

    result = EX_OK;

finish:
    SAFE_RELEASE(kernelImage);
    SAFE_RELEASE(prelinkKexts);
    SAFE_RELEASE(prelinkedKernel);

    return result;
}

/*****************************************************************************
 *****************************************************************************/
ExitStatus
getExpectedPrelinkedKernelModTime(
    KextcacheArgs  * toolArgs,
    struct timeval   cacheFileTimes[2],
    Boolean        * updateModTimeOut)
{
    struct timeval  kextTimes[2];
    struct timeval  kernelTimes[2];
    ExitStatus      result          = EX_SOFTWARE;
    Boolean         updateModTime   = false; 

    /* bail out if we don't have modtimes for extensions directory or kernel file
     */
    if (toolArgs->extensionsDirTimes[1].tv_sec == 0 ||
        toolArgs->kernelTimes[1].tv_sec == 0) {
        result = EX_OK;
        goto finish;
    }
        
    result = getLatestTimesFromCFURLArray(toolArgs->repositoryURLs,
                                          kextTimes);
    if (result != EX_OK) {
        goto finish;
    }

    /* bump kexts modtime by 1 second */
    kextTimes[1].tv_sec++;

    /* Check kernel mod time */
    result = getFilePathModTimePlusOne(toolArgs->kernelPath,
                                       &toolArgs->kernelTimes[1], kernelTimes);
    if (result != EX_OK) {
        goto finish;
    }
#if 0
    OSKextLogCFString(NULL,
                      kOSKextLogGeneralFlag | kOSKextLogErrorLevel,
                      CFSTR("%s: kernelPath %s"),
                      __func__, toolArgs->kernelPath);
    OSKextLogCFString(NULL,
                      kOSKextLogGeneralFlag | kOSKextLogErrorLevel,
                      CFSTR("%s: %ld <- latest kext mod time"),
                      __func__, kextTimes[1].tv_sec);
    OSKextLogCFString(NULL,
                      kOSKextLogGeneralFlag | kOSKextLogErrorLevel,
                      CFSTR("%s: %ld <- latest kernels mod time"),
                      __func__, kernelTimes[1].tv_sec);
#endif

    /* Get the access and mod times of the latest modified of the kernel,
     * or kext repositories.  For example:
     * kextTimes -> /System/Library/Extensions/ and /Library/Extensions/
     * kernelTimes -> /System/Library/Kernels/kernel
     * cacheFileTimes -> /S/L/Caches/com.apple.kext.caches/Startup/kernelcache
     */
    cacheFileTimes[0].tv_sec = kextTimes[0].tv_sec;     // access time
    cacheFileTimes[0].tv_usec = kextTimes[0].tv_usec;
    cacheFileTimes[1].tv_sec = kextTimes[1].tv_sec;     // mod time
    cacheFileTimes[1].tv_usec = kextTimes[1].tv_usec;
    if (timercmp(&kernelTimes[1], &kextTimes[1], >)) {
        cacheFileTimes[0].tv_sec = kernelTimes[0].tv_sec;   // access time
        cacheFileTimes[0].tv_usec = kernelTimes[0].tv_usec;
        cacheFileTimes[1].tv_sec = kernelTimes[1].tv_sec;   // mod time
        cacheFileTimes[1].tv_usec = kernelTimes[1].tv_usec;
    }
#if 0
    OSKextLogCFString(NULL,
                      kOSKextLogGeneralFlag | kOSKextLogErrorLevel,
                      CFSTR("%s: %ld <- using this mod time"),
                      __func__, cacheFileTimes[1].tv_sec);
#endif

    /* Set the mod time of the kernelcache relative to the kernel */
    updateModTime = true;
    result = EX_OK;

finish:
    if (updateModTimeOut) *updateModTimeOut = updateModTime;

    return result;
}

/*********************************************************************
 *********************************************************************/
ExitStatus
compressPrelinkedKernel(
                        CFURLRef            volumeRootURL,
                        const char        * prelinkPath,
                        Boolean             compress)
{
    ExitStatus          result          = EX_SOFTWARE;
    struct timeval      prelinkedKernelTimes[2];
    CFMutableArrayRef   prelinkedSlices = NULL; // must release
    CFMutableArrayRef   prelinkedArchs  = NULL; // must release
    CFDataRef           prelinkedSlice  = NULL; // must release
   const NXArchInfo  * archInfo         = NULL; // do not free
    const u_char      * sliceBytes      = NULL; // do not free
    mode_t              fileMode        = 0;
    int                 i               = 0;
    
    result = readMachOSlices(prelinkPath, &prelinkedSlices, 
        &prelinkedArchs, &fileMode, prelinkedKernelTimes);
    if (result != EX_OK) {
        goto finish;
    }
    
    /* Compress/uncompress each slice of the prelinked kernel.
     */

    for (i = 0; i < CFArrayGetCount(prelinkedSlices); ++i) {

        SAFE_RELEASE_NULL(prelinkedSlice);
        prelinkedSlice = CFArrayGetValueAtIndex(prelinkedSlices, i);

        if (compress) {
            const PrelinkedKernelHeader *header = (const PrelinkedKernelHeader *) 
                CFDataGetBytePtr(prelinkedSlice);
            Boolean     wantsFastLib = wantsFastLibCompressionForTargetVolume(volumeRootURL);
            uint32_t    compressionType = wantsFastLib ? COMP_TYPE_FASTLIB : COMP_TYPE_LZSS;
            
            
            prelinkedSlice = compressPrelinkedSlice(compressionType,
                                                    prelinkedSlice,
                                                    (OSSwapHostToBigInt32(header->prelinkVersion) == 1));
            if (!prelinkedSlice) {
                result = EX_DATAERR;
                goto finish;
            }
        } else {
            prelinkedSlice = uncompressPrelinkedSlice(prelinkedSlice);
            if (!prelinkedSlice) {
                result = EX_DATAERR;
                goto finish;
            }
        }

        CFArraySetValueAtIndex(prelinkedSlices, i, prelinkedSlice);
    }
    SAFE_RELEASE_NULL(prelinkedSlice);

    /* Snow Leopard prelinked kernels are not wrapped in a fat header, so we
     * have to decompress the prelinked kernel and look at the mach header
     * to get the architecture information.
     */

    if (!prelinkedArchs && CFArrayGetCount(prelinkedSlices) == 1) {
        if (!createCFMutableArray(&prelinkedArchs, NULL)) {
            OSKextLogMemError();
            result = EX_OSERR;
            goto finish;
        }

        sliceBytes = CFDataGetBytePtr(
            CFArrayGetValueAtIndex(prelinkedSlices, 0));

        archInfo = getThinHeaderPageArch(sliceBytes);
        if (archInfo) {
            CFArrayAppendValue(prelinkedArchs, archInfo);
        } else {
            SAFE_RELEASE_NULL(prelinkedArchs);
        }
    }

    /* If we still don't have architecture information, then something
     * definitely went wrong.
     */

    if (!prelinkedArchs) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogArchiveFlag,
            "Couldn't determine prelinked kernel's architecture");
        result = EX_SOFTWARE;
        goto finish;
    }

    result = writeFatFile(prelinkPath, prelinkedSlices, 
        prelinkedArchs, fileMode, prelinkedKernelTimes);
    if (result != EX_OK) {
        goto finish;
    }

    result = EX_OK;

finish:
    SAFE_RELEASE(prelinkedSlices);
    SAFE_RELEASE(prelinkedArchs);
    SAFE_RELEASE(prelinkedSlice);
    
    return result;
}

#pragma mark Boot!=Root


/*******************************************************************************
* usage()
*******************************************************************************/
void usage(UsageLevel usageLevel)
{
    fprintf(stderr,
      "usage: %1$s <mkext_flag> [options] [--] [kext or directory] ...\n"
      "       %1$s -prelinked-kernel <filename> [options] [--] [kext or directory]\n"
      "       %1$s -system-prelinked-kernel\n"
      "       %1$s [options] -prelinked-kernel\n"
#if !NO_BOOT_ROOT
    "       %1$s -invalidate <volume> \n"
    "       %1$s -update-volume <volume> [options]\n"
#endif /* !NO_BOOT_ROOT */
      "       %1$s -system-caches [options]\n"
      "\n",
      progname);

    if (usageLevel == kUsageLevelBrief) {
        fprintf(stderr, "use %s -%s for an explanation of each option\n",
            progname, kOptNameHelp);
    }

    if (usageLevel == kUsageLevelBrief) {
        return;
    }

    fprintf(stderr, "-%s <filename>: create an mkext (latest supported version)\n",
        kOptNameMkext);
    fprintf(stderr, "-%s <filename>: create an mkext (version 2)\n",
        kOptNameMkext2);
    fprintf(stderr, "-%s <filename> (-%c): create an mkext (version 1)\n",
        kOptNameMkext1, kOptMkext);
    fprintf(stderr, "-%s [<filename>] (-%c):\n"
        "        create/update prelinked kernel (must be last if no filename given)\n",
        kOptNamePrelinkedKernel, kOptPrelinkedKernel);
    fprintf(stderr, "-%s:\n"
        "        create/update system prelinked kernel\n",
        kOptNameSystemPrelinkedKernel);
#if !NO_BOOT_ROOT
    fprintf(stderr, "-%s <volume> (-%c): invalidate system kext caches for <volume>\n",
            kOptNameInvalidate, kOptInvalidate);
    fprintf(stderr, "-%s <volume> (-%c): update system kext caches for <volume>\n",
            kOptNameUpdate, kOptUpdate);
    fprintf(stderr, "-%s called us, modify behavior appropriately\n",
            kOptNameInstaller);
    fprintf(stderr, "-%s skips updating any helper partitions even if they appear out of date\n",
            kOptNameCachesOnly);
#endif /* !NO_BOOT_ROOT */
#if 0
// don't print this system-use option
    fprintf(stderr, "-%c <volume>:\n"
        "        check system kext caches for <volume> (nonzero exit if out of date)\n",
        kOptCheckUpdate);
#endif
    fprintf(stderr, "-%s: update system kext info caches for the root volume\n",
        kOptNameSystemCaches);
    fprintf(stderr, "\n");

    fprintf(stderr,
        "kext or directory: Consider kext or all kexts in directory for inclusion\n");
    fprintf(stderr, "-%s <bundle_id> (-%c):\n"
        "        include the kext whose CFBundleIdentifier is <bundle_id>\n",
        kOptNameBundleIdentifier, kOptBundleIdentifier);
    fprintf(stderr, "-%s <volume>:\n"
        "        Save kext paths in an mkext archive or prelinked kernel "
        " relative to <volume>\n",
        kOptNameVolumeRoot);
    fprintf(stderr, "-%s <kernel_filename> (-%c): Use kernel_filename for a prelinked kernel\n",
        kOptNameKernel, kOptKernel);
    fprintf(stderr, "-%s (-%c): Include all kexts ever loaded in prelinked kernel\n",
        kOptNameAllLoaded, kOptAllLoaded);
#if !NO_BOOT_ROOT
    fprintf(stderr, "-%s (-%c): Update volumes even if they look up to date\n",
        kOptNameForce, kOptForce);
    fprintf(stderr, "\n");
#endif /* !NO_BOOT_ROOT */

    fprintf(stderr, "-%s (-%c): Add 'Local-Root' kexts from directories to an mkext file\n",
        kOptNameLocalRoot, kOptLocalRoot);
    fprintf(stderr, "-%s (-%c): Add 'Local-Root' kexts to an mkext file\n",
        kOptNameLocalRootAll, kOptLocalRootAll);
    fprintf(stderr, "-%s (-%c): Add 'Network-Root' kexts from directories to an mkext file\n",
        kOptNameNetworkRoot, kOptNetworkRoot);
    fprintf(stderr, "-%s (-%c): Add 'Network-Root' kexts to an mkext file\n",
        kOptNameNetworkRootAll, kOptNetworkRootAll);
    fprintf(stderr, "-%s (-%c): Add 'Safe Boot' kexts from directories to an mkext file\n",
        kOptNameSafeBoot, kOptSafeBoot);
    fprintf(stderr, "-%s (-%c): Add 'Safe Boot' kexts to an mkext file\n",
        kOptNameSafeBootAll, kOptSafeBootAll);
    fprintf(stderr, "\n");

    fprintf(stderr, "-%s <archname>:\n"
        "        include architecture <archname> in created cache(s)\n",
        kOptNameArch);
    fprintf(stderr, "-%c: run at low priority\n",
        kOptLowPriorityFork);
    fprintf(stderr, "\n");

    fprintf(stderr, "-%s (-%c): quiet mode: print no informational or error messages\n",
        kOptNameQuiet, kOptQuiet);
    fprintf(stderr, "-%s [ 0-6 | 0x<flags> ] (-%c):\n"
        "        verbose mode; print info about analysis & loading\n",
        kOptNameVerbose, kOptVerbose);
    fprintf(stderr, "\n");

    fprintf(stderr, "-%s (-%c):\n"
        "        print diagnostics for kexts with problems\n",
        kOptNameTests, kOptTests);
    fprintf(stderr, "-%s (-%c): don't authenticate kexts (for use during development)\n",
        kOptNameNoAuthentication, kOptNoAuthentication);
    fprintf(stderr, "\n");
    
    fprintf(stderr, "-%s (-%c): print this message and exit\n",
        kOptNameHelp, kOptHelp);

    return;
}

#if 1 // 17821398
static Boolean needsPrelinkedKernelCopy( KextcacheArgs * toolArgs )
{
    Boolean     volSupportsIt = false;
    Boolean     isCorrectPrefix = false;
    Boolean     prelinkedKernelsExists = false;
    char        volRootBuf[PATH_MAX];
    char        tempBuf[PATH_MAX];
   
    volSupportsIt = wantsPrelinkedKernelCopy(toolArgs->volumeRootURL);
    
    while (volSupportsIt) {
        /* Now see if the toolArgs->prelinkedKernelPath path starts with
         * "/System/Library/Caches/com.apple.kext.caches/Startup/kernelcache"
         * We do not want to copy custom kernelcache files.
         */
        volRootBuf[0] = 0x00;
        if (toolArgs->volumeRootURL) {
            if (CFURLGetFileSystemRepresentation(toolArgs->volumeRootURL,
                                                 true,
                                                 (UInt8 *)volRootBuf,
                                                 sizeof(volRootBuf)) == false) {
                // this should not happen, but just in case...
                volRootBuf[0] = 0x00;
            }
        }
        
        /* handle case where there is no volumeRootURL or if the root is just "/" */
        if (strlen(volRootBuf) > 1) {
            strlcpy(tempBuf, volRootBuf, sizeof(tempBuf));
            if (strlcat(tempBuf, k_kernelcacheFilePath, sizeof(tempBuf)) >= sizeof(tempBuf)) {
                // overflow
                break;
            }
        }
        else {
            strlcpy(tempBuf, k_kernelcacheFilePath, sizeof(tempBuf));
        }
        
        char *      myPrefix;
        myPrefix = strnstr(toolArgs->prelinkedKernelPath,
                           &tempBuf[0],
                           strlen(&tempBuf[0]));
        isCorrectPrefix = (myPrefix != NULL);
        if (isCorrectPrefix == false) break;
        
        /* make sure "/System/Library/PrelinkedKernels" exists
         */
        if (strlen(volRootBuf) > 1) {
            strlcpy(tempBuf, volRootBuf, sizeof(tempBuf));
            if (strlcat(tempBuf, kPrelinkedKernelsPath, sizeof(tempBuf)) >= sizeof(tempBuf)) {
                // overflow
                break;
            }
        }
        else {
            strlcpy(tempBuf, kPrelinkedKernelsPath, sizeof(tempBuf));
        }
        
        struct stat         statBuf;
        if (statPath(tempBuf, &statBuf) == EX_OK) {
            prelinkedKernelsExists = true;
        }
        else {
            /* need to create System/Library/PrelinkedKernels/ */
            int         my_fd;
            
            my_fd = open(toolArgs->prelinkedKernelPath, O_RDONLY);
            if (my_fd != -1) {
                if (smkdir(my_fd, tempBuf, 0755) == 0) {
                    prelinkedKernelsExists = true;
                }
                close(my_fd);
            }
        }
        break;
    } // while (volSupportsIt)...
    
    return(volSupportsIt && isCorrectPrefix && prelinkedKernelsExists);
}

/* Make sure target volume wants a copy of the prelinked kernel in
 *      /System/Library/PrelinkedKernels/prelinkedkernel
 * This is a temporary hack, using the Yosemite added support for lzvn
 * compression to mean "wants /System/Library/PrelinkedKernels".
 *
 * We will update the path key / value in bootcaches.plist in Yosemite + 1
 */
static Boolean wantsPrelinkedKernelCopy(CFURLRef theVolRootURL)
{
    return(wantsFastLibCompressionForTargetVolume(theVolRootURL));
}

#endif

