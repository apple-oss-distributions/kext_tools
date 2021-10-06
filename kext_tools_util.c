/*
 * Copyright (c) 2008 Apple Computer, Inc. All rights reserved.
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

#include <TargetConditionals.h>
#if !TARGET_OS_EMBEDDED
    #include <bless.h>
    #include <libgen.h>
    #include "bootcaches.h"
#endif  // !TARGET_OS_EMBEDDED

#include <libc.h>
#include <stdint.h>
#include <sysexits.h>
#include <asl.h>
#include <syslog.h>
#include <sys/resource.h>
#include <IOKit/kext/OSKext.h>
#include <IOKit/kext/OSKextPrivate.h>
#include <sandbox/rootless.h>
#include <os/log_private.h>

#include <sys/types.h>
#include <sys/sysctl.h>

#include "kext_tools_util.h"

#if PRAGMA_MARK
#pragma mark Basic Utility
#endif /* PRAGMA_MARK */

/*********************************************************************
*********************************************************************/
char * createUTF8CStringForCFString(CFStringRef aString)
{
    char     * result = NULL;
    CFIndex    bufferLength = 0;

    if (!aString) {
        goto finish;
    }

    bufferLength = sizeof('\0') +
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(aString),
        kCFStringEncodingUTF8);

    result = (char *)malloc(bufferLength * sizeof(char));
    if (!result) {
        goto finish;
    }
    if (!CFStringGetCString(aString, result, bufferLength,
        kCFStringEncodingUTF8)) {

        SAFE_FREE_NULL(result);
        goto finish;
    }

finish:
    return result;
}

/*******************************************************************************
* createCFMutableArray()
*******************************************************************************/
Boolean createCFMutableArray(CFMutableArrayRef * array,
    const CFArrayCallBacks * callbacks)
{
    Boolean result = true;

    *array = CFArrayCreateMutable(kCFAllocatorDefault, 0,
        callbacks);
    if (!*array) {
        result = false;
    }
    return result;
}

/*******************************************************************************
* createCFMutableDictionary()
*******************************************************************************/
Boolean createCFMutableDictionary(CFMutableDictionaryRef * dict)
{
    Boolean result = true;

    *dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!*dict) {
        result = false;
    }
    return result;
}

/*******************************************************************************
* createCFMutableSet()
*******************************************************************************/
Boolean createCFMutableSet(CFMutableSetRef * setOut,
    const CFSetCallBacks * callbacks)
{
    Boolean result = true;

    *setOut = CFSetCreateMutable(kCFAllocatorDefault, 0,
        callbacks);
    if (!*setOut) {
        result = false;
    }
    return result;
}

/*******************************************************************************
*******************************************************************************/
void addToArrayIfAbsent(CFMutableArrayRef array, const void * value)
{
    if (kCFNotFound == CFArrayGetFirstIndexOfValue(array, RANGE_ALL(array),
        value)) {
        
        CFArrayAppendValue(array, value);
    }
    return;
}

/*******************************************************************************
 * createCFDataFromFile()
 *******************************************************************************/
Boolean createCFDataFromFile(CFDataRef  *dataRefOut,
                             const char *filePath)
{
    int             fd = -1;
    Boolean         result = false;
    struct stat     statBuf;
    void            *buffer;
    CFIndex         length;
    
    *dataRefOut = NULL;
    fd = open(filePath, O_RDONLY, 0);
    if (fd < 0) {
        goto finish;
    }
    if (fstat(fd, &statBuf) != 0) {
        goto finish;
    }
    if ((statBuf.st_mode & S_IFMT) != S_IFREG) {
        goto finish;
    }
    if (statBuf.st_size == 0) {
        goto finish;
    }
    
    // fill buffer used for CFData passed to caller
    length = (CFIndex) statBuf.st_size;
    buffer = CFAllocatorAllocate(kCFAllocatorDefault, length, 0);
    if (read(fd, buffer, length) < 0) {
        goto finish;
    }
    
    *dataRefOut = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
                                (const UInt8 *)buffer,
                                length,
                                kCFAllocatorDefault);
    if (*dataRefOut == NULL) {
        CFAllocatorDeallocate(kCFAllocatorDefault, buffer);
        goto finish;
    }
    result = true;
finish:
    if (fd != -1) {
        close(fd);
    }
    if (result == false) {
        OSKextLog(/* kext */ NULL,
                  kOSKextLogErrorLevel,
                  "%s: failed for '%s'", __func__, filePath);
    }
    return result;
}


/*******************************************************************************
 *******************************************************************************/
ExitStatus writeToFile(
                       int           fileDescriptor,
                       const UInt8 * data,
                       CFIndex       length)
{
    ExitStatus result = EX_OSERR;
    ssize_t bytesWritten = 0;
    ssize_t totalBytesWritten = 0;
    
    while (totalBytesWritten < length) {
        bytesWritten = write(fileDescriptor, data + totalBytesWritten,
                             length - totalBytesWritten);
        if (bytesWritten < 0) {
            OSKextLog(/* kext */ NULL,
                      kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
                      "Write failed %d - %s", errno, strerror(errno));
            goto finish;
        }
        totalBytesWritten += bytesWritten;
    }
    
    result = EX_OK;
finish:
    return result;
}

#if !TARGET_OS_EMBEDDED
/******************************************************************************
 ******************************************************************************/

void postNoteAboutKexts( CFStringRef theNotificationCenterName,
                         CFMutableDictionaryRef theDict )
{    
    CFNotificationCenterRef     myCenter    = NULL;
    
    if (theDict == NULL || theNotificationCenterName == NULL)
        return;
    
    myCenter = CFNotificationCenterGetDistributedCenter();
    CFRetain(theDict);

    CFNotificationCenterPostNotificationWithOptions(
        myCenter,
        theNotificationCenterName,
        NULL,
        theDict,
        kCFNotificationDeliverImmediately | kCFNotificationPostToAllSessions );
    
    SAFE_RELEASE(theDict);
    
    return;
}

/******************************************************************************
 * postNoteAboutKextLoadsMT will use CFNotificationCenter to post a notification.
 * The notification center is named by theNotificationCenterName.  This routine
 * is used to notify kextd about a kext that is getting loaded for message 
 * tracing.
 ******************************************************************************/

void postNoteAboutKextLoadsMT(CFStringRef theNotificationCenterName,
                              CFMutableArrayRef theKextPathArray)
{
    CFMutableDictionaryRef      myInfoDict  = NULL; // must release
    CFNotificationCenterRef     myCenter    = NULL;
    
    if (theKextPathArray == NULL || theNotificationCenterName == NULL)
        return;
 
    myCenter = CFNotificationCenterGetDistributedCenter();
    myInfoDict = CFDictionaryCreateMutable(
                                           kCFAllocatorDefault, 0,
                                           &kCFCopyStringDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
    
    if (myInfoDict && myCenter) {
        CFDictionaryAddValue(myInfoDict,
                             CFSTR("KextArrayKey"),
                             theKextPathArray);
        
        CFNotificationCenterPostNotificationWithOptions(
                                                        myCenter,
                                                        theNotificationCenterName,
                                                        NULL,
                                                        myInfoDict,
                                                        kCFNotificationDeliverImmediately |
                                                        kCFNotificationPostToAllSessions );
    }
    
    SAFE_RELEASE(myInfoDict);
   
    return;
}

/*******************************************************************************
 ******************************************************************************/
void addKextToAlertDict( CFMutableDictionaryRef *theDictPtr, OSKextRef theKext )
{
    CFStringRef         myBundleID;                 // do NOT release
    CFStringRef         myBundleVersion;            // do NOT release
    CFMutableArrayRef   myKextArray;                // do NOT release
    CFURLRef            myKextURL = NULL;           // must release
    CFStringRef         myKextPath = NULL;          // must release
    CFMutableDictionaryRef  myKextInfoDict = NULL;  // must release
    CFMutableDictionaryRef  myAlertInfoDict = NULL; // do NOT release
    CFIndex                myCount, i;
  
    
    if ( theDictPtr == NULL || theKext == NULL ) {
        return;
    }
    
    myAlertInfoDict = *theDictPtr;
    if (myAlertInfoDict == NULL) {
        /* caller wants us to create Alert Info Dictionary */
        myAlertInfoDict = CFDictionaryCreateMutable(
                                        kCFAllocatorDefault, 0,
                                        &kCFCopyStringDictionaryKeyCallBacks,
                                        &kCFTypeDictionaryValueCallBacks );
        if (myAlertInfoDict == NULL) {
            return;
        }
        *theDictPtr = myAlertInfoDict;
    }
        
    myBundleID = OSKextGetIdentifier(theKext);
    if ( myBundleID == NULL ) {
        goto finish;
    }
    
    /* We never alert about Apple Kexts */
    if ( CFStringHasPrefix(myBundleID, __kOSKextApplePrefix) == true) {
        goto finish;
    }
    
    myBundleVersion = OSKextGetValueForInfoDictionaryKey(theKext,
                                                         kCFBundleVersionKey);
    if (myBundleVersion == NULL) {
        goto finish;
    }
    
    myKextURL = CFURLCopyAbsoluteURL(OSKextGetURL(theKext));
    if (myKextURL == NULL) {
        goto finish;
    }
    
    myKextPath = CFURLCopyFileSystemPath(myKextURL, kCFURLPOSIXPathStyle);
    if (myKextPath == NULL) {
        goto finish;
    }
    
    /* add kext info to the Alert Dictionary.
     * We want BundleID, Version and full path to the kext
     */
    myKextArray = (CFMutableArrayRef)
        CFDictionaryGetValue(myAlertInfoDict, CFSTR("KextInfoArrayKey"));
    if (myKextArray == NULL) {
        /* first kext info so create the kext info array */
        myKextArray = CFArrayCreateMutable(kCFAllocatorDefault,
                                           0,
                                           &kCFTypeArrayCallBacks);
        if (myKextArray == NULL) {
            goto finish;
        }
        CFDictionarySetValue(myAlertInfoDict,
                             CFSTR("KextInfoArrayKey"),
                             myKextArray);
    }
    
    /* check for dup of this kext */
    myCount = CFArrayGetCount(myKextArray);
    if (myCount > 0) {
        for (i = 0; i < myCount; i++) {
            CFMutableDictionaryRef myDict;
            myDict = (CFMutableDictionaryRef)
            CFArrayGetValueAtIndex(myKextArray, i);
            if (myDict == NULL)   continue;
            
            if ( !CFDictionaryContainsValue(myDict, myBundleID) ) {
                continue;
            }
            if ( !CFDictionaryContainsValue(myDict,
                                            myBundleVersion) ) {
                continue;
            }
            /* already have this one so bail */
            goto finish;
        }
    }
    
    /* new kext info to add */
    myKextInfoDict = CFDictionaryCreateMutable(
                                        kCFAllocatorDefault, 0,
                                        &kCFCopyStringDictionaryKeyCallBacks,
                                        &kCFTypeDictionaryValueCallBacks);
    if (myKextInfoDict == NULL) {
        goto finish;
    }
    CFDictionaryAddValue(myKextInfoDict,
                         kCFBundleIdentifierKey,
                         myBundleID);
    CFDictionaryAddValue(myKextInfoDict,
                         kCFBundleVersionKey,
                         myBundleVersion);
    CFDictionaryAddValue(myKextInfoDict,
                         CFSTR("KextPathKey"),
                         myKextPath);
    
    CFArrayAppendValue(myKextArray,
                       myKextInfoDict);
        
finish:
    SAFE_RELEASE(myKextURL);
    SAFE_RELEASE(myKextPath);
    SAFE_RELEASE(myKextInfoDict);
    
    return;
}


/*******************************************************************************
 * isDebugSetInBootargs() - check to see if boot-args has debug set.  We cache
 * the result.
 *******************************************************************************/
Boolean isDebugSetInBootargs(void)
{
    static int          didOnce         = 0;
    static Boolean      result          = false;
    io_registry_entry_t optionsNode     = MACH_PORT_NULL;   // must release
    CFStringRef         bootargsEntry   = NULL;             // must release
    
    if (didOnce) {
        return(result);
    }
    optionsNode = IORegistryEntryFromPath(kIOMasterPortDefault,
                                          "IODeviceTree:/options");
    if (optionsNode) {
        bootargsEntry = (CFStringRef)
        IORegistryEntryCreateCFProperty(optionsNode,
                                        CFSTR("boot-args"),
                                        kCFAllocatorDefault, 0);
        if (bootargsEntry &&
            (CFGetTypeID(bootargsEntry) == CFStringGetTypeID())) {
            CFRange     findRange;
            findRange = CFStringFind(bootargsEntry, CFSTR("debug"), 0);
            
            if (findRange.length != 0) {
                result = true;
            }
        }
    }
    didOnce++;
    if (optionsNode)  IOObjectRelease(optionsNode);
    SAFE_RELEASE(bootargsEntry);
    
    return(result);
}

#endif  // !TARGET_OS_EMBEDDED

/*******************************************************************************
 * createRawBytesFromHexString() - Given an ASCII hex string + length, dump the
 * computer-readable equivalent into a byte pointer. **Only accepts hex strings
 * of even length, because I'm lazy.**
 *******************************************************************************/
Boolean createRawBytesFromHexString(char *bytePtr, size_t byteLen, const char *hexPtr, size_t hexLen)
{
    size_t minByteLen = (hexLen + 1)/2;

    if (!bytePtr || !hexPtr) {
        return false;
    } else if (hexLen % 2 != 0) {
        return false;
    } else if (minByteLen > byteLen) {
        return false;
    }

    /* reset the output to 0 */
    memset(bytePtr, 0, minByteLen);

    for (size_t index = 0; index < hexLen; index++) {
        uint8_t nibble;
        uint8_t shift = (((index + 1) % 2) ? 4 : 0);
        char    hex   = hexPtr[index];

        if ('0' <= hex && hex <= '9') {
            nibble = hex - '0';
        } else if ('A' <= hex && hex <= 'F') {
            nibble = 10 + (hex - 'A');
        } else if ('a' <= hex && hex <= 'f') {
            nibble = 10 + (hex - 'a');
        } else {
            return false;
        }
        bytePtr[index/2] += (nibble << shift);
    }
    return true;
}

/*******************************************************************************
 * createHexStringFromRawBytes() - Given a byte pointer + length, dump the
 * human-readable equivalent into a hex string pointer.
 *******************************************************************************/
Boolean createHexStringFromRawBytes(char *hexPtr, size_t hexLen, const char *bytePtr, size_t byteLen)
{
    size_t minHexLen = (byteLen * 2) + 1;
    const char *hexes = "0123456789abcdef";

    if (!hexPtr || !bytePtr) {
        return false;
    } else if (minHexLen > hexLen) {
        return false;
    }

    for (size_t bidx = 0, hidx = 0; bidx < byteLen; bidx++) {
        const uint8_t byte = (const uint8_t)bytePtr[bidx];
        hexPtr[hidx++] = hexes[byte >> 4];
        hexPtr[hidx++] = hexes[byte & 0x0f];
    }
    /* NULL-terminate */
    hexPtr[minHexLen-1] = 0;
    return true;
}

#if PRAGMA_MARK
#pragma mark Path & File
#endif /* PRAGMA_MARK */
/*******************************************************************************
*******************************************************************************/
ExitStatus checkPath(
    const char * path,
    const char * suffix,  // w/o the dot
    Boolean      directoryRequired,
    Boolean      writableRequired)
{
    Boolean result  = EX_USAGE;
    Boolean nameBad = FALSE;
    struct  stat statBuffer;
    
    if (!path) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
            "Internal error - %s - NULL path.",
                __FUNCTION__);
        result = EX_SOFTWARE;
        goto finish;
    }
    
    result = EX_USAGE;
    if (suffix) {
        size_t pathLength   = strlen(path);
        size_t suffixLength = strlen(suffix);
        size_t suffixIndex = 0;
        size_t periodIndex = 0;

        nameBad = TRUE;
        if (!pathLength || !suffixLength) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                "Internal error - %s - empty string.",
                __FUNCTION__);
            result = EX_SOFTWARE;
            goto finish;
        }

       /* Peel off any trailing '/' characters (silly shell completion),
        * then advance the length back to point to the character past
        * the real end (which will be a slash or '\0').
        */
        while (pathLength-- && path[pathLength] == '/') {
            /* just scanning for last non-slash */
            if (!pathLength) {
                goto finish;
            }
        }
        pathLength++;

        if (suffixLength >= pathLength) {
            goto finish;
        }
        suffixIndex = pathLength - suffixLength;
        periodIndex = suffixIndex - 1;
        if (path[periodIndex] != '.' ||
            strncmp(path + suffixIndex, suffix, suffixLength)) {
            goto finish;
        }
        nameBad = FALSE;
    }

    result = EX_NOINPUT;
    if (0 != stat(path, &statBuffer)) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
            "Can't stat %s - %s.", path,
            strerror(errno));
        goto finish;
    }
    
    if (directoryRequired && ((statBuffer.st_mode & S_IFMT) != S_IFDIR) ) {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
            "%s is not a directory.",
            path);
        goto finish;
    }

    result = EX_NOPERM;
    if (writableRequired && access(path, W_OK) == -1) {
        OSKextLog(/* kext */ NULL, kOSKextLogErrorLevel,
            "%s is not writable.", path);
        goto finish;
    }

    result = EX_OK;
    
finish:
    if (nameBad) {
        OSKextLog(/* kext */ NULL, kOSKextLogErrorLevel,
            "%s not of type '%s'.", path, suffix);
    }
    return result;
}

/*******************************************************************************
*******************************************************************************/
void
saveFile(const void * vKey, const void * vValue, void * vContext)
{
    CFStringRef       key      = (CFStringRef)vKey;
    CFDataRef         fileData = (CFDataRef)vValue;
    SaveFileContext * context  = (SaveFileContext *)vContext;

    long              length;
    int               fd = -1;
    mode_t            mode = 0666;
    struct  stat      statBuf;
    CFURLRef          saveURL = NULL;     // must release
    Boolean           fileExists = false;
    char              savePath[PATH_MAX];
    
    if (context->fatal) {
        goto finish;
    }
    
    saveURL = CFURLCreateCopyAppendingPathComponent(kCFAllocatorDefault,
        context->saveDirURL, key, /* isDirectory */ false);
    if (!saveURL) {
        context->fatal = true;
        goto finish;
    }

    if (!CFURLGetFileSystemRepresentation(saveURL, /* resolveToBase */ true,
        (u_char *)savePath, sizeof(savePath))) {
        context->fatal = true;
        goto finish;
    }
    
    if (!context->overwrite) {
        fileExists = CFURLResourceIsReachable(saveURL, NULL);
        if (fileExists) {
           switch (user_approve(/* ask_all */ TRUE, /* default_answer */ REPLY_YES,
                "%s exists, overwrite", savePath)) {

                case REPLY_YES:
                    // go ahead and overwrite.
                    break;
                case REPLY_ALL:
                    // go ahead and overwrite this and all following.
                    fprintf(stderr,
                        "Overwriting all symbol files for kexts in dependency graph.\n");
                    context->overwrite = TRUE;
                    break;
                case REPLY_NO:
                    goto finish;
                    break;
                default:
                    context->fatal = true;
                    goto finish;
                    break;
            }
        }
        else {
            OSKextLog(/* kext */ NULL, kOSKextLogErrorLevel,
                      "%s missing '%s'", __func__, savePath);
            context->fatal = true;
            goto finish;
        }
    }

    /* Write data.
     */
    length = CFDataGetLength(fileData);
    if (0 == stat(savePath, &statBuf)) {
        mode = statBuf.st_mode;
    }
    fd = open(savePath, O_WRONLY|O_CREAT|O_TRUNC, mode);
    if (fd != -1 && length) {
        ExitStatus result;
        result = writeToFile(fd, CFDataGetBytePtr(fileData), length);
        if (result != EX_OK) {
            OSKextLog(/* kext */ NULL, kOSKextLogErrorLevel,
                      "%s write failed for '%s'", __func__, savePath);
        }
    }
    else {
        /* Is this fatal to the whole program? I'd rather soldier on.
         */
        OSKextLog(/* kext */ NULL,
                  kOSKextLogErrorLevel | kOSKextLogFileAccessFlag,
                  "%s Failed to save '%s'", __func__, savePath);
    }

finish:
    if (fd != -1) {
        fsync(fd);
        close(fd);
    }
    SAFE_RELEASE(saveURL);
    return;
}

/*******************************************************************************
*******************************************************************************/
CFStringRef copyKextPath(OSKextRef aKext)
{
    CFStringRef result = NULL;
    CFURLRef    absURL = NULL;  // must release
    
    if (!OSKextGetURL(aKext)) {
        goto finish;
    }
    
    absURL = CFURLCopyAbsoluteURL(OSKextGetURL(aKext));
    if (!absURL) {
        goto finish;
    }
    result = CFURLCopyFileSystemPath(absURL, kCFURLPOSIXPathStyle);
finish:
    SAFE_RELEASE(absURL);
    return result;
}


/*******************************************************************************
 * Returns the access and mod times from the file in the given array of
 * fileURLs with the latest mod time.
 * 11860417 - support multiple extensions directories
 *******************************************************************************/
ExitStatus
getLatestTimesFromCFURLArray(
                             CFArrayRef       dirURLArray,
                             struct timeval   dirTimeVals[2])
{
    ExitStatus      result     = EX_SOFTWARE;
    int             i;
    CFURLRef        myURL;
    struct stat     myStatBuf;
    struct timeval  myTempModTime;
    struct timeval  myTempAccessTime;
    
    if (dirURLArray == NULL) {
        goto finish;
    }
    bzero(dirTimeVals, (sizeof(struct timeval) * 2));
    
    for (i = 0; i < CFArrayGetCount(dirURLArray); i++) {
        myURL = (CFURLRef) CFArrayGetValueAtIndex(dirURLArray, i);
        if (myURL == NULL) {
            OSKextLog(NULL,
                      kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                      "%s: NO fileURL at index %d!!!! ", __FUNCTION__, i);
            goto finish;
        }

        result = statURL(myURL, &myStatBuf);
        if (result != EX_OK) {
            goto finish;
        }
        TIMESPEC_TO_TIMEVAL(&myTempAccessTime, &myStatBuf.st_atimespec);
        TIMESPEC_TO_TIMEVAL(&myTempModTime, &myStatBuf.st_mtimespec);
       
        if (timercmp(&myTempModTime, &dirTimeVals[1], >)) {
            dirTimeVals[0].tv_sec = myTempAccessTime.tv_sec;
            dirTimeVals[0].tv_usec = myTempAccessTime.tv_usec;
            dirTimeVals[1].tv_sec = myTempModTime.tv_sec;
            dirTimeVals[1].tv_usec = myTempModTime.tv_usec;
        }
    }
    
    result = EX_OK;
finish:
    return result;
}

/*******************************************************************************
 * Returns the access and mod times from the file in the given directory with 
 * the latest mod time.
 *******************************************************************************/
ExitStatus
getLatestTimesFromDirURL(
                         CFURLRef       dirURL,
                         struct timeval dirTimeVals[2])
{
    ExitStatus          result              = EX_SOFTWARE;
    CFURLEnumeratorRef  myEnumerator        = NULL; // must release
    struct stat         myStatBuf;
    struct timeval      myTempModTime;
    struct timeval      myTempAccessTime;
    
    bzero(dirTimeVals, (sizeof(struct timeval) * 2));
    
    if (dirURL == NULL) {
        goto finish;
    }
   
    myEnumerator = CFURLEnumeratorCreateForDirectoryURL(
                                            NULL,
                                            dirURL,
                                            kCFURLEnumeratorDefaultBehavior,
                                            NULL );
    if (myEnumerator == NULL) {
        OSKextLogMemError();
        goto finish;
    }
    CFURLRef myURL = NULL;
    while (CFURLEnumeratorGetNextURL(
                                     myEnumerator,
                                     &myURL,
                                     NULL) == kCFURLEnumeratorSuccess) {
        if (statURL(myURL, &myStatBuf) != EX_OK) {
            goto finish;
        }
        TIMESPEC_TO_TIMEVAL(&myTempAccessTime, &myStatBuf.st_atimespec);
        TIMESPEC_TO_TIMEVAL(&myTempModTime, &myStatBuf.st_mtimespec);
       
        if (timercmp(&myTempModTime, &dirTimeVals[1], >)) {
            dirTimeVals[0].tv_sec = myTempAccessTime.tv_sec;
            dirTimeVals[0].tv_usec = myTempAccessTime.tv_usec;
            dirTimeVals[1].tv_sec = myTempModTime.tv_sec;
            dirTimeVals[1].tv_usec = myTempModTime.tv_usec;
        }
    } // while loop...
   
    result = EX_OK;
finish:
    if (myEnumerator)   CFRelease(myEnumerator);
    return result;
}

/*******************************************************************************
 * Returns the access and mod times from the file in the given directory with
 * the latest mod time.
 *******************************************************************************/
ExitStatus
getLatestTimesFromDirPath(
                          const char *   dirPath,
                          struct timeval dirTimeVals[2])
{
    ExitStatus          result              = EX_SOFTWARE;
    CFURLRef            kernURL             = NULL; // must release
    
    if (dirPath == NULL) {
        goto finish;
    }
 
    kernURL = CFURLCreateFromFileSystemRepresentation(
                                                      NULL,
                                                      (const UInt8 *)dirPath,
                                                      strlen(dirPath),
                                                      true );
    if (kernURL == NULL) {
        OSKextLogMemError();
        goto finish;
    }
   
    result = getLatestTimesFromDirURL(kernURL, dirTimeVals);
finish:
    if (kernURL)        CFRelease(kernURL);
    return result;
}

/*******************************************************************************
 *******************************************************************************/
ExitStatus
getParentPathTimes(
                   const char        * thePath,
                   struct timeval      cacheFileTimes[2] )
{
    ExitStatus          result          = EX_SOFTWARE;
    char *              lastSlash       = NULL;
    char                myTempPath[PATH_MAX];
   
    if (thePath == NULL) {
        goto finish;
    }
    
    lastSlash = strrchr(thePath, '/');
    // bail if no '/' or if length is < 2 (shortest possible dir path "/a/")
    if (lastSlash == NULL || (lastSlash - thePath) < 2) {
        goto finish;
    }
    // drop off everything at last '/' and beyond
    if (strlcpy(myTempPath,
                thePath,
                (lastSlash - thePath) + 1) >= PATH_MAX) {
        goto finish;
    }
   
    result = getFilePathTimes(myTempPath, cacheFileTimes);
finish:
    return result;
}

/*******************************************************************************
 *******************************************************************************/
ExitStatus
getFileDescriptorTimes(
                 int                the_fd,
                 struct timeval     cacheFileTimes[2])
{
    struct stat         statBuffer;
    ExitStatus          result          = EX_SOFTWARE;
    
    result = fstat(the_fd, &statBuffer);
    if (result != EX_OK) {
        goto finish;
    }
    
    TIMESPEC_TO_TIMEVAL(&cacheFileTimes[0], &statBuffer.st_atimespec);
    TIMESPEC_TO_TIMEVAL(&cacheFileTimes[1], &statBuffer.st_mtimespec);
    
    result = EX_OK;
finish:
    return result;
}

/*******************************************************************************
 *******************************************************************************/
ExitStatus
getFilePathTimes(
                 const char        * filePath,
                 struct timeval      cacheFileTimes[2])
{
    struct stat         statBuffer;
    ExitStatus          result          = EX_SOFTWARE;
    
    result = statPath(filePath, &statBuffer);
    if (result != EX_OK) {
        goto finish;
    }
    
    TIMESPEC_TO_TIMEVAL(&cacheFileTimes[0], &statBuffer.st_atimespec);
    TIMESPEC_TO_TIMEVAL(&cacheFileTimes[1], &statBuffer.st_mtimespec);
    
    result = EX_OK;
finish:
    return result;
}


/*******************************************************************************
 *******************************************************************************/
ExitStatus
statURL(CFURLRef anURL, struct stat * statBuffer)
{
    ExitStatus result = EX_OSERR;
    char path[PATH_MAX];
    
    if (!CFURLGetFileSystemRepresentation(anURL, /* resolveToBase */ true,
                                          (UInt8 *)path, sizeof(path)))
    {
        OSKextLogStringError(/* kext */ NULL);
        goto finish;
    }
    
    result = statPath(path, statBuffer);
    if (!result) {
        goto finish;
    }
    
    result = EX_OK;
finish:
    return result;
}

/*******************************************************************************
 *******************************************************************************/
ExitStatus
statPath(const char *path, struct stat *statBuffer)
{
    ExitStatus result = EX_OSERR;
    
    if (stat(path, statBuffer)) {
        OSKextLog(/* kext */ NULL,
                  kOSKextLogDebugLevel | kOSKextLogGeneralFlag,
                  "Can't stat %s - %s.", path, strerror(errno));
        goto finish;
    }
    
    result = EX_OK;
    
finish:
    return result;
}

/*******************************************************************************
 *******************************************************************************/
ExitStatus
statParentPath(const char *thePath, struct stat *statBuffer)
{
    ExitStatus          result          = EX_SOFTWARE;
    char *              lastSlash       = NULL;
    char                myTempPath[PATH_MAX];
    
    if (thePath == NULL) {
        goto finish;
    }
    
    lastSlash = strrchr(thePath, '/');
    // bail if no '/' or if length is < 2 (shortest possible dir path "/a/")
    if (lastSlash == NULL || (lastSlash - thePath) < 2) {
        goto finish;
    }
    // drop off everything at last '/' and beyond
    if (strlcpy(myTempPath,
                thePath,
                (lastSlash - thePath) + 1) >= PATH_MAX) {
        goto finish;
    }
    
    result = statPath(myTempPath, statBuffer);
finish:
    return result;
}

/*******************************************************************************
 * caller must free returned pointer
 *******************************************************************************/
char *
getPathExtension(const char * pathPtr)
{
    char *              suffixPtr       = NULL; // caller must free
    CFURLRef            pathURL         = NULL; // must release
    CFStringRef         tmpCFString     = NULL; // must release
    
    pathURL = CFURLCreateFromFileSystemRepresentation(
                                                      NULL,
                                                      (const UInt8 *)pathPtr,
                                                      strlen(pathPtr),
                                                      true );
    if (pathURL == NULL) {
        goto finish;
    }
    tmpCFString =  CFURLCopyPathExtension(pathURL);
    if (tmpCFString == NULL) {
        goto finish;
    }
    suffixPtr = createUTF8CStringForCFString(tmpCFString);
    
finish:
    SAFE_RELEASE(pathURL);
    SAFE_RELEASE(tmpCFString);
    
    return suffixPtr;
}

/*******************************************************************************
 *******************************************************************************/
int getFileDevAndInoWith_fd(int the_fd, dev_t * the_dev_t, ino_t * the_ino_t)
{
    int             my_result = -1;
    struct stat     my_stat_buf;
   
    if (fstat(the_fd, &my_stat_buf) == 0) {
        if (the_dev_t) {
            *the_dev_t = my_stat_buf.st_dev;
        }
        if (the_ino_t) {
            *the_ino_t = my_stat_buf.st_ino;
        }
        my_result = 0;
    }
    else {
        my_result = errno;
    }
    
    return(my_result);
}

/*******************************************************************************
 *******************************************************************************/
int getFileDevAndIno(const char * thePath, dev_t * the_dev_t, ino_t * the_ino_t)
{
    int             my_result = -1;
    struct stat     my_stat_buf;
    
    if (stat(thePath, &my_stat_buf) == 0) {
        if (the_dev_t) {
            *the_dev_t = my_stat_buf.st_dev;
        }
        if (the_ino_t) {
            *the_ino_t = my_stat_buf.st_ino;
        }
        my_result = 0;
    }
    else {
        my_result = errno;
    }
    
    return(my_result);
}

/*******************************************************************************
 * If the_dev_t and the_ino_t are 0 then we expect thePath to NOT exist.
 *******************************************************************************/
Boolean isSameFileDevAndIno(int the_fd,
                            const char * thePath,
                            bool followSymlinks,
                            dev_t the_dev_t,
                            ino_t the_ino_t)
{
    Boolean         my_result = FALSE;
    struct stat     my_stat_buf;

    if (the_fd == -1) {
        int ret;
        if (followSymlinks) {
            ret = stat(thePath, &my_stat_buf);
        } else {
            ret = lstat(thePath, &my_stat_buf);
        }
        /* means we are passed a full path in thePath */
        if (ret == 0) {
            if (the_dev_t == my_stat_buf.st_dev &&
                the_ino_t == my_stat_buf.st_ino) {
                my_result = TRUE;
            }
        } else if (errno == ENOENT && the_dev_t == 0 && the_ino_t == 0) {
            /* special case where thePath did not exist so it still should not
             * exist
             */
            my_result = TRUE;
        }
    } else {
        /* means we are passed a relative path from the_fd */
        int flags = followSymlinks ? 0 : AT_SYMLINK_NOFOLLOW;
        if (fstatat(the_fd, thePath, &my_stat_buf, flags) == 0) {
            if (the_dev_t == my_stat_buf.st_dev &&
                the_ino_t == my_stat_buf.st_ino) {
                my_result = TRUE;
            }
        } else if (errno == ENOENT && the_dev_t == 0 && the_ino_t == 0) {
            /* special case where thePath did not exist so it still should not
             * exist
             */
            my_result = TRUE;
        }
    }
    
    return(my_result);
}


/*******************************************************************************
 *******************************************************************************/
Boolean isSameFileDevAndInoWith_fd(int      the_fd,
                                   dev_t    the_dev_t,
                                   ino_t    the_ino_t)
{
    Boolean         my_result = FALSE;
    struct stat     my_stat_buf;
    
    if (fstat(the_fd, &my_stat_buf) == 0) {
        if (the_dev_t == my_stat_buf.st_dev &&
            the_ino_t == my_stat_buf.st_ino) {
            my_result = TRUE;
        }
    }
    
    return(my_result);
}

#if PRAGMA_MARK
#pragma mark Logging
#endif /* PRAGMA_MARK */
/*******************************************************************************
*******************************************************************************/
OSKextLogSpec _sLogSpecsForVerboseLevels[] = {
    kOSKextLogErrorLevel    | kOSKextLogVerboseFlagsMask,   // [0xff1] -v 0
    kOSKextLogBasicLevel    | kOSKextLogVerboseFlagsMask,   // [0xff3] -v 1
    kOSKextLogProgressLevel | kOSKextLogVerboseFlagsMask,   // [0xff4] -v 2
    kOSKextLogStepLevel     | kOSKextLogVerboseFlagsMask,   // [0xff5] -v 3
    kOSKextLogDetailLevel   | kOSKextLogVerboseFlagsMask,   // [0xff6] -v 4
    kOSKextLogDebugLevel    | kOSKextLogVerboseFlagsMask,   // [0xff7] -v 5
    kOSKextLogDebugLevel    | kOSKextLogVerboseFlagsMask |  // [0xfff] -v 6
        kOSKextLogKextOrGlobalMask
};

/*******************************************************************************
* getopt_long_only() doesn't actually handle optional args very well. So, we
* jump through some hoops here to handle all six possibilities:
*
*   cmd line      optarg  argv[optind]
*   ----------------------------------------------
*   -v            (null)  (following arg or null)
*   -v arg        (null)  arg
*   -v=arg        (null)  -v=arg -- ILLEGAL
*   -verbose      (null)  (following arg or null)
*   -verbose arg  (null)  arg
*   -verbose=arg  arg     (following arg or null)
*
* Note that only in the -verbose=arg case does optarg actually get set
* correctly!
*
* If we have not optarg but a following argv[optind], we check it to see if
* it looks like a legal arg to -v/-verbose; if it matches we increment optind.
* -v has never allowed the argument to immediately follow (as in -v2), so
* we still don't handle that.
*******************************************************************************/
#define kBadVerboseOptPrefix  "-v="

ExitStatus setLogFilterForOpt(
    int            argc,
    char * const * argv,
    OSKextLogSpec  forceOnFlags)
{
    ExitStatus      result       = EX_USAGE;
    OSKextLogSpec   logFilter    = 0;
    const char    * localOptarg  = NULL;

   /* Must be a bare -v; just use the extra flags.
    */
    if (!optarg && optind >= argc) {
        logFilter = _sLogSpecsForVerboseLevels[1];
        
    } else {

        if (optarg) {
            localOptarg = optarg;
        } else {
            localOptarg = argv[optind];
        }

        if (!strncmp(localOptarg, kBadVerboseOptPrefix,
            sizeof(kBadVerboseOptPrefix) - 1)) {

            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                "%s - syntax error (don't use = with single-letter option args).",
                localOptarg);
            goto finish;
        }
        
       /* Look for '-v0x####' with no space and advance to the 0x part.
        */
        if (localOptarg[0] == '-' && localOptarg[1] == kOptVerbose &&
            localOptarg[2] == '0' && (localOptarg[3] == 'x' || localOptarg[3] == 'X')) {

            localOptarg += 2;
        }

       /* Look for a 0x#### style verbose arg.
        */
        if (localOptarg[0] == '0' && (localOptarg[1] == 'x' || localOptarg[1] == 'X')) {
            char          * endptr      = NULL;
            OSKextLogSpec   parsedFlags = (unsigned)strtoul(localOptarg, &endptr, 16);

            if (endptr[0]) {
                OSKextLog(/* kext */ NULL,
                    kOSKextLogErrorLevel | kOSKextLogGeneralFlag,
                    "Can't parse verbose argument %s.", localOptarg);
                goto finish;
            }
            logFilter = parsedFlags;
            
            if (!optarg) {
                optind++;
            }

       /* Now a 0-6 style verbose arg.
        */
        } else if (((localOptarg[0] >= '0') || (localOptarg[0] <= '6')) &&
            (localOptarg[1] == '\0')) {

            logFilter = _sLogSpecsForVerboseLevels[localOptarg[0] - '0'];
            if (!optarg) {
                optind++;
            }

       /* Must be a -v with command args following; just use the extra flag.
        */
        } else {
            logFilter = _sLogSpecsForVerboseLevels[1];
        }
    }

    logFilter = logFilter | forceOnFlags;
    
    OSKextSetLogFilter(logFilter, /* kernel? */ false);
    OSKextSetLogFilter(logFilter, /* kernel? */ true);

    result = EX_OK;

finish:
    return result;
}

/*******************************************************************************
*******************************************************************************/
void beQuiet(void)
{
    fclose(stdout);
    fclose(stderr);
    close(1);
    close(2);
    OSKextSetLogFilter(kOSKextLogSilentFilter, /* kernel? */ false);
    OSKextSetLogFilter(kOSKextLogSilentFilter, /* kernel? */ true);
    return;
}

bool
get_kextlog(uint32_t *mode)
{
    char bootargs[1024];
    size_t size = sizeof(bootargs);
    char *kextlog;

    if (sysctlbyname("kern.bootargs", bootargs, &size, NULL, 0) == 0 && (kextlog = strcasestr(bootargs, "kextlog=")) != NULL) {
        char *token = NULL;
        uint32_t value = (uint32_t)strtoul(&kextlog[strlen("kextlog=")], &token, 16);
        if (token == NULL || (*token) == '\0' || isspace(*token)) {
            *mode = value;
            return true;
        }
    }

    return false;
}


/*******************************************************************************
*******************************************************************************/
FILE * g_log_stream = NULL;
static boolean_t sNewLoggingOnly  = false;
static os_log_t  sKextLog         = NULL;
static os_log_t  sKextSignpostLog = NULL;
// xxx - need to aslclose()

void tool_initlog()
{
    uint32_t kextlog_mode = 0;
    if (get_kextlog(&kextlog_mode)) {
        os_log(OS_LOG_DEFAULT, "Setting kext log mode: 0x%x", kextlog_mode);
        OSKextSetLogFilter(kextlog_mode, /* kernel? */ false);
        OSKextSetLogFilter(kextlog_mode, /* kernel? */ true);
    }

    if (sKextLog == NULL) {
        sKextLog = os_log_create("com.apple.kext", "kextlog");
        sKextSignpostLog = os_log_create("com.apple.kext", "signposts");
    }
}

void tool_openlog(const char * __unused name)
{
    sNewLoggingOnly = true;
    tool_initlog();
}

os_log_t
get_signpost_log(void)
{
    return sKextSignpostLog;
}

#if !TARGET_OS_EMBEDDED
/*******************************************************************************
 * Check to see if this is an Apple internal build.  If apple internel then 
 * use development kernel if it exists.
 * /System/Library/Kernels/kernel.development
 *******************************************************************************/
Boolean useDevelopmentKernel(const char * theKernelPath)
{
    struct stat     statBuf;
    char *          tempPath = NULL;
    size_t          length = 0;
    Boolean         myResult = FALSE;
    
    if (statPath(kAppleInternalPath, &statBuf) != EX_OK) {
        return(myResult);
    }
    tempPath = malloc(PATH_MAX);
    
    while (tempPath) {
        length = strlcpy(tempPath, theKernelPath, PATH_MAX);
        if (length >= PATH_MAX)   break;
        length = strlcat(tempPath,
                         kDefaultDevKernelSuffix,
                         PATH_MAX);
        if (length >= PATH_MAX)   break;
        if (statPath(tempPath, &statBuf) == EX_OK) {
            // use kernel.development
            myResult = TRUE;
        }
        break;
    } // while...
    
    if (tempPath)   free(tempPath);
    
    return(myResult);
}
#endif  // !TARGET_OS_EMBEDDED

/*******************************************************************************
* Basic log function. If any log flags are set, log the message
* to syslog/stderr.  Note: exported as SPI in bootroot.h.
*******************************************************************************/

void tool_log(
    OSKextRef       aKext __unused,
    OSKextLogSpec   msgLogSpec,
    const char    * format, ...)
{
    OSKextLogSpec kextLogLevel = msgLogSpec & kOSKextLogLevelMask;
    os_log_type_t logType = OS_LOG_TYPE_DEFAULT;
    va_list ap;

    if (kextLogLevel == kOSKextLogErrorLevel) {
        logType = OS_LOG_TYPE_ERROR;
    } else if (kextLogLevel == kOSKextLogWarningLevel) {
        logType = OS_LOG_TYPE_DEFAULT;
    } else if (kextLogLevel == kOSKextLogBasicLevel) {
        logType = OS_LOG_TYPE_DEFAULT;
    } else if (kextLogLevel < kOSKextLogDebugLevel) {
        logType = OS_LOG_TYPE_INFO;
    } else {
        logType = OS_LOG_TYPE_DEBUG;
    }

    va_start(ap, format);
    os_log_with_args(sKextLog, logType, format, ap, __builtin_return_address(0));
    va_end(ap);

    if (!sNewLoggingOnly) {
        // xxx - change to pick log stream based on log level
        // xxx - (0 == stdout, all others stderr)

        if (!g_log_stream) {
            g_log_stream = stderr;
        }

        va_start(ap, format);
        vfprintf(g_log_stream, format, ap);
        va_end(ap);

        fprintf(g_log_stream, "\n");
        fflush(g_log_stream);
    }

    return;
}

/*******************************************************************************
*******************************************************************************/
void log_CFError(
    OSKextRef     aKext __unused,
    OSKextLogSpec msgLogSpec,
    CFErrorRef    error)
{
    CFStringRef   errorString = NULL;  // must release
    char        * cstring     = NULL;  // must release

    if (!error) {
        return;
    }
    errorString = CFErrorCopyDescription(error);
    if (errorString) {
        cstring = createUTF8CStringForCFString(errorString);
        OSKextLog(/* kext */ NULL, msgLogSpec,
            "CFError descripton: %s.", cstring);
        SAFE_RELEASE_NULL(errorString);
        SAFE_FREE_NULL(cstring);
    }
    
    errorString = CFErrorCopyFailureReason(error);
    if (errorString) {
        cstring = createUTF8CStringForCFString(errorString);
        OSKextLog(/* kext */ NULL, msgLogSpec,
            "CFError reason: %s.", cstring);
        SAFE_RELEASE_NULL(errorString);
        SAFE_FREE_NULL(cstring);
    }
    
    return;
}

#if !TARGET_OS_EMBEDDED
// log helper for libbless, exported as SPI via bootroot.h
int32_t
BRBLLogFunc(void *refcon __unused, int32_t level, const char *string)
{
    OSKextLogSpec logSpec = kOSKextLogGeneralFlag;
    switch (level) {
    case kBLLogLevelVerbose:
        logSpec |= kOSKextLogDebugLevel;
        break;
    case kBLLogLevelError:
        logSpec |= kOSKextLogErrorLevel;
        break;
    default:
        logSpec |= kOSKextLogWarningLevel;
    }
    OSKextLog(NULL, logSpec, "%s", string);
    return 0;
}

/*******************************************************************************
 * returns TRUE and fills the Buffer with path to kernel extracted from
 * Kernelcache dictionary from /usr/standalone/bootcaches.plist on the
 * given volume.  If we can't find the path or if it does not exist or if
 * theBuffer is too small we return FALSE.
 *
 * If we find the kernel path in bootcaches.plist, but the file does not exist
 * we will copy the path into the buffer.  In that case the result will be FALSE
 * and strlen on buffer will be > 0.
 *
 * theVolRootURL == NULL means we want root volume.
 *******************************************************************************/
// FIXME: doesn't work on non-/ volumes due to SIP check in copyBootCachesDictForURL()
Boolean getKernelPathForURL(CFURLRef    theVolRootURL,
                            char *      theBuffer,
                            int         theBufferSize)
{
    CFDictionaryRef myDict              = NULL;     // must release
    CFDictionaryRef postBootPathsDict   = NULL;     // do not release
    CFDictionaryRef kernelCacheDict     = NULL;     // do not release
    Boolean         myResult            = FALSE;
   
    if (theBuffer) {
        *theBuffer = 0x00;
        
        myDict = copyBootCachesDictForURL(theVolRootURL);
        if (myDict != NULL) {
            postBootPathsDict = (CFDictionaryRef)
                CFDictionaryGetValue(myDict, kBCPostBootKey);
            
            if (postBootPathsDict &&
                CFGetTypeID(postBootPathsDict) == CFDictionaryGetTypeID()) {
                
                kernelCacheDict = (CFDictionaryRef)
                    CFDictionaryGetValue(postBootPathsDict, kBCKernelcacheV4Key);
                if (!kernelCacheDict) {
                    kernelCacheDict = (CFDictionaryRef)
                        CFDictionaryGetValue(postBootPathsDict, kBCKernelcacheV3Key);
                }
            }
        }
    } // theBuffer
    
    if (kernelCacheDict &&
        CFGetTypeID(kernelCacheDict) == CFDictionaryGetTypeID()) {
        CFStringRef     myTempStr;      // do not release
        
        myTempStr = (CFStringRef) CFDictionaryGetValue(kernelCacheDict,
                                                       kBCKernelPathKey);
        if (myTempStr != NULL &&
            CFGetTypeID(myTempStr) == CFStringGetTypeID()) {
            
            if (CFStringGetFileSystemRepresentation(myTempStr,
                                                    theBuffer,
                                                    theBufferSize)) {
                struct stat     statBuf;
                if (statPath(theBuffer, &statBuf) == EX_OK) {
                    myResult = TRUE;
                }
            }
        }
    } // kernelCacheDict
    
    SAFE_RELEASE(myDict);
        
    return(myResult);
}

/*******************************************************************************
 * returns copy of /usr/standalone/bootcaches.plist (as CFDictionary) from
 * given volume.
 *
 * theVolRootURL == NULL means we want root volume.
 *
 * Caller must release returned CFDictionaryRef.
 *******************************************************************************/
CFDictionaryRef copyBootCachesDictForURL(CFURLRef theVolRootURL) 
{
    CFStringRef             myVolRoot = NULL;           // must release
    CFStringRef             myPath = NULL;              // must release
    CFURLRef                myURL = NULL;               // must release
    CFDictionaryRef         myBootCachesPlist = NULL;   // do not release
    char *                  myCString = NULL;           // must free
    
    if (theVolRootURL) {
        myVolRoot = CFURLCopyFileSystemPath(theVolRootURL,
                                            kCFURLPOSIXPathStyle);
        if (myVolRoot == NULL) {
            goto finish;
        }
        myPath = CFStringCreateWithFormat(
                                          kCFAllocatorDefault,
                                          /* formatOptions */ NULL,
                                          CFSTR("%@%s"),
                                          myVolRoot,
                                          "/usr/standalone/bootcaches.plist" );
    }
    else {
        myPath = CFStringCreateWithCString(
                                           kCFAllocatorDefault,
                                           "/usr/standalone/bootcaches.plist",
                                           kCFStringEncodingUTF8 );
    }
    if (myPath == NULL) {
        goto finish;
    }
    
    myCString = createUTF8CStringForCFString(myPath);
    if (myCString == NULL) {
        goto finish;
    }
    if (rootless_check_trusted(myCString) != 0) {
        OSKextLog(/* kext */ NULL,
                  kOSKextLogErrorLevel  | kOSKextLogGeneralFlag,
                  "Untrusted file '%s' cannot be used",
                  myCString);
        goto finish;
    }
    
    myURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                          myPath,
                                          kCFURLPOSIXPathStyle,
                                          false );
    if (myURL && CFURLResourceIsReachable(myURL, NULL)) {
        CFReadStreamRef         readStream      = NULL;  // must release
        struct stat             myStatBuf;
        ExitStatus              myExitStatus;
        
        myExitStatus = statURL(myURL, &myStatBuf);
        if (myExitStatus != EX_OK) {
            goto finish;
        }
        if (myStatBuf.st_uid != 0) {
            goto finish;
        }
        if (myStatBuf.st_mode & S_IWGRP || myStatBuf.st_mode & S_IWOTH) {
            goto finish;
        }
        
        readStream = CFReadStreamCreateWithFile(kCFAllocatorDefault, myURL);
        if (readStream) {
            if (CFReadStreamOpen(readStream)) {
                /* read in contents of bootcaches.plist */
                myBootCachesPlist = CFPropertyListCreateWithStream(
                                                                   kCFAllocatorDefault,
                                                                   readStream,
                                                                   0,
                                                                   kCFPropertyListMutableContainersAndLeaves,
                                                                   NULL, NULL);
                CFReadStreamClose(readStream);
            }
            SAFE_RELEASE(readStream);
        }
    }
    
finish:    
    SAFE_RELEASE(myURL);
    SAFE_RELEASE(myPath);
    SAFE_RELEASE(myVolRoot);
    SAFE_FREE(myCString);

    return(myBootCachesPlist);
}

bool
translatePrelinkedToImmutablePath(const char *prelinked_path, char *imk_path, size_t imk_len)
{
    char plk_name[PATH_MAX] = {};
    char plk_path[PATH_MAX] = {};

    if (!prelinked_path || !imk_path || imk_len < strlen(_kOSKextPrelinkedKernelFileName))
        return false;

    if (!basename_r(prelinked_path, plk_name))
        return false;
    if (!dirname_r(prelinked_path, plk_path))
        return false;
    /*
     * If dirname_r() doesn't find any path component in 'prelinked_path'
     * either because the path is NULL, or because it's a simple filename,
     * then it will copy "." into plk_path. We want to translate a simple
     * filename, e.g. "prelinkedkernel.kasan", into "immutablekernel.kasan",
     * so we look for a return value of "." and simply clear the path
     * component of the name.
     */
    if (strncmp(plk_path, ".", 2) == 0)
        plk_path[0] = 0;

    // validate the prelinkedkernel name
    size_t plk_nm_len = strnlen(plk_name, sizeof(plk_name));
    size_t plk_pfx_len = strlen(_kOSKextPrelinkedKernelFileName);
    if (plk_nm_len < plk_pfx_len ||
        strncmp(plk_name, _kOSKextPrelinkedKernelFileName, plk_pfx_len) != 0) {
        OSKextLog(/* kext */ NULL,
                  kOSKextLogGeneralFlag | kOSKextLogErrorLevel,
                  "Cannot build immutable kernel using \"%s\": the filename must begin with \"%s\"",
                  prelinked_path, _kOSKextPrelinkedKernelFileName);
        return false;
    }

    // build the immutable kernel file name
    // note 'plk_path' contains the directory name from dirname_r above
    // (or NULL for filename only conversion)
    const char *plk_suffix = (const char *)((uintptr_t)plk_name + plk_pfx_len);
    if (strlcpy(imk_path, plk_path, imk_len) > imk_len ||
        strlcat(imk_path, kImmutableKernelFileName, imk_len) > imk_len ||
        strlcat(imk_path, plk_suffix, imk_len) > imk_len) {
        return false;
    }

    return true;
}
#endif   // !TARGET_OS_EMBEDDED

/*******************************************************************************
* safe_mach_error_string()
*******************************************************************************/
const char * safe_mach_error_string(mach_error_t error_code)
{
    const char * result = mach_error_string(error_code);
    if (!result) {
        result = "(unknown)";
    }
    return result;
}

#if PRAGMA_MARK
#pragma mark User Input
#endif /* PRAGMA_MARK */
/*******************************************************************************
* user_approve()
*
* Ask the user a question and wait for a yes/no answer.
*******************************************************************************/
int user_approve(Boolean ask_all, int default_answer, const char * format, ...)
{
    int     result = REPLY_YES;
    va_list ap;
    char    fake_buffer[2];
    int     output_length;
    char  * output_string;
    int     c, x;

    va_start(ap, format);
    output_length = vsnprintf(fake_buffer, 1, format, ap);
    va_end(ap);

    output_string = (char *)malloc(output_length + 1);
    if (!output_string) {
        result = REPLY_ERROR;
        goto finish;
    }

    va_start(ap, format);
    vsnprintf(output_string, output_length + 1, format, ap);
    va_end(ap);
    
    while ( 1 ) {
        fprintf(stderr, "%s [%s/%s", output_string,
            (default_answer == REPLY_YES) ? "Y" : "y",
            (default_answer == REPLY_NO)  ? "N" : "n");
        if (ask_all) {
            fprintf(stderr, "/%s",
                (default_answer == REPLY_ALL) ? "A" : "a");
        }
        fprintf(stderr, "]? ");
        fflush(stderr);

        c = fgetc(stdin);

        if (c == EOF) {
            result = REPLY_ERROR;
            goto finish;
        }

       /* Make sure we get a newline.
        */
        if ( c != '\n' ) {
            do {
                x = fgetc(stdin);
            } while (x != '\n' && x != EOF);

            if (x == EOF) {
                result = REPLY_ERROR;
                goto finish;
            }
        }

        if (c == '\n') {
            result = default_answer;
            goto finish;
        } else if (tolower(c) == 'y') {
            result = REPLY_YES;
            goto finish;
        } else if (tolower(c) == 'n') {
            result = REPLY_NO;
            goto finish;
        } else if (ask_all && tolower(c) == 'a') {
            result = REPLY_ALL;
            goto finish;
        } else {
            fprintf(stderr, "Please answer 'y' or 'n'%s.\n",
                ask_all ? " or 'a'" : "");
        }
    }

finish:
    if (output_string) free(output_string);

    return result;
}

/*******************************************************************************
* user_input()
*
* Ask the user for input.
*******************************************************************************/
const char * user_input(Boolean * eof, const char * format, ...)
{
    char * result = NULL;  // return value
    va_list ap;
    char fake_buffer[2];
    int output_length;
    char * output_string = NULL;
    unsigned index;
    size_t size = 80;  // more than enough to input a hex address
    int c;

    if (eof) {
        *eof = false;
    }

    result = (char *)malloc(size);
    if (!result) {
        goto finish;
    }
    index = 0;

    va_start(ap, format);
    output_length = vsnprintf(fake_buffer, 1, format, ap);
    va_end(ap);

    output_string = (char *)malloc(output_length + 1);
    if (!output_string) {
        if (result) free(result);
        result = NULL;
        goto finish;
    }

    va_start(ap, format);
    vsnprintf(output_string, output_length + 1, format, ap);
    va_end(ap);

    fprintf(stderr, "%s ", output_string);
    fflush(stderr);

    c = fgetc(stdin);
    while (c != '\n' && c != EOF) {
        if (index >= (size - 1)) {
            fprintf(stderr, "input line too long\n");
            if (result) free(result);
            result = NULL;
            goto finish;
        }
        result[index++] = (char)c;
        c = fgetc(stdin);
    }

    result[index] = '\0';

    if (c == EOF) {
        if (result) free(result);
        result = NULL;
        if (eof) {
            *eof = true;
        }
        goto finish;
    }

finish:
    if (output_string) free(output_string);

    return result;
}

#if PRAGMA_MARK
#pragma mark Caches
#endif /* PRAGMA_MARK */
/*******************************************************************************
*******************************************************************************/
Boolean readSystemKextPropertyValues(
    CFStringRef        propertyKey,
    const NXArchInfo * arch,
    Boolean            forceUpdateFlag,
    CFArrayRef       * valuesOut)
{
    Boolean                result                  = false;
    CFArrayRef             sysExtensionsFolderURLs = OSKextGetSystemExtensionsFolderURLs();
    CFMutableArrayRef      values                  = NULL;  // must release
    CFStringRef            cacheBasename           = NULL;  // must release
    CFArrayRef             kexts                   = NULL;  // must release
    CFMutableDictionaryRef newDict                 = NULL;  // must release
    CFStringRef            kextPath                = NULL;  // must release
    CFTypeRef              value                   = NULL;  // do not release
    CFStringRef            kextVersion             = NULL;  // do not release
    CFIndex                count, i;

    cacheBasename = CFStringCreateWithFormat(kCFAllocatorDefault,
        /* formatOptions */ NULL, CFSTR("%s%@"),
        _kKextPropertyValuesCacheBasename,
        propertyKey);
    if (!cacheBasename) {
        OSKextLogMemError();
        goto finish;
    }

    if (OSKextGetUsesCaches() && !forceUpdateFlag) {

       /* See if we have an up-to-date cache containing an array, and return
        * that if we have one.
        */
        if (_OSKextReadCache(sysExtensionsFolderURLs, cacheBasename,
            arch, _kOSKextCacheFormatCFXML, /* parseXML? */ true,
            (CFPropertyListRef *)&values)) {

            if (values && CFGetTypeID(values) == CFArrayGetTypeID()) {
                result = true;
                goto finish;
            }
        }
    }

    values = CFArrayCreateMutable(kCFAllocatorDefault, /* capacity */ 0,
        &kCFTypeArrayCallBacks);
    if (!values) {
        OSKextLogMemError();
        goto finish;
    }

    kexts = OSKextCreateKextsFromURLs(kCFAllocatorDefault,
    sysExtensionsFolderURLs);

    if (!kexts) {
        // Create function should log error
        goto finish;
    }

    count = CFArrayGetCount(kexts);

    for (i = 0; i < count; i++) {
        OSKextRef aKext = (OSKextRef)CFArrayGetValueAtIndex(kexts, i);

        SAFE_RELEASE_NULL(newDict);
        SAFE_RELEASE_NULL(kextPath);
        // do not release kextVersion
        kextVersion = NULL;

        if ((OSKextGetSimulatedSafeBoot() || OSKextGetActualSafeBoot()) &&
            !OSKextIsLoadableInSafeBoot(aKext)) {

            continue;
        }
        //??? if (OSKextGetLoadFailed(aKext)) continue;  -- don't have in OSKext

        value = OSKextGetValueForInfoDictionaryKey(aKext, propertyKey);
        if (!value) {
            continue;
        }

        newDict = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        if (!newDict) {
            goto finish;
        }

        CFDictionarySetValue(newDict, CFSTR("Data"), value);

        CFDictionarySetValue(newDict, CFSTR("CFBundleIdentifier"),
            OSKextGetIdentifier(aKext));

        kextPath = copyKextPath(aKext);
        if (!kextPath) {
            goto finish;
        }
        CFDictionarySetValue(newDict, CFSTR("OSBundlePath"), kextPath);

        kextVersion = OSKextGetValueForInfoDictionaryKey(aKext,
            CFSTR("CFBundleVersion"));
        if (!kextVersion) {
            goto finish;
        }
        CFDictionarySetValue(newDict, CFSTR("CFBundleVersion"),
            kextVersion);

        CFArrayAppendValue(values, newDict);
    }

    if (OSKextGetUsesCaches() || forceUpdateFlag) {
        _OSKextWriteCache(sysExtensionsFolderURLs, cacheBasename,
            arch, _kOSKextCacheFormatCFXML, values);
    }

    result = true;

finish:
    if (result && valuesOut && values) {
        *valuesOut = (CFArrayRef)CFRetain(values);
    }

    SAFE_RELEASE(values);
    SAFE_RELEASE(cacheBasename);
    SAFE_RELEASE(kexts);
    SAFE_RELEASE(newDict);
    SAFE_RELEASE(kextPath);

    return result;
}

void
setVariantSuffix(void)
{
    char* variant = 0;
    size_t len = 0;
    int result;
    result = sysctlbyname("kern.osbuildconfig", NULL, &len, NULL, 0);
    if (result == 0) {
        variant = (char *)malloc(len + 2);
        variant[0] = '_';
        result = sysctlbyname("kern.osbuildconfig", &variant[1], &len, NULL, 0);
        if (result == 0) {
            OSKextLog(/* kext */ NULL,
                kOSKextLogDebugLevel,
                "variant is %s",variant);
            if (strcmp(&variant[1], "release") != 0) {
               OSKextSetExecutableSuffix(variant, NULL);
            }
        } else {
            OSKextLog(/* kext */ NULL,
                kOSKextLogErrorLevel,
                "kern.osbuildconfig failed after reporting return size of size %d",(int) len);
        }
        free(variant);
    } else {
        OSKextLog(/* kext */ NULL,
            kOSKextLogErrorLevel,
            "Impossible to query kern.osbuildconfig");
    }
}
