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
 * FILE: bootcaches.c
 * AUTH: Soren Spies (sspies)
 * DATE: "spring" 2006
 * DESC: routines for bootcache data
 *
 */

#include <bless.h>
#include <bootfiles.h>
#include <IOKit/IOKitLib.h>
#include <fcntl.h>
#include <libgen.h>
#include <notify.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/kmod.h>

#include <Kernel/IOKit/IOKitKeysPrivate.h>
#include <Kernel/libsa/mkext.h>
#include <TargetConditionals.h>
#if !TARGET_OS_EMBEDDED
#include <DiskArbitration/DiskArbitration.h>        // for UUID fetching
#include <DiskArbitration/DiskArbitrationPrivate.h> // path -> DADisk
#endif
#include <IOKit/kext/fat_util.h>
#include <IOKit/kext/macho_util.h>
#include <IOKit/kext/kernel_check.h>

#include "bootcaches.h"	    // includes CF
#include "globals.h"
#include "logging.h"
#include "mkext_util.h"
#include "safecalls.h"
#include "utility.h"


static MkextCRCResult getMkextCRC(const char * file_path, uint32_t * crc_ptr);

// X these could take a label/action as their third parameter
#define pathcpy(dst, src) do { \
        if (strlcpy(dst, src, PATH_MAX) >= PATH_MAX)  goto finish; \
    } while(0)
#define pathcat(dst, src) do { \
        if (strlcat(dst, src, PATH_MAX) >= PATH_MAX)  goto finish; \
    } while(0)

/******************************************************************************
* destroyCaches cleans up a bootCaches structure
******************************************************************************/
void destroyCaches(struct bootCaches *caches)
{
    if (caches->cachefd != -1)  close(caches->cachefd);
    if (caches->cacheinfo)      CFRelease(caches->cacheinfo);
    if (caches->miscpaths)      free(caches->miscpaths);  // free all strings
    if (caches->rpspaths)       free(caches->rpspaths);
    free(caches);
}

/******************************************************************************
* readCaches checks for and reads bootcaches.plist
******************************************************************************/
// used for turning /foo/bar into :foo:bar for kTSCacheDir entries (see awk(1))
static void gsub(char old, char new, char *s)
{
    char *p;

    while((p = s++) && *p)
        if (*p == old)
            *p = new;
}

// fillCachedPath not currently used beyond this module, but it is in the header
int fillCachedPath(cachedPath *cpath, char *uuidchars, char *relpath)
{
    int rval = ELAST + 1;

    if (strlcat(cpath->tspath, kTSCacheDir, PATH_MAX) >= PATH_MAX) goto finish;
    pathcat(cpath->tspath, uuidchars);
    pathcat(cpath->tspath, "/");

    // now append the actual path and stamp name
    if (strlcat(cpath->rpath, relpath, PATH_MAX) >= PATH_MAX) goto finish;
    gsub('/', ':', relpath);
    if (strlcat(cpath->tspath, relpath, PATH_MAX) >= PATH_MAX) goto finish;

    rval = 0;

finish:
    return rval;
}

// wrap fillCachedPath() with the local idiom
#define str2cachedPath(cpath, caches, relstr) \
do { \
    char relpath[PATH_MAX]; \
\
    if (!CFStringGetFileSystemRepresentation(relstr, relpath, PATH_MAX)) \
        goto finish; \
    if (fillCachedPath(cpath, caches->uuid_str, relpath))  goto finish; \
} while(0)

// parse bootcaches.plist and dadisk dictionaries into passed struct
// caller populates fields it needed to load the plist
// and properly frees the structure if we fail
static int
finishParse(struct bootCaches *caches, CFDictionaryRef bcDict,
           CFDictionaryRef ddesc, char **errmsg)
{
    int rval = ELAST + 1;
    CFDictionaryRef dict;   // don't release
    CFIndex keyCount;       // track whether we've handled all keys
    CFStringRef str;        // used to point to objects owned by others
    CFStringRef createdStr = NULL;
    CFUUIDRef uuid;

    *errmsg = "error getting disk metadata";
    // volume UUID, name, bsdname
    if (!(uuid = CFDictionaryGetValue(ddesc, kDADiskDescriptionVolumeUUIDKey)))
        goto finish;
    if (!(createdStr = CFUUIDCreateString(nil, uuid)))
        goto finish;
     if (!CFStringGetFileSystemRepresentation(createdStr,caches->uuid_str,NCHARSUUID))
        goto finish;

    if (!(str = CFDictionaryGetValue(ddesc, kDADiskDescriptionVolumeNameKey)))
        goto finish;
    if (!CFStringGetFileSystemRepresentation(str, caches->volname, NAME_MAX))
        goto finish;

    // bsdname still needed for bless
    if (!(str = CFDictionaryGetValue(ddesc, kDADiskDescriptionMediaBSDNameKey)))
        goto finish;
    if (!CFStringGetFileSystemRepresentation(str, caches->bsdname, NAME_MAX))
        goto finish;


    *errmsg = "unsupported bootcaches data";    // covers the rest
    keyCount = CFDictionaryGetCount(bcDict);    // start with the top

    // process keys for paths read "before the booter"
    dict = (CFDictionaryRef)CFDictionaryGetValue(bcDict, kBCPreBootKey);
    if (dict) {
        CFArrayRef apaths;
        CFIndex miscindex = 0;

        if (CFGetTypeID(dict) != CFDictionaryGetTypeID())  goto finish;
        caches->nmisc = CFDictionaryGetCount(dict);     // >= 1 path / key
        keyCount += CFDictionaryGetCount(dict);

        // variable-sized member first
        apaths = (CFArrayRef)CFDictionaryGetValue(dict, kBCAdditionalPathsKey);
        if (apaths) {
	    CFIndex acount;

	    if (CFArrayGetTypeID() != CFGetTypeID(apaths))  goto finish;
	    acount = CFArrayGetCount(apaths);
	    // total "misc" paths = # of keyed paths + # additional paths
	    caches->nmisc += acount - 1;   // replacing array in misc count

	    if (caches->nmisc > INT_MAX/sizeof(*caches->miscpaths)) goto finish;
	    caches->miscpaths = (cachedPath*)calloc(caches->nmisc,
		    sizeof(*caches->miscpaths));
	    if (!caches->miscpaths)  goto finish;

	    for (/*miscindex = 0 (above)*/; miscindex < acount; miscindex++) {
		str = CFArrayGetValueAtIndex(apaths, miscindex);
		if (CFGetTypeID(str) != CFStringGetTypeID())  goto finish;

		str2cachedPath(&caches->miscpaths[miscindex], caches, str);  // M
	    }
	    keyCount--;	// AdditionalPaths sub-key
	} else {
	    // allocate enough for the top-level keys (nothing variable-sized)
	    if (caches->nmisc > INT_MAX/sizeof(*caches->miscpaths)) goto finish;
	    caches->miscpaths = calloc(caches->nmisc, sizeof(cachedPath));
	    if (!caches->miscpaths)	goto finish;
	}

	str = (CFStringRef)CFDictionaryGetValue(dict, kBCLabelKey);
	if (str) {
	    if (CFGetTypeID(str) != CFStringGetTypeID())  goto finish;
	    str2cachedPath(&caches->miscpaths[miscindex], caches, str); // macro
	    caches->label = &caches->miscpaths[miscindex];

	    miscindex++;	    // get ready for the next guy
        keyCount--;	    // DiskLabel is dealt with
	}

	// add new keys here
	keyCount--;	// preboot dict
    }

    // process booter keys
    dict = (CFDictionaryRef)CFDictionaryGetValue(bcDict, kBCBootersKey);
    if (dict) {
	if (CFGetTypeID(dict) != CFDictionaryGetTypeID())  goto finish;
	keyCount += CFDictionaryGetCount(dict);

	str = (CFStringRef)CFDictionaryGetValue(dict, kBCEFIBooterKey);
	if (str) {
	    if (CFGetTypeID(str) != CFStringGetTypeID())  goto finish;
	    str2cachedPath(&caches->efibooter, caches, str);  // macro

	    keyCount--;	    // EFIBooter is dealt with
	}

	str = (CFStringRef)CFDictionaryGetValue(dict, kBCOFBooterKey);
	if (str) {
	    if (CFGetTypeID(str) != CFStringGetTypeID())  goto finish;
	    str2cachedPath(&caches->ofbooter, caches, str);  // macro

	    keyCount--;	    // BootX, check
	}

	// add new booters here
	keyCount--;	// booters dict
    }

    dict = (CFDictionaryRef)CFDictionaryGetValue(bcDict, kBCPostBootKey);
    if (dict) {
	CFDictionaryRef mkDict;
	CFArrayRef apaths;
	CFIndex rpsindex = 0;

	if (CFGetTypeID(dict) != CFDictionaryGetTypeID())  goto finish;
	keyCount += CFDictionaryGetCount(dict);
	caches->nrps = CFDictionaryGetCount(dict);	// >= 1 path / key

	// variable-sized member first
	apaths = (CFArrayRef)CFDictionaryGetValue(dict, kBCAdditionalPathsKey);
	if (apaths) {
	    CFIndex acount;

	    if (CFArrayGetTypeID() != CFGetTypeID(apaths))  goto finish;
	    acount = CFArrayGetCount(apaths);
	    // total rps paths = # of keyed paths + # additional paths
	    caches->nrps += acount - 1;   // replace array w/contents in nrps

	    if (caches->nrps > INT_MAX/sizeof(*caches->rpspaths)) goto finish;
	    caches->rpspaths = (cachedPath*)calloc(caches->nrps,
		    sizeof(*caches->rpspaths));
	    if (!caches->rpspaths)  goto finish;

	    for (; rpsindex < acount; rpsindex++) {
		str = CFArrayGetValueAtIndex(apaths, rpsindex);
		if (CFGetTypeID(str) != CFStringGetTypeID())  goto finish;

		str2cachedPath(&caches->rpspaths[rpsindex], caches, str); // M
	    }
	    keyCount--;	// AdditionalPaths sub-key
	} else {
	    // allocate enough for the top-level keys (nothing variable-sized)
	    if (caches->nrps > INT_MAX/sizeof(*caches->rpspaths)) goto finish;
	    caches->rpspaths = calloc(caches->nrps, sizeof(cachedPath));
	    if (!caches->rpspaths)	goto finish;
	}

	str = (CFStringRef)CFDictionaryGetValue(dict, kBCBootConfigKey);
	if (str) {
	    if (CFGetTypeID(str) != CFStringGetTypeID())  goto finish;
	    str2cachedPath(&caches->rpspaths[rpsindex], caches, str);  // M

	    caches->bootconfig = &caches->rpspaths[rpsindex++];
	    keyCount--;	    // handled BootConfig
	}

	mkDict = (CFDictionaryRef)CFDictionaryGetValue(dict, kBCMKextKey);
	if (mkDict) {
	    if (CFGetTypeID(mkDict) != CFDictionaryGetTypeID())  goto finish;

	    // path to mkext itself
	    str = (CFStringRef)CFDictionaryGetValue(mkDict, kBCPathKey);
	    if (CFGetTypeID(str) != CFStringGetTypeID())  goto finish;
	    str2cachedPath(&caches->rpspaths[rpsindex], caches, str);	// M

	    // get the Extensions folder path and set up exts by hand
	    str=(CFStringRef)CFDictionaryGetValue(mkDict, kBCExtensionsDirKey);
	    if (str) {
		char path[PATH_MAX];
		if (CFGetTypeID(str) != CFStringGetTypeID())  goto finish;
		if (!CFStringGetFileSystemRepresentation(str, path, PATH_MAX))
		    goto finish;

		if (strlcat(caches->exts, path, PATH_MAX) >= PATH_MAX)
		    goto finish;
	    }

	    // Archs are fetched from the cacheinfo dictionary when needed
	    caches->mkext = &caches->rpspaths[rpsindex++];
	    keyCount--;	    // mkext key handled
	}

	keyCount--;	// postBootPaths handled
    }


    if (keyCount) {
	*errmsg = "unrecognized bootcaches data; skipping";
    } else {
	// hooray
	*errmsg = NULL;
	rval = 0;
	caches->cacheinfo = CFRetain(bcDict);	// for archs, etc
    }

finish:
    if (createdStr)	CFRelease(createdStr);

    return rval;
}

struct bootCaches* readCaches(DADiskRef dadisk)
{
    struct bootCaches *rval = NULL, *caches = NULL;
    char *errmsg;
    int errnum = 4;	
    struct stat sb;
    CFDictionaryRef ddesc = NULL;
    CFURLRef volURL = NULL;	// owned by dict; don't release
    int ntries = 0;

    char bcpath[PATH_MAX];
    void *bcbuf = NULL;
    CFDataRef bcData = NULL;
    CFDictionaryRef bcDict = NULL;
    char bspath[PATH_MAX];

    errmsg = "allocation failure";	    	
    caches = calloc(1, sizeof(*caches));
    if (!caches)    	    goto finish;
    caches->cachefd = -1;			// set cardinal (fd 0 valid)

    // 'kextcache -U /' needs this retry to work around 5454260
    // kexd's vol_appeared filters volumes w/o mount points
    errmsg = "error copying disk description";
    do {
#if TARGET_OS_EMBEDDED
	    goto finish;
#else
        if (!(ddesc = DADiskCopyDescription(dadisk)))         goto finish;
#endif
        if((volURL=CFDictionaryGetValue(ddesc,kDADiskDescriptionVolumePathKey)))
            break;
        else
            sleep(1);
    } while (++ntries < kKXDiskArbMaxRetries);
    if (ntries == kKXDiskArbMaxRetries) {
        kextd_error_log("Disk description missing mount point for %d tries",
            ntries);
    } else if (ntries) {
        kextd_log("WARNING: readCaches got mount point after %d tries", ntries);
    }

    if (!CFURLGetFileSystemRepresentation(volURL, false, 
	 (UInt8*)caches->root, PATH_MAX))      goto finish;

    errmsg = "error reading " kBootCachesPath;
    if (strlcpy(bcpath, caches->root, PATH_MAX) >= PATH_MAX)  goto finish;
    if (strlcat(bcpath, kBootCachesPath, PATH_MAX) >= PATH_MAX)  goto finish;
    // Sec: cachefd lets us validate data, operations
    if (-1 == (caches->cachefd = open(bcpath, O_RDONLY|O_EVTONLY))) {
	if (errno == ENOENT)	errmsg = NULL;
	goto finish;
    }

    // check the owner and mode (fstat() to insure it's the same file)
    // w/Leopard, root can see all the way to the disk; 99 -> truly unknown
    // note: 'sudo cp mach_kernel /Volumes/disrespected/' should -> error
    if (fstat(caches->cachefd, &sb))
	goto finish;
    if (sb.st_uid!= 0) {
	// XXX is there a way we can return a specific error for kextcache -u?
	errmsg = kBootCachesPath " not owned by root; no rebuilds";
	goto finish;
    }
    if (sb.st_mode & S_IWGRP || sb.st_mode & S_IWOTH) {
	errmsg = kBootCachesPath " writable by non-root";
	goto finish;
    }

    // read the plist
    if (!(bcbuf = malloc(sb.st_size)))  goto finish;
    if (read(caches->cachefd, bcbuf, sb.st_size)!=sb.st_size)  goto finish;
    if (!(bcData = CFDataCreate(nil, bcbuf, sb.st_size)))  goto finish;

    errmsg = kBootCachesPath " doesn't contain a dictionary";
    // Sec: see 4623105 & related for an assessment of our XML parsers
    bcDict = (CFDictionaryRef)CFPropertyListCreateFromXMLData(nil,
		bcData, kCFPropertyListImmutable, NULL);
    if (!bcDict || CFGetTypeID(bcDict)!=CFDictionaryGetTypeID())
	goto finish; 

    // let finishParse() fill in the rest of the structure
    if (finishParse(caches, bcDict, ddesc, &errmsg))	goto finish;

    errmsg = "error creating "kTSCacheDir;
    if (strlcpy(bspath, caches->root, PATH_MAX) >= PATH_MAX)  goto finish;
    if (strlcat(bspath, kTSCacheDir, PATH_MAX) >= PATH_MAX)  goto finish;
    pathcat(bspath, caches->uuid_str);
    if ((errnum = stat(bspath, &sb))) {
	if (errno == ENOENT) {
	    // s..mkdir ensures the cache directory is on the same volume
	    if ((errnum = sdeepmkdir(caches->cachefd, bspath, kTSCacheMask)))
		goto finish;
	}
	else
	    goto finish;
    }

    // success!
    rval = caches;
    errmsg = NULL;

finish:
    // report any error message
    if (errmsg) {
	if (errnum == -1)
	    kextd_error_log("%s: %s: %s",caches->root,errmsg,strerror(errno));
	else
	    kextd_error_log("%s: %s", caches->root, errmsg);
    }

    // clean up (unwind in allocation order)
    if (ddesc)      CFRelease(ddesc);
    if (bcDict)     CFRelease(bcDict);	// retained for struct by finishParse
    if (bcData)     CFRelease(bcData);
    if (bcbuf)      free(bcbuf);

    // if finishParse() failed, clean up our stuff
    if (!rval)
	destroyCaches(caches);	    // closes cachefd if needed

    return rval;
}

/*******************************************************************************
* needsUpdate checks a single path and timestamp; populates path->tstamp
* We compare/copy the *ctime* of the source file to the *mtime* of the bootstamp.
*******************************************************************************/
int needsUpdate(char *root, cachedPath* cpath, Boolean *outofdate)
{
    Boolean ood;
    int bsderr = -1;
    struct stat rsb, tsb;
    char fullrp[PATH_MAX], fulltsp[PATH_MAX];

    // create full paths
    pathcpy(fullrp, root);
    pathcat(fullrp, cpath->rpath);
    pathcpy(fulltsp, root);
    pathcat(fulltsp, cpath->tspath);

    // stat resolved rpath -> tstamp
    if (stat(fullrp, &rsb)) {
	if (errno == ENOENT) {
	    // if the file doesn't exist; it can't be out of date
	    bsderr = 0;
	    *outofdate = false;
	} else {
	    kextd_error_log("cached file %s: %s", fullrp, strerror(errno));
	}
	goto finish;
    }

    cpath->tstamps[0].tv_sec = rsb.st_atimespec.tv_sec;	    // to apply later
    cpath->tstamps[0].tv_usec = rsb.st_atimespec.tv_nsec / 1000;
    cpath->tstamps[1].tv_sec = rsb.st_ctimespec.tv_sec;	    // don't ask ;p
    cpath->tstamps[1].tv_usec = rsb.st_ctimespec.tv_nsec / 1000;

    // stat tspath
    // and compare as appropriate
    if (stat(fulltsp, &tsb) == 0) {
	ood = (tsb.st_mtimespec.tv_sec != rsb.st_ctimespec.tv_sec ||
	       tsb.st_mtimespec.tv_nsec != rsb.st_ctimespec.tv_nsec);
    } else {
	if (errno == ENOENT) {
	    ood = true;	// nothing to compare with
	} else {
	    kextd_error_log("cached file %s: %s", fulltsp, strerror(errno));
	    goto finish;
	}
    }

    *outofdate = ood;
    bsderr = 0;

finish:
    return bsderr;
}

/*******************************************************************************
* needUpdates checks all paths and returns details if you want them
* expects callers only to call it on volumes that will have timestamp paths
* (e.g. BootRoot volumes! ;)
*******************************************************************************/
int needUpdates(struct bootCaches *caches, Boolean *any,
		    Boolean *rps, Boolean *booters, Boolean *misc)
{
    int rval = 0;	// looking for problems (any one will cause failure)
    Boolean needsUp, rpsOOD, bootersOOD, miscOOD, anyOOD;
    cachedPath *cp;

    // assume nothing needs updating (caller may interpret error -> needsUpdate)
    rpsOOD = bootersOOD = miscOOD = anyOOD = false;

    // in theory, all we have to do is find one "problem" (out of date file)
    // but in practice, there could be real problems (like missing sources)
    // we also like populating the tstamps
    for (cp = caches->rpspaths; cp < &caches->rpspaths[caches->nrps]; cp++) {
	if ((rval = needsUpdate(caches->root, cp, &needsUp)))    goto finish;
	if (needsUp) 					anyOOD = rpsOOD = true;
	// one is enough, but needsUpdate populates tstamps which we need later
    }
    if ((cp = &(caches->efibooter)), cp->rpath[0]) {
	if ((rval = needsUpdate(caches->root, cp, &needsUp)))    goto finish;
	if (needsUp)				    anyOOD = bootersOOD = true;
    }
    if ((cp = &(caches->ofbooter)), cp->rpath[0]) {
	if ((rval = needsUpdate(caches->root, cp, &needsUp)))    goto finish;
	if (needsUp)				    anyOOD = bootersOOD = true;
    }
    for (cp = caches->miscpaths; cp < &caches->miscpaths[caches->nmisc]; cp++){
	(void)needsUpdate(caches->root, cp, &needsUp);
	// could emit warnings in an appropriate verbose mode
	// no one cares if .VolumeIcon.icns is missing
	// though evidently (4487046) the label file is important
	if (needsUp)				    anyOOD = miscOOD = true;
    }


    if (rps)  	    *rps = rpsOOD;
    if (booters)    *booters = bootersOOD;
    if (misc)	    *misc = miscOOD;
    if (any)	    *any = anyOOD;

finish:
    return rval;
}

/*******************************************************************************
* applyStamps runs through all of the cached paths in a struct bootCaches
* and applies the timestamps captured before the update
* not going to bother with a re-stat() of the sources for now
*******************************************************************************/
// Sec review: no need to drop privs thanks to safecalls.[ch]
static int applyStamp(char *root, cachedPath *cpath, int fdvol)
{
    int bsderr = -1, fd;
    char tspath[PATH_MAX];

    pathcpy(tspath, root);
    pathcat(tspath, cpath->tspath);

    (void)sunlink(fdvol, tspath);    // since sopen passes O_EXCL
    if (-1 == (fd = sopen(fdvol, tspath, O_WRONLY|O_CREAT, kTSCacheMask)))
	goto finish;	    

    bsderr = futimes(fd, cpath->tstamps);

finish:
    return bsderr;
}

int applyStamps(struct bootCaches *caches)
{
    int rval = 0;
    cachedPath *cp;

    // run through all of the cached paths apply bootstamp
    for (cp = caches->rpspaths; cp < &caches->rpspaths[caches->nrps]; cp++) {
	rval |= applyStamp(caches->root, cp, caches->cachefd);
    }
    if ((cp = &(caches->efibooter)), cp->rpath[0]) {
	rval |= applyStamp(caches->root, cp, caches->cachefd);
    }
    if ((cp = &(caches->ofbooter)), cp->rpath[0]) {
	rval |= applyStamp(caches->root, cp, caches->cachefd);
    }
    for (cp = caches->miscpaths; cp < &caches->miscpaths[caches->nmisc]; cp++){
	rval |= applyStamp(caches->root, cp, caches->cachefd);
    }


    return rval;
}

/*******************************************************************************
* rebuild_mkext fires off kextcache on the given volume
* XX there is a bug here that can mask a stale mkext in the Apple_Boot (4764605)
*******************************************************************************/
int rebuild_mkext(struct bootCaches *caches, Boolean wait)
{   
    int rval = ELAST + 1;
    int pid = -1;
    CFIndex i, argi = 0, argc = 0, narchs = 0;
    CFDictionaryRef pbDict, mkDict;
    CFArrayRef archArray;
    char **kcargs = NULL, **archstrs = NULL;    // no [ARCH_MAX] anywhere?
    char fullmkextp[PATH_MAX], fullextsp[PATH_MAX];

    pbDict = CFDictionaryGetValue(caches->cacheinfo, kBCPostBootKey);
    if (!pbDict || CFGetTypeID(pbDict) != CFDictionaryGetTypeID())  goto finish;
    mkDict = CFDictionaryGetValue(pbDict, kBCMKextKey);
    if (!mkDict || CFGetTypeID(mkDict) != CFDictionaryGetTypeID())  goto finish;
    archArray = CFDictionaryGetValue(mkDict, kBCArchsKey);
    if (archArray) {
        narchs = CFArrayGetCount(archArray);
        archstrs = calloc(narchs, sizeof(char*));
        if (!archstrs)  goto finish;
    }

    //  argv[0] "-a x -a y" '-l'  '-m'  mkext  exts  NULL
    argc =  1  + narchs*2  +  1  +  1  +  1  +  1  +  1;
    kcargs = malloc(argc * sizeof(char*));
    if (!kcargs)  goto finish;
    kcargs[argi++] = "kextcache";

    // convert each -arch argument into a char* and add to the vector
    for(i = 0; i < narchs; i++) {
        CFStringRef archStr;
        size_t archSize;

        // get  arch
        archStr = CFArrayGetValueAtIndex(archArray, i);
        if (!archStr || CFGetTypeID(archStr)!=CFStringGetTypeID()) goto finish;
        // XX an arch is not a pathname; EncodingASCII might be more appropriate
        archSize = CFStringGetMaximumSizeOfFileSystemRepresentation(archStr);
        if (!archSize)  goto finish;
        // X marks the spot: over 800 lines written before I realized that
        // there were some serious security implications
        archstrs[i] = malloc(archSize);
        if (!archstrs[i])  goto finish;
        if (!CFStringGetFileSystemRepresentation(archStr,archstrs[i],archSize))
            goto finish;

        kcargs[argi++] = "-a";
        kcargs[argi++] = archstrs[i];
    }

    kcargs[argi++] = "-l";
    kcargs[argi++] = "-m";

    pathcpy(fullmkextp, caches->root);
    pathcat(fullmkextp, caches->mkext->rpath);
    kcargs[argi++] = fullmkextp;

    pathcpy(fullextsp, caches->root);
    pathcat(fullextsp, caches->exts);
    kcargs[argi++] = fullextsp;

    kcargs[argi] = NULL;

    rval = 0;

   /* wait:false means the return value is <0 for fork/exec failures and
    * the pid of the forked process if >0.
    *
    * wait:true means the return value is <0 for fork/exec failures and
    * the exit status of the forked process (>=0) otherwise.
    */
    pid = fork_program("/usr/sbin/kextcache", kcargs, 0 /* delay */, wait);  // logs its own errors

finish:
    if (rval) 	kextd_error_log("data error before mkext rebuild");
    if (wait || pid < 0)
        rval = pid;

    if (archstrs) {
        for (i = 0; i < narchs; i++) {
            if (archstrs[i])  	free(archstrs[i]);
        }
        free(archstrs);
    }
    if (kcargs)	    free(kcargs);

    return rval;
}

Boolean check_plist_cache(struct bootCaches *caches)
{   
    Boolean needsrebuild = false;
    struct stat sb;
    struct stat extsb;
    char fullplistcachep[PATH_MAX], fullextsp[PATH_MAX];

    // struct bootCaches paths are all *relative*
    pathcpy(fullplistcachep, caches->root);
    pathcat(fullplistcachep, caches->exts);
    pathcat(fullplistcachep, "/Caches/com.apple.kext.info");
    pathcpy(fullextsp, caches->root);
    pathcat(fullextsp, caches->exts);

    if (stat(fullextsp, &extsb) == -1) {
        kextd_log("WARNING: %s: %s", caches->exts, strerror(errno));
        // assert(needsrebuild == false);   // we can't build w/o exts
        goto finish;
    }

    // plist cache
    needsrebuild = true;  // since this stat() will fail if plist cache gone
    if (stat(fullplistcachep, &sb) == -1)
        goto finish;
    needsrebuild = (sb.st_mtime != extsb.st_mtime + 1);

finish:
    return needsrebuild;
}

Boolean check_mkext(struct bootCaches *caches)
{   
    Boolean needsrebuild = false;
    struct stat sb;
    char fullmkextp[PATH_MAX], fullextsp[PATH_MAX];

    // struct bootCaches paths are all *relative*
    pathcpy(fullmkextp, caches->root);
    pathcat(fullmkextp, caches->mkext->rpath);
    pathcpy(fullextsp, caches->root);
    pathcat(fullextsp, caches->exts);

    // mkext implies exts
    if (caches->mkext) {
        struct stat extsb;

        if (stat(fullextsp, &extsb) == -1) {
            kextd_log("WARNING: %s: %s", caches->exts, strerror(errno));
            // assert(needsrebuild == false);   // we can't build w/o exts
            goto finish;
        }

        // Extensions.mkext
        needsrebuild = true;  // since this stat() will fail if mkext gone
        if (stat(fullmkextp, &sb) == -1)
            goto finish;
        needsrebuild = (sb.st_mtime != extsb.st_mtime + 1);
    }

finish:
    return needsrebuild;
}

/*******************************************************************************
* createDiskForMount creates a DADisk object given a mount point
* session is optional; one is created and released if the caller can't supply
*******************************************************************************/
DADiskRef createDiskForMount(DASessionRef session, const char *mount)
{
    DADiskRef rval = NULL;
    DASessionRef dasession = NULL;
    CFURLRef volURL = NULL;

    if (session) {
	dasession = session;
    } else {
#if !TARGET_OS_EMBEDDED
	dasession = DASessionCreate(nil);
#endif
	if (!dasession)     goto finish;
    }

    volURL = CFURLCreateFromFileSystemRepresentation(nil, (UInt8*)mount,
            strlen(mount), 1 /*isDirectory*/);
    if (!volURL)        goto finish;

#if !TARGET_OS_EMBEDDED
    rval = DADiskCreateFromVolumePath(nil, dasession, volURL);
#endif

finish:
    if (volURL)	    CFRelease(volURL);
    if (!session && dasession)
	CFRelease(dasession);

    return rval;
}

/*******************************************************************************
* hasBoots lets you know if a volume has boot partitions and if it's on GPT
*******************************************************************************/
Boolean hasBoots(char *bsdname, CFArrayRef *auxPartsCopy, Boolean *isGPT)
{
    CFDictionaryRef binfo = NULL;
    Boolean rval = false, gpt = false;
    int err;
    CFArrayRef ar;
    char * errmsg = NULL;
    char stack_bsdname[DEVMAXPATHSIZE];
    char * lookup_bsdname = bsdname;
    CFArrayRef dataPartitions = NULL; // do not release;
    char fulldevname[MAXPATHLEN];
    char parentdevname[MAXPATHLEN];
    uint32_t partitionNum;
    BLPartitionType partitionType;

   /* Get the BL info about partitions & such.
    */
    if (BLCreateBooterInformationDictionary(NULL,bsdname,&binfo))   goto finish;

   /*****
    * Now, for a GPT check, use one of the data partitions given by the above
    * call to BLCreateBooterInformationDictionary().
    */
    dataPartitions = CFDictionaryGetValue(binfo, kBLDataPartitionsKey);
    if (dataPartitions && CFArrayGetCount(dataPartitions)) {
        CFStringRef dpBsdName = CFArrayGetValueAtIndex(dataPartitions, 0);

        if (dpBsdName) {
            errmsg = "string conversion failure for bsdname";
             // I hate CFString
            if (!CFStringGetCString(dpBsdName, stack_bsdname,
                sizeof(stack_bsdname), kCFStringEncodingUTF8)) {
                goto finish;
            }
            
            lookup_bsdname = stack_bsdname;
        }
    }
    
   /* Get the BL info about the partition type (that's all we use, but
    * we have to pass in valid buffer pointers for all the rest).
    */
    errmsg = "internal error";
    if (sizeof(fulldevname) <= snprintf(fulldevname, sizeof(fulldevname),
        "/dev/%s", lookup_bsdname)) {

        goto finish;
    }

    errmsg = "can't get partition type";
    if (err = BLGetParentDeviceAndPartitionType(NULL /* context */,
        fulldevname, parentdevname, &partitionNum, &partitionType)) {
        goto finish;
    }
    if (partitionType == kBLPartitionType_GPT) {
        gpt = true;
    }

    errmsg = "can't get helper partitions";
    // check for helper partitions
    ar = CFDictionaryGetValue(binfo, kBLAuxiliaryPartitionsKey);   
    rval = (ar && CFArrayGetCount(ar) > 0);
    if (auxPartsCopy)
        *auxPartsCopy = CFRetain(ar);

    errmsg = NULL;
    
finish:
    if (binfo)      CFRelease(binfo);

    if (isGPT)      *isGPT = gpt;

    if (errmsg) {
        kextd_error_log(errmsg);
    }

    return rval;
}

/*******************************************************************************
*
*******************************************************************************/
Boolean bootedFromDifferentMkext(void)
{
    Boolean result = true;
    MkextCRCResult startupCrcFound;
    MkextCRCResult onDiskCrcFound;
    uint32_t startupCrc;
    uint32_t onDiskCrc;

    startupCrcFound = getMkextCRC(NULL, &startupCrc);
    if (startupCrcFound != kMkextCRCFound) {
        result = false;
        goto finish;
    }

    onDiskCrcFound = getMkextCRC("/System/Library/Extensions.mkext",
        &onDiskCrc);
    if (onDiskCrcFound != kMkextCRCFound) {
        goto finish;
    }

    if (startupCrc == onDiskCrc) {
        result = false;
    }

finish:
    return result;
}

/*******************************************************************************
*
*******************************************************************************/
MkextCRCResult getMkextCRC(const char * file_path, uint32_t * crc_ptr)
{
    MkextCRCResult result = kMkextCRCError;
    fat_iterator iter = NULL;
    const void * file_start = NULL;
    void * file_end = NULL;
    mkext_header * mkext_hdr;
    io_registry_entry_t ioRegRoot = MACH_PORT_NULL;
    CFTypeRef   regObj = NULL;  // must release
    CFDataRef   dataObj = NULL; // must release
    CFIndex     numBytes;
    uint32_t    crc;

    if (!file_path) {
        ioRegRoot = IORegistryGetRootEntry(kIOMasterPortDefault);
        if (ioRegRoot != MACH_PORT_NULL) {
            regObj = IORegistryEntryCreateCFProperty(ioRegRoot,
                CFSTR(kIOStartupMkextCRC), kCFAllocatorDefault, kNilOptions);
            if (!regObj) {
                result = kMkextCRCNotFound;
                goto finish;
            }
            if (CFGetTypeID(regObj) != CFDataGetTypeID()) {
                goto finish;
            }
        }

        dataObj = (CFDataRef)regObj;
        numBytes = CFDataGetLength(dataObj);
        if (numBytes != sizeof(uint32_t)) {
            goto finish;
        }

        CFDataGetBytes(dataObj, CFRangeMake(0, numBytes), (void *)&crc);
    } else {

        iter = fat_iterator_open(file_path, 0);
        if (!iter) {
            goto finish;
        }
        file_start = fat_iterator_file_start(iter);
        if (!file_start) {
            goto finish;
        }

        if (ISMKEXT(MAGIC32(file_start))) {
            mkext_hdr = (struct mkext_header *)file_start;
        } else {
            file_start = fat_iterator_find_host_arch(
                iter, &file_end);
            if (!file_start) {
                goto finish;
            }
            if (!ISMKEXT(MAGIC32(file_start))) {
                goto finish;
            }
            mkext_hdr = (struct mkext_header *)file_start;
        }
        crc = OSSwapBigToHostInt32(mkext_hdr->adler32);
    }

    *crc_ptr = crc;
    result = kMkextCRCFound;

finish:
    if (ioRegRoot)  IOObjectRelease(ioRegRoot);
    if (dataObj)    CFRelease(dataObj);
    if (iter)	    fat_iterator_close(iter);
    return result;
}

void _daDone(DADiskRef disk, DADissenterRef dissenter, void *ctx)
{
    if (dissenter)
        CFRetain(dissenter);
    *(DADissenterRef*)ctx = dissenter;
    CFRunLoopStop(CFRunLoopGetCurrent());   // assumed okay even if not running
}
