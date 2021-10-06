#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFBundlePriv.h>
#include <err.h>    	// warn[x]
#include <errno.h>
#include <libc.h>
#include <stdlib.h> 	// devname()
#include <libgen.h>	// dirname()
#include <Kernel/libsa/mkext.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fts.h>
#include <mach-o/arch.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_port.h>	// mach_port_allocate()
#include <mach/mach_types.h>
#include <mach/machine/vm_param.h>
#include <mach/kmod.h>
#include <servers/bootstrap.h>	// bootstrap mach ports
#include <unistd.h>		// sleep(3)


#include <IOKit/kext/KXKextManager.h>
#include <IOKit/kext/kextmanager_types.h>
#include <IOKit/IOTypes.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOKitServer.h>
#include <IOKit/IOCFUnserialize.h>
#include <IOKit/IOCFSerialize.h>
#include <libkern/OSByteOrder.h>

#include "bootroot.h"
#include "update_boot.h"

// XX need to export kextmanager.h from IOKitUser
// #include "kextmanager.h"	// since we didn't want to generate a .c file
kern_return_t kextmanager_lock_volume(mach_port_t p,
			    mach_port_t client, char *volDev, int *lockstatus);
kern_return_t kextmanager_unlock_volume(mach_port_t p,
			    mach_port_t client, char *volDev, int result);


// XX: switch to bootfiles.h when we get the chance
#define DEFAULT_CACHE_DIR	"/System/Library/Caches/com.apple.kernelcaches"
#define SA_HAS_RUN_PATH		"/var/db/.AppleSetupDone"
#define kKXROMExtensionsFolder  "/System/Library/Caches/com.apple.romextensions/"
#define TEMP_DIR		"/com.apple.iokit.kextcache.mkext.XX"

#define LCK_MAXTRIES 10
#define LCK_DELAY    30 	// up to five minutes of waiting

/*******************************************************************************
* Program Globals
*******************************************************************************/
char * progname = "(unknown)";

/*******************************************************************************
* File-Globals
*******************************************************************************/
static mach_port_t sLockPort = nil;
static mach_port_t sKextdPort = nil;

/*******************************************************************************
* Extern functions.
*******************************************************************************/

// In compression.c
__private_extern__ u_int32_t
local_adler32(u_int8_t *buffer, int32_t length);

// In arch.c
__private_extern__ void
find_arch(u_int8_t **dataP, off_t *sizeP, cpu_type_t in_cpu,
    cpu_subtype_t in_cpu_subtype, u_int8_t *data_ptr, off_t filesize);
__private_extern__ int
get_arch_from_flag(char *name, cpu_type_t *cpuP, cpu_subtype_t *subcpuP);

// in mkext_file.c
ssize_t createMkextArchive(
    int fd,
    CFDictionaryRef kextDict,
    const char * mkextFilename,
    const char * archName,
    cpu_type_t archCPU,
    cpu_subtype_t archSubtype,
    int verbose_level);
Boolean checkMkextArchiveSize( ssize_t size );

// in prelink.c
KXKextManagerError
PreLink(KXKextManagerRef theKextManager, CFDictionaryRef kextDict,
        const char * kernelCacheFilename,
	const struct timeval *cacheFileTimes,
	const char * kernel_file_name, 
	const char * platform_name,
	const char * root_path,
	CFSetRef kernel_requests,
	Boolean all_plists,
	const NXArchInfo * arch,
	int verbose_level, Boolean debug_mode);

/*******************************************************************************
* Utility and callback functions.
*******************************************************************************/
char * CFURLCopyCString(CFURLRef anURL);
static void addKextForMkextCache(
    KXKextRef theKext,
    CFMutableDictionaryRef checkDictionary,
    cpu_type_t archCPU,
    cpu_subtype_t archSubtype,
    Boolean local,
    Boolean network,
    Boolean safeboot,
    CFSetRef kernel_requests,
    int verbose_level,
    Boolean do_tests);
static void collectKextsForMkextCache(
    CFArrayRef kexts,
    CFMutableDictionaryRef checkDictionary,
    cpu_type_t archCPU,
    cpu_subtype_t archSubtype,
    Boolean local,
    Boolean network,
    Boolean safeboot,
    CFSetRef kernel_requests,
    int verbose_level,
    Boolean do_tests);

// locking: "put" and "take" indicate that routines decide if a lock is needed
// int takeVolumeForPaths(char *volPath, int filec, const char *files[]);
static int takeVolumeForPath(const char *path);
static int takeVolume(dev_t devid);
// static int putVolumeForPath(const char *path, int status);

__private_extern__ void verbose_log(const char * format, ...);
static void error_log(const char * format, ...);
static int user_approve(int default_answer, const char * format, ...);
static const char * user_input(const char * format, ...);
static Boolean addKextsToManager(
    KXKextManagerRef aManager,
    CFArrayRef kextNames,
    CFMutableArrayRef kextNamesToUse,
    Boolean do_tests);
static void get_catalog_demand_lists(CFMutableSetRef * kernel_requests,
				      CFSetRef * kernel_cache_misses,
				      int verbose_level);
static void usage(int level);

#define kMaxArchs 64
#define kRootPathLen 256

/*******************************************************************************
*******************************************************************************/

int main(int argc, const char * argv[])
{
    int exit_code = 0;
    int optchar;
    KXKextManagerRef theKextManager = NULL;  // must release
    KXKextManagerError result;

    CFIndex i, count;

   /*****
    * Set by command line option flags.
    * XX a struct would allow much simpler initialization
    * *and* allow argument processing elsewhere.
    */
    Boolean do_tests = false;                // -t
    Boolean forkExit = false;                // -F
    Boolean local_for_repositories = false;  // -l
    Boolean network_for_repositories = false;// -n
    Boolean safeboot_for_repositories = false; // -s
    Boolean local_for_all = false;           // -L
    Boolean network_for_all = false;         // -N
    Boolean safeboot_for_all = false;        // -S
    Boolean repositoryCaches = false;        // -k
    const char * mkextFilename = NULL;       // -m; don't release
    char kernelCacheBuffer[PATH_MAX];
    const char * kernelCacheFilename = NULL; // -c; don't release
    Boolean pretend_authentic = false;       // -z
    Boolean load_in_task = false;
    Boolean include_kernel_requests = false;
    Boolean cache_looks_uptodate = false;
    Boolean debug_mode = false;		     // -d
    Boolean forceUpdate = false;             // -f
    char *updateRoot = NULL;		     // -u
    int verbose_level = 0;		     // -v
    const char * default_kernel_file = "/mach_kernel";
    const char * kernel_file = default_kernel_file;  // overriden by -K option
    const char * source_extensions    = "/System/Library/Extensions";
    int fd = -1;
    const char * output_filename = NULL;
    const char * temp_dir;
    char * temp_file = NULL;

    struct stat kernel_stat_buf;
    struct stat extensions_stat_buf;
    struct stat rom_extensions_stat_buf;
    Boolean have_kernel_time, have_extensions_time;
    Boolean need_default_kernelcache_info = false;
    struct timeval _cacheFileTimes[2];
    struct timeval *cacheFileTimes = NULL;

    struct {
	char platform_name[64];
	char root_path[kRootPathLen];
    } platform_name_root_path;
    io_registry_entry_t entry;
    Boolean all_plists;

    // -a for these three
    NXArchInfo archAny = {
        "any",
        CPU_TYPE_ANY,
        CPU_SUBTYPE_MULTIPLE
    };

    CFMutableArrayRef repositoryDirectories = NULL;  // must release
    CFMutableArrayRef repositories = NULL;           // must release
    CFMutableArrayRef kextNames = NULL;              // args; must release
    CFMutableArrayRef kextNamesToUse = NULL;         // must release
    CFMutableArrayRef namedKexts = NULL;             // must release
    CFMutableArrayRef repositoryKexts = NULL;        // must release
    CFMutableDictionaryRef checkDictionary = NULL;   // must release
    CFMutableSetRef   kernel_requests = NULL;	     // must release
    CFSetRef	      kernel_cache_misses = NULL;    // must release
    const NXArchInfo *archs[kMaxArchs] = {NULL};
    int nArchs = 0;

   /*****
    * Find out what the program was invoked as.
    */
    progname = rindex(argv[0], '/');
    if (progname) {
        progname++;   // go past the '/'
    } else {
        progname = (char *)argv[0];
    }

   /*****
    * Allocate collection objects.
    */
    repositoryDirectories = CFArrayCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeArrayCallBacks);
    if (!repositoryDirectories) {
        exit_code = 1;
        fprintf(stderr, "memory allocation failure\n");
        goto finish;
    }

    repositories = CFArrayCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeArrayCallBacks);
    if (!repositories) {
        exit_code = 1;
        fprintf(stderr, "memory allocation failure\n");
        goto finish;
    }

    kextNames = CFArrayCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeArrayCallBacks);
    if (!kextNames) {
        exit_code = 1;
        fprintf(stderr, "memory allocation failure\n");
        goto finish;
    }

    kextNamesToUse = CFArrayCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeArrayCallBacks);
    if (!kextNamesToUse) {
        exit_code = 1;
        fprintf(stderr, "memory allocation failure\n");
        goto finish;
    }

    namedKexts = CFArrayCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeArrayCallBacks);
    if (!namedKexts) {
        exit_code = 1;
        fprintf(stderr, "memory allocation failure\n");
        goto finish;
    }

    repositoryKexts = CFArrayCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeArrayCallBacks);
    if (!repositoryKexts) {
        exit_code = 1;
        fprintf(stderr, "memory allocation failure\n");
        goto finish;
    }

    checkDictionary = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!checkDictionary) {
        exit_code = 1;
        fprintf(stderr, "memory allocation failure\n");
        goto finish;
    }

    bzero(&platform_name_root_path, sizeof(platform_name_root_path));
    entry = IORegistryEntryFromPath(kIOMasterPortDefault, kIOServicePlane ":/");
    if (entry)
    {
	if (KERN_SUCCESS != IORegistryEntryGetName(entry, platform_name_root_path.platform_name))
	    platform_name_root_path.platform_name[0] = 0;
	IOObjectRelease(entry);
    }

    entry = IORegistryEntryFromPath(kIOMasterPortDefault, kIODeviceTreePlane ":/chosen");
    if (entry)
    {
	CFTypeRef obj = 0;
	obj = IORegistryEntryCreateCFProperty(entry, CFSTR("rootpath"), kCFAllocatorDefault, kNilOptions);
	if (obj && (CFGetTypeID(obj) == CFDataGetTypeID()))
	{
	    CFIndex len = CFDataGetLength((CFDataRef) obj);
	    strncpy(platform_name_root_path.root_path, (char *)CFDataGetBytePtr((CFDataRef) obj), len);
	    platform_name_root_path.root_path[len] = 0;
	} else {
            const char *data;
            char *ptr = platform_name_root_path.root_path;
            CFIndex len;
            // Construct entry from UUID of boot volume and kernel name.
            obj = 0;
            do {
                obj = IORegistryEntryCreateCFProperty(entry, CFSTR("boot-device-path"), kCFAllocatorDefault, kNilOptions);
                if (!obj)
                    break;

                if (CFGetTypeID(obj) == CFDataGetTypeID()) {
                    data = (char *)CFDataGetBytePtr((CFDataRef) obj);
                    len = CFDataGetLength((CFDataRef) obj);
                } else if (CFGetTypeID(obj) == CFStringGetTypeID()) {
                    data = CFStringGetCStringPtr((CFStringRef) obj, kCFStringEncodingUTF8);
                    if (!data)
                        break;
                    len = strlen(data) + 1; // include trailing null
                } else {
                    break;
                }
                if (len > kRootPathLen)
                    len = kRootPathLen;
                memcpy(ptr, data, len);
                ptr += len;

                CFRelease(obj);

                obj = IORegistryEntryCreateCFProperty(entry, CFSTR("boot-file"), kCFAllocatorDefault, kNilOptions);
                if (!obj)
                    break;

                if (CFGetTypeID(obj) == CFDataGetTypeID()) {
                    data = (char *)CFDataGetBytePtr((CFDataRef) obj);
                    len = CFDataGetLength((CFDataRef) obj);
                } else if (CFGetTypeID(obj) == CFStringGetTypeID()) {
                    data = CFStringGetCStringPtr((CFStringRef) obj, kCFStringEncodingUTF8);
                    if (!data)
                        break;
                    len = strlen(data);
                } else {
                    break;
                }
                if ((ptr - platform_name_root_path.root_path + len) >= kRootPathLen)
                    len = kRootPathLen - (ptr - platform_name_root_path.root_path);
                memcpy(ptr, data, len);
            } while (0);
        }
        if (obj)
            CFRelease(obj);
	IOObjectRelease(entry);
    }
    if (!platform_name_root_path.platform_name[0] || !platform_name_root_path.root_path[0])
	platform_name_root_path.platform_name[0] = platform_name_root_path.root_path[0] = 0;

    /*****
    * Process command line arguments. If running in kextload-compatibiliy
    * mode, accept its old set of options and none other. If running in
    * the new mode, process the new, larger set of options.
    */
    while ((optchar = getopt(argc, (char * const *)argv,
               "a:cdefFhkK:lLm:nNrsStu:vz")) != -1) {

        switch (optchar) {

          case 'a':
            if (nArchs >= kMaxArchs) {
                  fprintf(stderr, "maximum of %d architectures supported\n", kMaxArchs);
                  exit_code = 1;
                  goto finish;
            }
            archs[nArchs] = NXGetArchInfoFromName(optarg);
            if (archs[nArchs] == NULL) {
                fprintf(stderr, "unknown CPU arch %s\n\n", optarg);
                usage(0);
                exit_code = 1;
                goto finish;
            }
            ++nArchs;
            break;

          case 'r':
	    include_kernel_requests = true;
	    break;

          case 'd':
	    debug_mode = true;
	    break;

          case 'e':
            if (mkextFilename) {
                fprintf(stderr, "an output filename has already been set\n\n");
                usage(0);
                exit_code = 1;
                goto finish;
            }
            mkextFilename = "/System/Library/Extensions.mkext";
            CFArrayAppendValue(repositoryDirectories, kKXSystemExtensionsFolder);
            break;

          case 'F':
            forkExit = true;
            break;

          case 'K':
            kernel_file = optarg;
            break;

          case 'k':
            repositoryCaches = true;
            break;

          case 'l':
            local_for_repositories = true;
            break;

          case 'L':
            local_for_all = true;
            break;

          case 'c':
            if (kernelCacheFilename) {
                fprintf(stderr, "an output filename has already been set\n\n");
                usage(0);
                exit_code = 1;
                goto finish;
            }

	    if (optind >= argc) {
                need_default_kernelcache_info = true;
	    } else {
		kernelCacheFilename = argv[optind++];
	    }
            break;

          case 'm':
            if (mkextFilename) {
                fprintf(stderr, "an mkext filename has already been set\n\n");
                usage(0);
                exit_code = 1;
                goto finish;
            }
            mkextFilename = optarg;
            break;

          case 'n':
            network_for_repositories = true;
            break;

          case 'N':
            network_for_all = true;
            break;


          case 's':
            safeboot_for_repositories = true;
            break;

          case 'S':
            safeboot_for_all = true;
            break;

          case 't':
            do_tests = true;
            break;

	  case 'u':
	    updateRoot = optarg;
	    break;
	  
	  case 'f':
	    forceUpdate = true;
	    break;

          case 'v':
            {
                const char * next;

                if (verbose_level > 0) {
                    fprintf(stderr, "duplicate use of -v option\n\n");
                    usage(0);
                    exit_code = 1;
                    goto finish;
                }
                if (optind >= argc) {
                    verbose_level = 1;
                } else {
                    next = argv[optind];
                    if ((next[0] == '1' || next[0] == '2' ||
                         next[0] == '3' || next[0] == '4' ||
                         next[0] == '5' || next[0] == '6') &&
                         next[1] == '\0') {

                        verbose_level = atoi(next);
                        optind++;
                    } else {
                        verbose_level = 1;
                    }
                }
            }
            break;

          case 'z':
            pretend_authentic = true;
            break;

	case 'h':
	  usage(2);
	  exit_code = 0;
	  goto finish;

	case '?':
	  usage(0);
	  exit_code = 1;	// should be EX_USAGE (sysexits.h)
	  goto finish;

        default:
            fprintf(stderr, "unknown option -%c\n", optchar);
            usage(0);
            exit_code = 1;
            goto finish;
        }
    }

   /* Try a lock on the volume for the mkext being updated.
    */
    if (mkextFilename) {
        result = takeVolumeForPath(mkextFilename);
        if (result) {
            goto finish;
        }
    }

   /* Get the default kernel timestamps before mucking with kernelcache info.
    */
    have_kernel_time     = (0 == stat(kernel_file,       &kernel_stat_buf));
    have_extensions_time = (0 == stat(source_extensions, &extensions_stat_buf));

    if (have_kernel_time || have_extensions_time)
    {
	cacheFileTimes = _cacheFileTimes;
	if (!have_kernel_time 
	    || (have_extensions_time && (extensions_stat_buf.st_mtime > kernel_stat_buf.st_mtime)))
	{
	    TIMESPEC_TO_TIMEVAL(&cacheFileTimes[0], &extensions_stat_buf.st_atimespec);
	    TIMESPEC_TO_TIMEVAL(&cacheFileTimes[1], &extensions_stat_buf.st_mtimespec);
	}
	else
	{
	    TIMESPEC_TO_TIMEVAL(&cacheFileTimes[0], &kernel_stat_buf.st_atimespec);
	    TIMESPEC_TO_TIMEVAL(&cacheFileTimes[1], &kernel_stat_buf.st_mtimespec);
	}
	cacheFileTimes[1].tv_sec++;
    }

   /* If -c was given as the last argument, get the default kernelcache info
    * using information gleaned from the previous default kernel file stats.
    */
    if (need_default_kernelcache_info) {
        // default args for kextd's usage

        kernelCacheFilename = DEFAULT_CACHE_DIR "/kernelcache";
        struct stat cache_stat_buf;

        if ((-1 == stat(DEFAULT_CACHE_DIR, &cache_stat_buf))
            || !(S_IFDIR & cache_stat_buf.st_mode)) {
            mkdir(DEFAULT_CACHE_DIR, 0755);
        }
        // Make sure we scan the standard Extensions folder.
        CFArrayAppendValue(repositoryDirectories,
            kKXSystemExtensionsFolder);
        // Make sure we scan the ROM Extensions folder.
        if (0 == stat(kKXROMExtensionsFolder, &rom_extensions_stat_buf))
            CFArrayAppendValue(repositoryDirectories, CFSTR(kKXROMExtensionsFolder));

        if (!platform_name_root_path.platform_name[0]) {
            // need more than the minimal set
            local_for_repositories = true;
        }

        if (!include_kernel_requests) {
            // this cache isn't tied to a config
            platform_name_root_path.root_path[0] = platform_name_root_path.platform_name[0] = 0;
        }

        if (platform_name_root_path.platform_name[0] || platform_name_root_path.root_path[0])
        {
            sprintf(kernelCacheBuffer, "%s.%08X", 
                kernelCacheFilename,
                NXSwapHostIntToBig(local_adler32((u_int8_t *) &platform_name_root_path, 
                                    sizeof(platform_name_root_path))));
            kernelCacheFilename = kernelCacheBuffer;
        }

        if ((0 == stat(kernelCacheFilename, &cache_stat_buf))
            && have_kernel_time && have_extensions_time)
        {
            if ((cache_stat_buf.st_mtime > kernel_stat_buf.st_mtime)
             &&  (cache_stat_buf.st_mtime > extensions_stat_buf.st_mtime)
             &&  (cache_stat_buf.st_mtime == cacheFileTimes[1].tv_sec))
                cache_looks_uptodate = true;
        }

        if (-1 == stat(SA_HAS_RUN_PATH, &cache_stat_buf)) {
            if (verbose_level >= 1) {
                verbose_log("SetupAssistant not yet run");
            }
            exit(0);
        }
    }
    
   /* Update argc, argv based on option processing.
    */
    argc -= optind;
    argv += optind;

   /*****
    * Check for bad combinations of options.
    */
    if (!mkextFilename && !kernelCacheFilename && !repositoryCaches
	    && !updateRoot) {
        fprintf(stderr, "no work to do; one of -m, -c, -k, or -u must be specified\n");
        usage(1);
        exit_code = 1;
        goto finish;
    }

    if (updateRoot) {
	struct stat sb;

	if (mkextFilename || kernelCacheFilename || repositoryCaches) {
	    fprintf(stderr, "-u (auto-update) incompatible with other flags\n");
	    usage(0);
	    exit_code = 1;
	    goto finish;
	}

	if (stat(updateRoot, &sb)) {
	    warn("%s", updateRoot);
	    exit_code = 1;
	} else {
	    exit_code = updateBoots(updateRoot, argc, argv, forceUpdate,
				/*gross*/verbose_level);
	}

	goto finish;
    }

   /*****
    * Record the kext/directory names from the command line.
    */
    for (i = 0; i < argc; i++) {
        CFStringRef argString = CFStringCreateWithCString(kCFAllocatorDefault,
              argv[i], kCFStringEncodingMacRoman);
        if (!argString) {
            fprintf(stderr, "memory allocation failure\n");
            exit_code = 1;
            goto finish;
        }

        // FIXME: Use a more robust test here?
        if (CFStringHasSuffix(argString, CFSTR(".kext")) ||
            CFStringHasSuffix(argString, CFSTR(".kext/")) ) {
            CFArrayAppendValue(kextNames, argString);
        }
        else
        {
            if (!CFArrayGetCount(repositoryDirectories))
            {
                source_extensions = argv[i];
                have_extensions_time = (0 == stat(source_extensions, &extensions_stat_buf));
                if (have_kernel_time || have_extensions_time)
                {
                    cacheFileTimes = _cacheFileTimes;
                    if (!have_kernel_time 
                        || (have_extensions_time && (extensions_stat_buf.st_mtime > kernel_stat_buf.st_mtime)))
                    {
                        TIMESPEC_TO_TIMEVAL(&cacheFileTimes[0], &extensions_stat_buf.st_atimespec);
                        TIMESPEC_TO_TIMEVAL(&cacheFileTimes[1], &extensions_stat_buf.st_mtimespec);
                    }
                    else
                    {
                        TIMESPEC_TO_TIMEVAL(&cacheFileTimes[0], &kernel_stat_buf.st_atimespec);
                        TIMESPEC_TO_TIMEVAL(&cacheFileTimes[1], &kernel_stat_buf.st_mtimespec);
                    }
                    cacheFileTimes[1].tv_sec++;
                }
            }
            CFArrayAppendValue(repositoryDirectories, argString);
        }
        CFRelease(argString);
    }

    if (CFArrayGetCount(kextNames) == 0 &&
        CFArrayGetCount(repositoryDirectories) == 0) {

        fprintf(stderr, "no kernel extensions or directories specified\n\n");
        usage(0);
        exit_code = 1;
        goto finish;
    }

    if (forkExit) {
        if (verbose_level >= 1) {
            verbose_log("forking a child process to do the work\n");
        }

        switch (fork()) {
          case -1:
            fprintf(stderr, "cannot fork\n");
            exit_code = 1;
            goto finish;
            break;
          case 0:   // child task
            setpriority(PRIO_PROCESS, getpid(), 20); // run at really low priority
	    if (include_kernel_requests) {
		kern_return_t    kern_result;
		mach_timespec_t  waitTime = { 40, 0 };

		kern_result = IOKitWaitQuiet(kIOMasterPortDefault, &waitTime);
		if (kern_result == kIOReturnTimeout) {
		    fprintf(stderr, "IOKitWaitQuiet() timed out");
		} else if (kern_result != kIOReturnSuccess) {
		    fprintf(stderr, "IOKitWaitQuiet() failed with result code %x",
				    kern_result);
		}
		sleep(40);
	    }
            break;
          default:  // parent task
            exit_code = 0;
            goto finish;
            break;
        }
    }

    if (include_kernel_requests)
	get_catalog_demand_lists(&kernel_requests, &kernel_cache_misses, verbose_level);

    if (cache_looks_uptodate)
    {
	CFShow(kernel_cache_misses);
	CFShow(kernel_requests);
	if ((!kernel_cache_misses || !kernel_requests)
	    || (!CFSetGetCount(kernel_cache_misses))
	    || (CFSetGetCount(kernel_cache_misses) == CFSetGetCount(kernel_requests)))
	{
	    if (verbose_level >= 1) {
		verbose_log("cache %s up to date", kernelCacheFilename);
	    }
	    exit(0);
	}
    }

   /*****
    * Set up the kext manager.
    */

    theKextManager = KXKextManagerCreate(kCFAllocatorDefault);
    if (!theKextManager) {
        fprintf(stderr, "can't allocate kernel extension manager\n");
        exit_code = 1;
        goto finish;
    }

    result = KXKextManagerInit(theKextManager, load_in_task,
        false /* safeBoot */);
    if (result != kKXKextManagerErrorNone) {
        fprintf(stderr, "can't initialize kernel extension manager (%s)\n",
            KXKextManagerErrorStaticCStringForError(result));
        exit_code = 1;
        goto finish;
    }

    KXKextManagerSetPerformsFullTests(theKextManager, do_tests);
    KXKextManagerSetPerformsStrictAuthentication(theKextManager, true);
    KXKextManagerSetLogLevel(theKextManager, verbose_level);
    KXKextManagerSetLogFunction(theKextManager, &verbose_log);
    KXKextManagerSetErrorLogFunction(theKextManager, &error_log);
    KXKextManagerSetUserVetoFunction(theKextManager, &user_approve);
    KXKextManagerSetUserApproveFunction(theKextManager, &user_approve);
    KXKextManagerSetUserInputFunction(theKextManager, &user_input);

   /*****
    * Disable clearing of relationships until we're done putting everything
    * together.
    */
    KXKextManagerDisableClearRelationships(theKextManager);

   /*****
    * Add the extensions folders specified with -r to the manager.
    * And collect all their kexts--good and bad--into the repositoryKexts
    * array.
    */
    count = CFArrayGetCount(repositoryDirectories);
    for (i = 0; i < count; i++) {
        CFURLRef directoryURL = NULL;          // must release
        KXKextRepositoryRef repository = NULL; // don't release
        CFArrayRef candidateKexts = NULL;      // must release
        CFArrayRef badKexts = NULL;            // must release

        CFStringRef directory = (CFStringRef)CFArrayGetValueAtIndex(
            repositoryDirectories, i);
        directoryURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                directory, kCFURLPOSIXPathStyle, true);
        if (!directoryURL) {
            char repositoryPath[MAXPATHLEN];
            if (!CFStringGetCString(directory, repositoryPath,
                sizeof(repositoryPath) / sizeof(char), kCFStringEncodingMacRoman)) {

                fprintf(stderr, "string conversion error\n");

            } else {
                fprintf(stderr, "'%s': no such directory\n", repositoryPath);
            }
            exit_code = 1;

            goto finish;
        }

        result = KXKextManagerAddRepositoryDirectory(theKextManager,
            directoryURL, true /* scanForKexts */,
            false /* use_repository_caches */, &repository);
        if (result != kKXKextManagerErrorNone) {
            fprintf(stderr, "can't add repository (%s).\n",
                KXKextManagerErrorStaticCStringForError(result));
	    continue;
        }

        CFArrayAppendValue(repositories, repository);

        candidateKexts = KXKextRepositoryCopyCandidateKexts(repository);
        if (candidateKexts) {
            CFArrayAppendArray(repositoryKexts, candidateKexts,
                CFRangeMake(0, CFArrayGetCount(candidateKexts)));
            CFRelease(candidateKexts);
            candidateKexts = NULL;
        }

        badKexts = KXKextRepositoryCopyBadKexts(repository);
        if (badKexts) {
            CFArrayAppendArray(repositoryKexts, badKexts,
                CFRangeMake(0, CFArrayGetCount(badKexts)));
            CFRelease(badKexts);
            badKexts = NULL;
        }

        CFRelease(directoryURL);
        directoryURL = NULL;
    }

   /*****
    * Add each kext named on the command line to the manager and collect
    * their names in the kextNamesToUse array.
    */
    if (!addKextsToManager(theKextManager, kextNames, kextNamesToUse, do_tests)) {
        exit_code = 1;
        goto finish;
    }

    if (pretend_authentic) {
        // Yes, do this even if do_tests is true; -tz means fake authentication.
        KXKextManagerMarkKextsAuthentic(theKextManager);
    } else if (do_tests) {
        KXKextManagerAuthenticateKexts(theKextManager);
    }

    KXKextManagerEnableClearRelationships(theKextManager);

    KXKextManagerCalculateVersionRelationships(theKextManager);
    KXKextManagerResolveAllKextDependencies(theKextManager);

   /*****
    * Write kextcache files for repositories.
    */
    if (repositoryCaches) {
        count = CFArrayGetCount(repositories);
        for (i = 0; i < count; i++) {
            KXKextRepositoryRef repository = (KXKextRepositoryRef)
                CFArrayGetValueAtIndex(repositories, i);
            /*KXKextManagerError kmResult = */ KXKextRepositoryWriteCache(
                repository, NULL);
            // FIXME: Do anything with kmResult?
        }
    }

   /*****
    * Do the cache files.
    */
    if (!mkextFilename && !kernelCacheFilename) {
        goto finish;
    }

   /*****
    * Get KXKextRef objects for each of the kexts named on the command line.
    */
    count = CFArrayGetCount(kextNamesToUse);
    for (i = 0; i < count; i++) {
        CFStringRef kextName = CFArrayGetValueAtIndex(kextNamesToUse, i);
        CFURLRef kextURL = NULL;  // must release
        KXKextRef thisKext = NULL;

        kextURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
            kextName, kCFURLPOSIXPathStyle, true);
        if (!kextURL) {
            fprintf(stderr, "memory allocation failure\n");
            exit_code = 1;
            goto finish;
        }

        thisKext = KXKextManagerGetKextWithURL(theKextManager,
            kextURL);
        CFRelease(kextURL);
        if (!thisKext) {
            fprintf(stderr, "internal error; kext not found\n");
            exit_code = 1;
            goto finish;
        }

        CFArrayAppendValue(namedKexts, thisKext);
    }

    /* If no architectures are specified on the command line,
     * create one kext file that contains everything.
     * If one architecture is specified, create one mkext file
     * that is thinned to the specified architecture.
     * If multiple architectures are specified,
     * create a fat file containing multiple thin mkext files.
     */

    if (nArchs == 0) {
        archs[nArchs++] = &archAny;
    }

    if (kernelCacheFilename)
    {
	KXKextManagerError err;
	const NXArchInfo * hostArch;
	
	hostArch = NXGetLocalArchInfo();
	if (!hostArch) {
	    fprintf(stderr, "can't determine the host architecture\n");
	    exit_code = 1;
	    // still create mkext if requested
	    goto domkext;
	}

	if((nArchs > 1) ||
	  ((archs[0]->cputype != CPU_TYPE_ANY) && (archs[0]->cputype != hostArch->cputype))) {
	    fprintf(stderr, "can't create fat cache\n");
	    exit_code = 1;
	    // still create mkext if requested
	    goto domkext;
	}

        CFDictionaryRemoveAllValues(checkDictionary);
        collectKextsForMkextCache(namedKexts, checkDictionary,
                                  hostArch->cputype, hostArch->cpusubtype,
                                  local_for_all, network_for_all, safeboot_for_all,
				  kernel_requests,
                                  verbose_level, do_tests);
        collectKextsForMkextCache(repositoryKexts, checkDictionary,
                                  hostArch->cputype, hostArch->cpusubtype,
                                  local_for_repositories || local_for_all,
                                  network_for_repositories || network_for_all,
                                  safeboot_for_repositories || safeboot_for_all,
				  kernel_requests,
                                  verbose_level, do_tests);

	all_plists = false;
	if (!include_kernel_requests) {
	    // this cache isn't tied to a config
	    platform_name_root_path.root_path[0] = platform_name_root_path.platform_name[0] = 0;
	    all_plists = !(    local_for_repositories || local_for_all
			    || network_for_repositories || network_for_all
			    || safeboot_for_repositories || safeboot_for_all);
	}
	err = PreLink(theKextManager, checkDictionary, kernelCacheFilename, cacheFileTimes, kernel_file,
			platform_name_root_path.platform_name, platform_name_root_path.root_path,
			kernel_requests, all_plists,
			hostArch, // archs[0],
			verbose_level, debug_mode);

	if (kKXKextManagerErrorNone != err)
	{
	    exit_code = 1;
	    // still create mkext if requested
	    goto domkext;
	}
    }

domkext:
    if (!mkextFilename) {
        goto finish;
    }

    temp_dir = getenv("TMPDIR");
    if (!temp_dir)
        temp_dir = "/tmp";
    temp_file = malloc(strlen(temp_dir) + strlen(TEMP_DIR) + 1);
    strcpy(temp_file, temp_dir);
    strcat(temp_file, TEMP_DIR);
    mktemp(temp_file);

    fd = open(temp_file, O_WRONLY|O_CREAT|O_TRUNC, 0666);
    if (-1 == fd) {
        fprintf(stderr, "can't create %s - %s\n", temp_file,
            strerror(errno));
        result = false;
        goto finish;  // FIXME: mkextcache used to exit with EX_CANTCREAT
    }
    output_filename = temp_file;

    struct fat_header fatHeader;
    struct fat_arch fatArchs[kMaxArchs];
    unsigned long fat_offset = 0;
    ssize_t bytes_written = 0;

    if (nArchs > 1) {
        fat_offset = sizeof(struct fat_header) + (sizeof(struct fat_arch) * nArchs);
        lseek(fd, fat_offset, SEEK_SET);
    }

    for (i=0; i<nArchs; i++) {
        fatArchs[i].cputype = NXSwapHostLongToBig(archs[i]->cputype);
        fatArchs[i].cpusubtype = NXSwapHostLongToBig(archs[i]->cpusubtype);
        fatArchs[i].offset = NXSwapHostLongToBig(fat_offset);

        if (verbose_level >= 1) {
            verbose_log("processing architecture %s", archs[i]->name);
        }
        CFDictionaryRemoveAllValues(checkDictionary);
        collectKextsForMkextCache(namedKexts, checkDictionary,
                                  archs[i]->cputype, archs[i]->cpusubtype,
                                  local_for_all, network_for_all, safeboot_for_all,
				  NULL /*kernel_requests*/,
                                  verbose_level, do_tests);
        collectKextsForMkextCache(repositoryKexts, checkDictionary,
                                  archs[i]->cputype, archs[i]->cpusubtype,
                                  local_for_repositories || local_for_all,
                                  network_for_repositories || network_for_all,
                                  safeboot_for_repositories || safeboot_for_all,
				  NULL /*kernel_requests*/,
                                  verbose_level, do_tests);

        bytes_written = createMkextArchive(fd, checkDictionary, output_filename,
                                archs[i]->name, archs[i]->cputype, archs[i]->cpusubtype, verbose_level);

        if (bytes_written < 0) {
            exit_code = 1;
            goto finish;
        }
        fat_offset += bytes_written;

        if (checkMkextArchiveSize(bytes_written) == false) {
                fprintf(stderr, "archive would be too large; aborting\n");
                exit_code = 1;
                goto finish;
        }

        fatArchs[i].size = NXSwapHostLongToBig(bytes_written);
        fatArchs[i].align = NXSwapHostLongToBig(0);
    }

    if (nArchs > 1) {
        lseek(fd, 0, SEEK_SET);
        fatHeader.magic = NXSwapHostLongToBig(FAT_MAGIC);
        fatHeader.nfat_arch = NXSwapHostLongToBig(nArchs);
        bytes_written = write(fd, &fatHeader, sizeof(fatHeader));
        if (bytes_written != sizeof(fatHeader)) {
            perror("write");
            exit_code = 1;
            goto finish;
        }

        for (i=0; i<nArchs; i++) {
            bytes_written = write(fd, &fatArchs[i], sizeof(fatArchs[i]));
            if (bytes_written != sizeof(fatArchs[i])) {
                perror("write");
                exit_code = 1;
                goto finish;
            }
        }
    }

    close(fd);
    fd = -1;

    if (have_extensions_time)
    {
	struct timespec mod_time = extensions_stat_buf.st_mtimespec;
	if ((0 == stat(source_extensions, &extensions_stat_buf))
	  && ((mod_time.tv_sec != extensions_stat_buf.st_mtimespec.tv_sec)
	      || (mod_time.tv_nsec != extensions_stat_buf.st_mtimespec.tv_nsec)))
	{
	    fprintf(stderr, "cache stale - not creating %s\n", mkextFilename);
	    exit_code = 1;
	    goto finish;
	}

	TIMESPEC_TO_TIMEVAL(&cacheFileTimes[0], &extensions_stat_buf.st_atimespec);
	TIMESPEC_TO_TIMEVAL(&cacheFileTimes[1], &extensions_stat_buf.st_mtimespec);
	cacheFileTimes[1].tv_sec++;
    }

    // move it to the final destination
    if (-1 == rename(output_filename, mkextFilename)) {
	fprintf(stderr, "can't create file %s: %s\n", mkextFilename,
		strerror(errno));
	exit_code = 1;
	goto finish;
    }
    output_filename = NULL;

    // give the cache file the mod time of the source files when kextcache started
    if (cacheFileTimes && (-1 == utimes(mkextFilename, cacheFileTimes))) {
	fprintf(stderr, "can't set file times %s: %s\n", mkextFilename,
		strerror(errno));
	exit_code = 1;
	goto finish;
    }

finish:

   /*****
    * Clean everything up.
    */

    if (-1 != fd) close(fd);
    if (output_filename)
    {
	if (-1 == unlink(output_filename)) {
	    fprintf(stderr, "can't remove file %s - %s\n", output_filename,
			strerror(errno));
	}
    }
    if (temp_file)             free(temp_file);
    if (repositoryDirectories) CFRelease(repositoryDirectories);
    if (kextNames)             CFRelease(kextNames);
    if (kextNamesToUse)        CFRelease(kextNamesToUse);
    if (namedKexts)            CFRelease(namedKexts);
    if (repositoryKexts)       CFRelease(repositoryKexts);
    if (checkDictionary)       CFRelease(checkDictionary);

    if (theKextManager)        CFRelease(theKextManager);
    putVolumeForPath(mkextFilename, exit_code);	    // handles not locked

    exit(exit_code);
    return exit_code;
}

/*******************************************************************************
* takeVolume takes a dev_t to lock with kextd
*******************************************************************************/
// upstat() stat()s up the parental chain if a file doesn't exist
static int upstat(const char *path, struct stat *sb)
{
    int rval;
    const char *tpath = path;

    while ((rval = stat(tpath, sb)) != 0 && errno == ENOENT) {
	// "." and "/" should always exist, but you never know
	if (tpath[0] == '.' && tpath[1] == '\0')  goto finish;
	if (tpath[0] == '/' && tpath[1] == '\0')  goto finish;
	tpath = dirname(tpath);	    // our dirname() takes a const char*
    }

finish:
    if (rval)
	warn("couldn't find volume for %s", path);

    return rval;
}

// takeVolumeForPath used by forms other than '-u' (e.g. mkext)
static int takeVolumeForPath(const char *path)
{
    int rval = ELAST + 1;
    struct stat sb;

    rval = upstat(path, &sb);
    if (rval)  goto finish;
    rval = takeVolume(sb.st_dev);

finish:
    return rval;
}

// takeVolumeForPaths ensures all paths are on the given volume, then locks
int takeVolumeForPaths(char *volPath, int filec, const char *files[])
{
    int rval, bsderr, i;
    struct stat volsb;

    bsderr = stat(volPath, &volsb);
    if (bsderr)  goto finish;

    for (i = 0; i < filec; i++) {
	struct stat sb;

	rval = upstat(files[i], &sb);
	if (rval)  goto finish;

	// better be on the same device as the volume
	if (sb.st_dev != volsb.st_dev) {
	    warnx("can't lock: %s, %s on different volumes", volPath, files[i]);
	    goto finish;
	}
    }

    rval = takeVolume(volsb.st_dev);

finish:
    if (bsderr) {
	warn("couldn't lock paths on volume %s", volPath);
	rval = bsderr;
    } 

    return rval;
}

// can return success if a lock isn't needed
// can return failure if sLockPort is already in use
static int takeVolume(dev_t devid)
{
    int rval = ELAST + 1;
    int lckstatus, nretries = LCK_MAXTRIES;
    Boolean createdPort = false;
    dev_path_t voldev = "<unknown>";
    kern_return_t macherr = KERN_SUCCESS;
    mach_port_t tport = MACH_PORT_NULL;

    if (sLockPort)  goto finish;    // only support one lock at a time :)

    if (getuid() != 0) {
	// kextd shouldn't be watching anything you can touch
	// and ignores locking requests from non-root anyway
	rval = 0;
	goto finish;
    }

    tport = mach_task_self();
    if (tport == MACH_PORT_NULL)  goto finish;

    // look up kextd if not cached
    if (!sKextdPort) {
	mach_port_t bsport;
	macherr = task_get_bootstrap_port(tport, &bsport);
	if (macherr)  goto finish;
	macherr = bootstrap_look_up(bsport, KEXTD_SERVER_NAME, &sKextdPort);
	if (macherr)  goto finish;
    }

    // convert dev_t into diskXsY
    if(!devname_r(devid, S_IFBLK, voldev, DEVMAXPATHSIZE))  goto finish;

    // allocate a port to pass (in case we die -- it's released on exit() :)
    macherr = mach_port_allocate(tport,MACH_PORT_RIGHT_RECEIVE,&sLockPort);
    createdPort = true;
    if (macherr)  goto finish;
    // take the lock in a retry/delay loop in case the volume is busy
    do {
	macherr = kextmanager_lock_volume(sKextdPort, sLockPort, voldev,&lckstatus);
	if (macherr)  goto finish;

	if (lckstatus == EBUSY) {
	    warnx("%s locked; sleeping (%d retries left)", voldev, --nretries);
	    sleep(LCK_DELAY);
	}
    } while (lckstatus == EBUSY && nretries > 0);

    rval = lckstatus;


finish:
    // if kextd isn't competing with us, then we didn't need the lock
    if (lckstatus == ENOENT || macherr == BOOTSTRAP_UNKNOWN_SERVICE) {
	mach_port_mod_refs(tport, sLockPort, MACH_PORT_RIGHT_RECEIVE, -1);
	sLockPort = nil;
	rval = 0;
    } else if (macherr != KERN_SUCCESS) {
	if (macherr <= KERN_RETURN_MAX) {
	    warnx("couldn't lock %s: %s",voldev,mach_error_string(macherr));
	} else {
	    warnx("couldn't lock %s: error %d", voldev, macherr);
	}
	rval = macherr;
    } else {
	if (rval) {
	    warnx("couldn't lock %s: %s", voldev, strerror(rval));
	}
    }

    if (rval && createdPort) {
	mach_port_mod_refs(tport, sLockPort, MACH_PORT_RIGHT_RECEIVE, -1);
	sLockPort = nil;
    }

    return rval;
}


/*******************************************************************************
* putVolumeForPath will unlock the relevant volume, passing 'status' to
* inform kextd whether of our potential success
*******************************************************************************/
int putVolumeForPath(const char *path, int status)
{
    int rval = KERN_SUCCESS;
    struct stat sb;
    dev_path_t voldev;

    // if not locked, don't sweat it
    if (!sLockPort) {
	rval = 0;
	goto finish;
    }

    rval = upstat(path, &sb);
    if (rval)  goto finish;
    // get "diskXsY"
    if(!devname_r(sb.st_dev, S_IFBLK, voldev, DEVMAXPATHSIZE))  goto finish;

    rval = kextmanager_unlock_volume(sKextdPort, sLockPort, voldev, status);

    // the server will clean us up (with an error logged) if we couldn't
    mach_port_mod_refs(mach_task_self(), sLockPort, MACH_PORT_RIGHT_RECEIVE,-1);
    sLockPort = nil;

finish:
    if (rval) {
	if (rval == -1) {
	    warn("trouble unlocking volume for %s", path);
	} else if (rval <= KERN_RETURN_MAX) {
	    warnx("couldn't unlock volume for %s: %s",
		path, mach_error_string(rval));
	} else {
	    warnx("couldn't unlock volume for %s: error %d", path, rval);
	}
    }

    return rval;
}

/*******************************************************************************
*
*******************************************************************************/

static void get_catalog_demand_lists(CFMutableSetRef * kernel_requests,
				      CFSetRef * kernel_cache_misses,
				      int verbose_level)
{
    kern_return_t kr;
    char * propertiesBuffer;
    int    loaded_bytecount;

    kr = IOCatalogueGetData(MACH_PORT_NULL, kIOCatalogGetModuleDemandList,
			    &propertiesBuffer, &loaded_bytecount);
    if (kIOReturnSuccess == kr)
    { 
	CFSetRef set;
	set = (CFSetRef)
		IOCFUnserialize(propertiesBuffer, kCFAllocatorDefault, 0, 0);
	vm_deallocate(mach_task_self(), (vm_address_t) propertiesBuffer, loaded_bytecount);
	*kernel_requests = CFSetCreateMutableCopy(kCFAllocatorDefault, 0, set);
	CFRelease(set);
    }

    kr = IOCatalogueGetData(MACH_PORT_NULL, kIOCatalogGetCacheMissList,
			    &propertiesBuffer, &loaded_bytecount);
    if (kIOReturnSuccess == kr)
    { 
	if (verbose_level > 0)
	    verbose_log("cache misses:\n%s", propertiesBuffer);
	*kernel_cache_misses = (CFSetRef)
		IOCFUnserialize(propertiesBuffer, kCFAllocatorDefault, 0, 0);
	vm_deallocate(mach_task_self(), (vm_address_t) propertiesBuffer, loaded_bytecount);
    }
}

/*******************************************************************************
*
*******************************************************************************/
char * CFURLCopyCString(CFURLRef anURL)
{
    char * string = NULL; // returned
    CFIndex bufferLength;
    CFURLRef absURL = NULL;        // must release
    CFStringRef urlString = NULL;  // must release
    Boolean error = false;

    absURL = CFURLCopyAbsoluteURL(anURL);
    if (!absURL) {
        goto finish;
    }

    urlString = CFURLCopyFileSystemPath(absURL, kCFURLPOSIXPathStyle);
    if (!urlString) {
        goto finish;
    }

    bufferLength = 1 + CFStringGetLength(urlString);

    string = (char *)malloc(bufferLength * sizeof(char));
    if (!string) {
        goto finish;
    }

    if (!CFStringGetCString(urlString, string, bufferLength,
           kCFStringEncodingMacRoman)) {

        error = true;
        goto finish;
    }

finish:
    if (error) {
        free(string);
        string = NULL;
    }
    if (absURL)    CFRelease(absURL);
    if (urlString) CFRelease(urlString);
    return string;
}


/*******************************************************************************
*
*******************************************************************************/
static void addKextForMkextCache(
    KXKextRef theKext,
    CFMutableDictionaryRef checkDictionary,
    cpu_type_t archCPU,
    cpu_subtype_t archSubtype,
    Boolean local,
    Boolean network,
    Boolean safeboot,
    CFSetRef kernel_requests,
    int verbose_level,
    Boolean do_tests)
{
    Boolean include_it = true;  // assume success
    Boolean do_children = true;

    CFStringRef kextName = NULL; // must release
    char kext_name[MAXPATHLEN] = "";

    CFBundleRef kextBundle = NULL;        // don't release
    CFStringRef requiredString = NULL;    // don't release

    CFURLRef executableURL = NULL;        // must release
    char * executable_path = NULL;  // must free

    int fd = -1;         // sentinel value for close() call at finish
    caddr_t machO = (caddr_t)-1; // sentinel value for munmap() call at finish
    off_t machOSize = 0;

    kextName = KXKextCopyAbsolutePath(theKext);
    if (!kextName) {
        fprintf(stderr, "memory allocation failure\n");
        exit(1);
    }

    if (!CFStringGetCString(kextName, kext_name,
        sizeof(kext_name) / sizeof(char), kCFStringEncodingMacRoman)) {

        fprintf(stderr, "string conversion failure\n");
        include_it = false;
        do_children = false;
        goto finish;
    }

    if (CFDictionaryGetValue(checkDictionary, kextName)) {
        include_it = false;
        do_children = true;
        goto finish;
    }

    if (!KXKextIsValid(theKext)) {
        fprintf(stderr, "kernel extension %s is not valid; "
             "skipping it and any plugins\n", kext_name);
        if (do_tests) {
            fprintf(stderr, "kext diagnostics:\n");
            KXKextPrintDiagnostics(theKext, stderr);
        }
        include_it = false;
        do_children = false;
        goto finish;
    }

    kextBundle = KXKextGetBundle(theKext);

    include_it = false;
    do_children = true;

    if (kernel_requests) {
	CFStringRef bundleIdentifier = CFBundleGetIdentifier(kextBundle);
	if (!bundleIdentifier) {
	    fprintf(stderr, "can't get identifier for kext\n");
	    do_children = false;
	    goto finish;
	}
	include_it = CFSetContainsValue(kernel_requests, bundleIdentifier);
    } else if (!local && !network && !safeboot)
	include_it = true;

    if (!include_it && (local || network || safeboot)) {
        requiredString = CFBundleGetValueForInfoDictionaryKey(kextBundle,
            CFSTR("OSBundleRequired"));
    
        if (!requiredString) {

            if (verbose_level >= 3) {
                verbose_log("skipping bundle %s; no OSBundleRequired key "
                    "(still checking plugins)\n",
                    kext_name);
            }
            include_it = false;
            do_children = true;
            goto finish;

        } else if (CFStringCompare(requiredString,
                CFSTR("Root"), 0) == kCFCompareEqualTo ||
            CFStringCompare(requiredString,
                CFSTR("Console"), 0) == kCFCompareEqualTo) {

            include_it = true;
            do_children = true;

        } else if (local &&
            CFStringCompare(requiredString, CFSTR("Local-Root"), 0) ==
                kCFCompareEqualTo) {

            include_it = true;
            do_children = true;

        } else if (network &&
            CFStringCompare(requiredString, CFSTR("Network-Root"), 0) ==
                kCFCompareEqualTo) {

            include_it = true;
            do_children = true;

        } else if (safeboot &&
            CFStringCompare(requiredString, CFSTR("Safe Boot"), 0) ==
                kCFCompareEqualTo) {

            include_it = true;
            do_children = true;

        } else {
            char required_string[120];

           // skip for any other value

           if (!CFStringGetCString(requiredString, required_string,
               sizeof(required_string) / sizeof(char),
               kCFStringEncodingASCII)) {

               strcpy(required_string, "(unknown)");
            }

            if (verbose_level >= 3) {
                verbose_log(
                    "skipping bundle %s; OSBundleRequired key is \"%s\" "
                    "(still checking plugins)\n",
                    kext_name, required_string);
            }
            include_it = false;
            do_children = true;
            goto finish;
        }
    }

    if (!KXKextHasBeenAuthenticated(theKext)) {
        KXKextAuthenticate(theKext);
    }

    if (!KXKextIsAuthentic(theKext)) {
        fprintf(stderr, "kernel extension %s is not authentic (check ownership and permissions); "
             "skipping it and any plugins\n", kext_name);
        if (do_tests) {
            fprintf(stderr, "kext diagnostics:\n");
            KXKextPrintDiagnostics(theKext, stderr);
        }
        include_it = false;
        do_children = false;
        goto finish;
    }

    KXKextResolveDependencies(theKext);
    if (!KXKextGetHasAllDependencies(theKext)) {
        fprintf(stderr, "warning: kernel extension %s is missing dependencies "
            "(including in cache anyway; "
            "dependencies may be available from elsewhere)\n",
            kext_name);
        if (do_tests) {
            fprintf(stderr, "kext diagnostics:\n");
            KXKextPrintDiagnostics(theKext, stderr);
        }
        // include it anyway
	include_it = true;
	do_children = true;
    }

    if (KXKextGetDeclaresExecutable(theKext)) {
        executableURL = CFBundleCopyExecutableURL(kextBundle);
        if (!executableURL) {
            fprintf(stderr, "skipping bundle %s; "
                " declares an exectuable but has none "
                "(still checking plugins)\n",
                kext_name);
            include_it = false;
            do_children = true;
            goto finish;
        } else {

            executable_path = CFURLCopyCString(executableURL);
            if (!executable_path) {
                fprintf(stderr, "memory allocation failure\n");
                exit(1);
            } else {
                struct stat stat_buf;
                off_t archSize;
    
                fd = open(executable_path, O_RDONLY, 0);
                if (-1 == fd) { // Can't read executable
                    fprintf(stderr, "can't open file %s; "
                        "skipping the kext that contains it\n",
                        executable_path);
                    include_it = false;
                    do_children = true;
                    goto finish;
                }

                if (fstat(fd, &stat_buf) < 0) {
                    fprintf(stderr, "can't get size of file %s; "
                        "skipping the kext that contains it\n",
                        executable_path);
                    include_it = false;
                    do_children = true;
                    goto finish;
                }
                machOSize = stat_buf.st_size;

                machO = mmap(NULL, machOSize,
                    PROT_READ|PROT_WRITE, MAP_FILE|MAP_PRIVATE,
                    fd, 0 /* offset */);
                if (machO == (caddr_t)-1) {
                    fprintf(stderr, "can't map file %s; "
                        "skipping the kext that contains it\n",
                        executable_path);
                    include_it = false;
                    do_children = true;
                    goto finish;
                }
  
                find_arch(NULL, &archSize, archCPU, archSubtype,
                   (u_int8_t *)machO, machOSize);
                if (!archSize) { // Couldn't find an architecture
                    fprintf(stderr, "%s doesn't contain code for the "
                        "architecture specified; "
                        "skipping the kext that contains it\n",
                        executable_path);
                    include_it = false;
                    do_children = true;
                    goto finish;
                }
            }
        }
    }

finish:

    if (machO != (caddr_t)-1) {
        munmap(machO, machOSize);
    }

    if (fd != -1) {
        close(fd);
    }

    if (include_it && !CFDictionaryGetValue(checkDictionary, kextName)) {
        CFDictionarySetValue(checkDictionary, kextName, theKext);
    }

    if (do_children) {
        collectKextsForMkextCache(KXKextGetPlugins(theKext),
            checkDictionary, archCPU, archSubtype, local, network, safeboot, kernel_requests,
            verbose_level, do_tests);
    }

    if (kextName)        CFRelease(kextName);
    if (executableURL)   CFRelease(executableURL);
    if (executable_path) free(executable_path);

    return;
}

/*******************************************************************************
*
*******************************************************************************/
static void collectKextsForMkextCache(
    CFArrayRef kexts,
    CFMutableDictionaryRef checkDictionary,
    cpu_type_t archCPU,
    cpu_subtype_t archSubtype,
    Boolean local,
    Boolean network,
    Boolean safeboot,
    CFSetRef kernel_requests,
    int verbose_level,
    Boolean do_tests)
{
    CFIndex count, i;
    KXKextRef theKext = NULL; // don't release
    if (!kexts) {
        goto finish;
    }

   /*****
    * Get busy processing kexts.
    */
    count = CFArrayGetCount(kexts);
    for (i = 0; i < count; i++) {
        theKext = (KXKextRef)CFArrayGetValueAtIndex(kexts, i);

        addKextForMkextCache(theKext, checkDictionary, archCPU, archSubtype,
            local, network, safeboot, kernel_requests, verbose_level, do_tests);
    }

finish:

    return;
}

/*******************************************************************************
* verbose_log()
*
* Print a log message prefixed with the name of the program.
*******************************************************************************/

__private_extern__ void verbose_log(const char * format, ...)
{
    va_list ap;
    char fake_buffer[2];
    int output_length;
    char * output_string;

    va_start(ap, format);
    output_length = vsnprintf(fake_buffer, 1, format, ap);
    va_end(ap);

    output_string = (char *)malloc(output_length + 1);
    if (!output_string) {
        fprintf(stderr, "memory allocation failure\n");
        return;
    }

    va_start(ap, format);
    vsprintf(output_string, format, ap);
    va_end(ap);

    va_start(ap, format);
    fprintf(stdout, "%s: %s\n", progname, output_string);
    va_end(ap);

    free(output_string);

    return;
}

/*******************************************************************************
* error_log()
*
* Print an error message prefixed with the name of the program.
*******************************************************************************/
static void error_log(const char * format, ...)
{
    va_list ap;
    char fake_buffer[2];
    int output_length;
    char * output_string;

    va_start(ap, format);
    output_length = vsnprintf(fake_buffer, 1, format, ap);
    va_end(ap);

    output_string = (char *)malloc(output_length + 1);
    if (!output_string) {
        fprintf(stderr, "memory allocation failure\n");
        return;
    }

    va_start(ap, format);
    vsprintf(output_string, format, ap);
    va_end(ap);

    va_start(ap, format);
    fprintf(stderr, "%s: %s\n", progname, output_string);
    va_end(ap);

    free(output_string);

    return;
}

/*******************************************************************************
* user_approve()
*
* Ask the user a question and wait for a yes/no answer.
*******************************************************************************/
static int user_approve(int default_answer, const char * format, ...)
{
    int result = 1;
    va_list ap;
    char fake_buffer[2];
    int output_length;
    char * output_string;
    char * prompt_string = NULL;
    int c, x;

    va_start(ap, format);
    output_length = vsnprintf(fake_buffer, 1, format, ap);
    va_end(ap);

    output_string = (char *)malloc(output_length + 1);
    if (!output_string) {
        fprintf(stderr, "memory allocation failure\n");
        result = -1;
        goto finish;
    }

    va_start(ap, format);
    vsprintf(output_string, format, ap);
    va_end(ap);

    prompt_string = default_answer ? " [Y/n]" : " [y/N]";
    
    while ( 1 ) {
        fprintf(stdout, "%s%s%s", output_string, prompt_string, "? ");
        fflush(stdout);

        c = fgetc(stdin);

        if (c == EOF) {
            result = -1;
            goto finish;
        }

       /* Make sure we get a newline.
        */
        if ( c != '\n' ) {
            do {
                x = fgetc(stdin);
            } while (x != '\n' && x != EOF);

            if (x == EOF) {
                result = -1;
                goto finish;
            }
        }

        if (c == '\n') {
            result = default_answer ? 1 : 0;
            goto finish;
        } else if (tolower(c) == 'y') {
            result = 1;
            goto finish;
        } else if (tolower(c) == 'n') {
            result = 0;
            goto finish;
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
static const char * user_input(const char * format, ...)
{
    char * result = NULL;  // return value
    va_list ap;
    char fake_buffer[2];
    int output_length;
    char * output_string = NULL;
    unsigned index;
    size_t size = 80;
    int c;

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
        fprintf(stderr, "memory allocation failure\n");
        result = NULL;
        goto finish;
    }

    va_start(ap, format);
    vsprintf(output_string, format, ap);
    va_end(ap);

    fprintf(stdout, "%s ", output_string);
    fflush(stdout);

    c = fgetc(stdin);
    while (c != '\n' && c != EOF) {
        if (index >= size) {
            size += 80;
            result = realloc(result, size);
            if (!result) {
                goto finish;
            }
        }
        result[index++] = (char)c;
        c = fgetc(stdin);
    }
    if (c == EOF) {
        if (result) free(result);
        result = NULL;
        goto finish;
    }

finish:
    if (output_string) free(output_string);

    return result;
}

/*******************************************************************************
* addKextsToManager()
*
* Add the kexts named in the kextNames array to the given kext manager, and
* put their names into the kextNamesToUse.
*******************************************************************************/
static Boolean addKextsToManager(
    KXKextManagerRef aManager,
    CFArrayRef kextNames,
    CFMutableArrayRef kextNamesToUse,
    Boolean do_tests)
{
    Boolean result = true;     // assume success
    KXKextManagerError kxresult = kKXKextManagerErrorNone;
    CFIndex i, count;
    KXKextRef theKext = NULL;  // don't release
    CFURLRef kextURL = NULL;   // must release

   /*****
    * Add each kext named to the manager.
    */
    count = CFArrayGetCount(kextNames);
    for (i = 0; i < count; i++) {
        char name_buffer[MAXPATHLEN];

        CFStringRef kextName = (CFStringRef)CFArrayGetValueAtIndex(
            kextNames, i);

        if (kextURL) {
            CFRelease(kextURL);
            kextURL = NULL;
        }

        kextURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
            kextName, kCFURLPOSIXPathStyle, true);
        if (!kextURL) {
            fprintf(stderr, "memory allocation failure\n");
            result = false;
            goto finish;
        }

        if (!CFStringGetCString(kextName,
            name_buffer, sizeof(name_buffer) - 1, kCFStringEncodingMacRoman)) {

            fprintf(stderr, "memory allocation or string conversion error\n");
            result = false;
            goto finish;
        }

        kxresult = KXKextManagerAddKextWithURL(aManager, kextURL, true, &theKext);
        if (kxresult != kKXKextManagerErrorNone) {
            fprintf(stderr, "can't add kernel extension %s (%s)",
                name_buffer, KXKextManagerErrorStaticCStringForError(kxresult));
#if 0
            if (do_tests && theKext) {
                fprintf(stderr, "kernel extension problems:\n");
                KXKextPrintDiagnostics(theKext, stderr);
            }
            continue;
#endif 0
        }
        if (kextNamesToUse && theKext &&
            (kxresult == kKXKextManagerErrorNone || do_tests)) {

            CFArrayAppendValue(kextNamesToUse, kextName);
        }
    }

finish:
    if (kextURL) CFRelease(kextURL);
    return result;
}

/*******************************************************************************
* usage()
*******************************************************************************/
static void usage(int level)
{
    fprintf(stderr,
      "usage: %s [-a arch] [-c kernel_cache_filename] [-e] [-F] [-h] [-k]\n"
      "       [-K kernel_filename] [-l | -L] [-r] [-m mkext_filename] [-n | -N]"
      "\n"
      "       [-r] [-s | -S] [-t] [-v [1-6]] [-z] [kext_or_directory] ...\n"
      "\n",
      progname);
    fprintf(stderr, "       %s [-f] -u volume\n\n", progname);

    if (level < 1) {
        return;
    }

    if (level == 1) {
        fprintf(stderr, "use %s -h for an explanation of each option\n",
            progname);
        return;
    }

    fprintf(stderr,
        "  kext_or_directory: Add the kext or all kexts in directory to cache\n"
        "    (required unless using -e)\n");
    fprintf(stderr,
        "  -a arch: Add only kexts that contain code for arch\n");
    fprintf(stderr,
        "  -c kernel_cache_filename: Create an kernel prelink cache\n");
    fprintf(stderr,
        "  -e: Create or update the mkext cache for /System/Library/Extensions\n");
    fprintf(stderr,
        "  -F: Fork and exit immediately (to work in background)\n");
    fprintf(stderr,
        "  -h: This usage statement\n");
    fprintf(stderr,
        "  -k: Create or update any repository .kextcache files\n");
    fprintf(stderr,
        "  -K kernel_filename: Name of kernel file for kernel cache\n");
    fprintf(stderr,
        "  -l: Add local-root kexts in directories to an mkext cache\n");
    fprintf(stderr,
        "  -L: Add local-root kexts for all args to an mkext cache\n");
    fprintf(stderr,
        "  -m mkext_filename: Create an mkext archive\n");
    fprintf(stderr,
        "  -n: Add network-root kexts in directories to an mkext cache\n");
    fprintf(stderr,
        "  -N: Add network-root kexts for all args to an mkext cache\n");
    fprintf(stderr,
        "  -r: Add kexts previously loaded by this machine\n");
    fprintf(stderr,
        "  -s: Add safe boot kexts in directories to an mkext cache\n");
    fprintf(stderr,
        "  -S: Add safe boot kexts for all args to an mkext cache\n");
    fprintf(stderr,
        "  -t: Perform diagnostic tests on kexts and print results\n");
    fprintf(stderr,
        "  -v: verbose mode; print info about caching process\n");
    fprintf(stderr,
        "  -z: don't authenticate kexts (for use during development)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "At least one of -k, -c or -m must be specified.\n");
    fprintf(stderr,
        "-l/-L and -n/-N may both be specified to make a cache of local-\n"
        "and network-root kexts\n");

    return;
}



