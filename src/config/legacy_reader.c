/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Config routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "legacy_reader.h"
#include "config.h"
#include "setupVars.h"
#include "log.h"
// nice()
#include <unistd.h>
// argv_dnsmasq
#include "args.h"
// INT_MAX
#include <limits.h>

// Private global variables
static char *conflinebuffer = NULL;
static size_t size = 0;
static pthread_mutex_t lock;

// Private prototypes
static char *parseFTLconf(FILE *fp, const char *key);
static void releaseConfigMemory(void);
static char *getPath(FILE* fp, const char *option, char *ptr);
static bool parseBool(const char *option, bool *ptr);
static void readDebugingSettingsLegacy(FILE *fp);
static void getBlockingModeLegacy(FILE *fp);
static void getPrivacyLevelLegacy(FILE *fp);

static FILE * __attribute__((nonnull(1), malloc, warn_unused_result)) openFTLconf(const char **path)
{
	FILE *fp;
	// First check if there is a local file overwriting the global one
	*path = "pihole-FTL.conf";
	if((fp = fopen(*path, "r")) != NULL)
		return fp;

	// Local file not present, try system file
	*path = "/etc/pihole/pihole-FTL.conf";
	fp = fopen(*path, "r");

	return fp;
}

bool getLogFilePathLegacy(FILE *fp)
{
	const char *path = NULL;
	if(fp == NULL)
		fp = openFTLconf(&path);
	if(fp == NULL)
		return false;

	// Read LOGFILE value if available
	// defaults to: "/var/log/pihole/FTL.log"
	char *buffer = parseFTLconf(fp, "LOGFILE");

	errno = 0;
	// No option set => use default log location
	if(buffer == NULL)
	{
		// Use standard path if no custom path was obtained from the config file
		config.files.log.v.s = strdup("/var/log/pihole/FTL.log");

		// Test if memory allocation was successful
		if(config.files.log.v.s == NULL)
		{
			printf("FATAL: Allocating memory for config.files.log.v.s failed (%s, %i). Exiting.",
			       strerror(errno), errno);
			exit(EXIT_FAILURE);
		}
	}
	// Use sscanf() to obtain filename from config file parameter only if buffer != NULL
	else if(sscanf(buffer, "%127ms", &config.files.log.v.s) == 0)
	{
		// Empty file string
		config.files.log.v.s = NULL;
		log_info("Using syslog facility");
	}

	fclose(fp);
	return true;
}

// Returns which file was read
const char *readFTLlegacy(void)
{
	char *buffer;
	const char *path = NULL;
	FILE *fp = openFTLconf(&path);
	if(fp == NULL)
		return NULL;

	log_notice("Reading legacy config file");

	// AAAA_QUERY_ANALYSIS
	// defaults to: Yes
	buffer = parseFTLconf(fp, "AAAA_QUERY_ANALYSIS");
	parseBool(buffer, &config.dns.analyzeAAAA.v.b);

	// MAXDBDAYS
	// defaults to: 365 days
	buffer = parseFTLconf(fp, "MAXDBDAYS");

	int value = 0;
	const int maxdbdays_max = INT_MAX / 24 / 60 / 60;
	if(buffer != NULL && sscanf(buffer, "%i", &value))
	{
		// Prevent possible overflow
		if(value > maxdbdays_max)
			value = maxdbdays_max;

		// Only use valid values
		if(value == -1 || value >= 0)
			config.database.maxDBdays.v.i = value;
	}

	// RESOLVE_IPV6
	// defaults to: Yes
	buffer = parseFTLconf(fp, "RESOLVE_IPV6");
	parseBool(buffer, &config.resolver.resolveIPv6.v.b);

	// RESOLVE_IPV4
	// defaults to: Yes
	buffer = parseFTLconf(fp, "RESOLVE_IPV4");
	parseBool(buffer, &config.resolver.resolveIPv4.v.b);

	// DBINTERVAL
	// How often do we store queries in FTL's database [minutes]?
	// this value can be a floating point number, e.g. "DBINTERVAL=0.5"
	// defaults to: once per minute
	buffer = parseFTLconf(fp, "DBINTERVAL");

	float fvalue = 0;
	if(buffer != NULL && sscanf(buffer, "%f", &fvalue))
		// check if the read value is
		// - larger than 0.1min (6sec), and
		// - smaller than 1440.0min (once a day)
		if(fvalue >= 0.1f && fvalue <= 1440.0f)
			config.database.DBinterval.v.ui = (int)(fvalue * 60);

	// DBFILE
	// defaults to: "/etc/pihole/pihole-FTL.db"
	buffer = parseFTLconf(fp, "DBFILE");

	// Use sscanf() to obtain filename from config file parameter only if buffer != NULL
	if(!(buffer != NULL && sscanf(buffer, "%127ms", &config.files.database.v.s)))
	{
		// Use standard path if no custom path was obtained from the config file
		config.files.database.v.s = config.files.database.d.s;
	}

	if(config.files.database.v.s == NULL || strlen(config.files.database.v.s) == 0)
	{
		// Use standard path if path was set to zero but override
		// MAXDBDAYS=0 to ensure no queries are stored in the database
		config.files.database.v.s = config.files.database.d.s;
		config.database.maxDBdays.v.i = 0;
	}

	// MAXLOGAGE
	// Up to how many hours in the past should queries be imported from the database?
	// defaults to: 24.0 via MAXLOGAGE defined in FTL.h
	buffer = parseFTLconf(fp, "MAXLOGAGE");

	fvalue = 0;
	if(buffer != NULL && sscanf(buffer, "%f", &fvalue))
	{
		if(fvalue >= 0.0f && fvalue <= 1.0f*MAXLOGAGE)
			config.database.maxHistory.v.ui = (int)(fvalue * 3600);
	}

	// PRIVACYLEVEL
	// Specify if we want to anonymize the DNS queries somehow, available options are:
	// PRIVACY_SHOW_ALL (0) = don't hide anything
	// PRIVACY_HIDE_DOMAINS (1) = show and store all domains as "hidden", return nothing for Top Domains + Top Ads
	// PRIVACY_HIDE_DOMAINS_CLIENTS (2) = as above, show all domains as "hidden" and all clients as "127.0.0.1"
	//                                    (or "::1"), return nothing for any Top Lists
	// PRIVACY_MAXIMUM (3) = Disabled basically everything except the anonymous statistics, there will be no entries
	//                       added to the database, no entries visible in the query log and no Top Item Lists
	// PRIVACY_NOSTATS (4) = Disable any analysis on queries. No counters are available in this mode.
	// defaults to: PRIVACY_SHOW_ALL
	getPrivacyLevelLegacy(fp);

	// ignoreLocalhost
	// defaults to: false
	buffer = parseFTLconf(fp, "IGNORE_LOCALHOST");
	parseBool(buffer, &config.dns.ignoreLocalhost.v.b);

	if(buffer != NULL && strcasecmp(buffer, "yes") == 0)
		config.dns.ignoreLocalhost.v.b = true;

	// BLOCKINGMODE
	// defaults to: MODE_IP
	getBlockingModeLegacy(fp);

	// ANALYZE_ONLY_A_AND_AAAA
	// defaults to: false
	buffer = parseFTLconf(fp, "ANALYZE_ONLY_A_AND_AAAA");
	parseBool(buffer, &config.dns.analyzeOnlyAandAAAA.v.b);

	if(buffer != NULL && strcasecmp(buffer, "true") == 0)
		config.dns.analyzeOnlyAandAAAA.v.b = true;

	// DBIMPORT
	// defaults to: Yes
	buffer = parseFTLconf(fp, "DBIMPORT");
	parseBool(buffer, &config.database.DBimport.v.b);

	// PIDFILE
	config.files.pid.v.s = getPath(fp, "PIDFILE", config.files.pid.v.s);

	// SETUPVARSFILE
	config.files.setupVars.v.s = getPath(fp, "SETUPVARSFILE", config.files.setupVars.v.s);

	// MACVENDORDB
	config.files.macvendor.v.s = getPath(fp, "MACVENDORDB", config.files.macvendor.v.s);

	// GRAVITYDB
	config.files.gravity.v.s = getPath(fp, "GRAVITYDB", config.files.gravity.v.s);

	// PARSE_ARP_CACHE
	// defaults to: true
	buffer = parseFTLconf(fp, "PARSE_ARP_CACHE");
	parseBool(buffer, &config.database.network.parseARPcache.v.b);

	// CNAME_DEEP_INSPECT
	// defaults to: true
	buffer = parseFTLconf(fp, "CNAME_DEEP_INSPECT");
	parseBool(buffer, &config.dns.CNAMEdeepInspect.v.b);

	// DELAY_STARTUP
	// defaults to: zero (seconds)
	buffer = parseFTLconf(fp, "DELAY_STARTUP");

	unsigned int unum;
	if(buffer != NULL && sscanf(buffer, "%u", &unum) && unum > 0 && unum <= 300)
		config.misc.delay_startup.v.ui = unum;

	// BLOCK_ESNI
	// defaults to: true
	buffer = parseFTLconf(fp, "BLOCK_ESNI");
	parseBool(buffer, &config.dns.blockESNI.v.b);

	// WEBROOT
	config.http.paths.webroot.v.s = getPath(fp, "WEBROOT", config.http.paths.webroot.v.s);

	// WEBPORT
	// On which port should FTL's API be listening?
	// defaults to: 8080
	buffer = parseFTLconf(fp, "WEBPORT");

	value = 0;
	if(buffer != NULL && strlen(buffer) > 0)
		config.http.port.v.s = strdup(buffer);

	// WEBHOME
	// From which sub-directory is the web interface served from?
	// Defaults to: /admin/ (both slashes are needed!)
	config.http.paths.webhome.v.s = getPath(fp, "WEBHOME", config.http.paths.webhome.v.s);

	// WEBACL
	// Default: allow all access
	// An Access Control List (ACL) allows restrictions to be
	// put on the list of IP addresses which have access to our
	// web server.
	// The ACL is a comma separated list of IP subnets, where
	// each subnet is pre-pended by either a - or a + sign.
	// A plus sign means allow, where a minus sign means deny.
	// If a subnet mask is omitted, such as -1.2.3.4, this means
	// to deny only that single IP address.
	// Subnet masks may vary from 0 to 32, inclusive. The default
	// setting is to allow all accesses. On each request the full
	// list is traversed, and the last match wins.
	//
	// Example 1: "-0.0.0.0/0,+127.0.0.1"
	//            ---> deny all accesses, except from localhost (IPv4)
	// Example 2: "-0.0.0.0/0,+192.168/16"
	//            ---> deny all accesses, except from the
	//                 192.168/16 subnet
	//
	buffer = parseFTLconf(fp, "WEBACL");
	if(buffer != NULL)
		config.http.acl.v.s = strdup(buffer);

	// API_AUTH_FOR_LOCALHOST
	// defaults to: true
	buffer = parseFTLconf(fp, "API_AUTH_FOR_LOCALHOST");
	parseBool(buffer, &config.http.localAPIauth.v.b);

	// API_SESSION_TIMEOUT
	// How long should a session be considered valid after login?
	// defaults to: 300 seconds
	buffer = parseFTLconf(fp, "API_SESSION_TIMEOUT");

	value = 0;
	if(buffer != NULL && sscanf(buffer, "%i", &value) && value > 0)
		config.http.sessionTimeout.v.ui = value;

	// API_PRETTY_JSON
	// defaults to: false
	buffer = parseFTLconf(fp, "API_PRETTY_JSON");
	parseBool(buffer, &config.http.prettyJSON.v.b);

	// API_ERROR_LOG
	config.files.ph7_error.v.s = getPath(fp, "API_ERROR_LOG", config.files.ph7_error.v.s);

	// API_INFO_LOG
	config.files.http_info.v.s = getPath(fp, "API_INFO_LOG", config.files.http_info.v.s);

	// NICE
	// Shall we change the nice of the current process?
	// defaults to: -10 (can be disabled by setting value to -999)
	//
	// The nice value is an attribute that can be used to influence the CPU
	// scheduler to favor or disfavor a process in scheduling decisions.
	//
	// The range of the nice value varies across UNIX systems. On modern Linux,
	// the range is -20 (high priority) to +19 (low priority). On some other
	// systems, the range is -20..20. Very early Linux kernels (Before Linux
	// 2.0) had the range -infinity..15.
	buffer = parseFTLconf(fp, "NICE");

	// MAXNETAGE
	// IP addresses (and associated host names) older than the specified number
	// of days are removed to avoid dead entries in the network overview table
	// defaults to: the same value as MAXDBDAYS
	buffer = parseFTLconf(fp, "MAXNETAGE");

	int ivalue = 0;
	if(buffer != NULL &&
	    sscanf(buffer, "%i", &ivalue) &&
	    ivalue > 0 && ivalue <= 8760) // 8760 days = 24 years
			config.database.network.expire.v.ui = ivalue;

	// NAMES_FROM_NETDB
	// Should we use the fallback option to try to obtain client names from
	// checking the network table? Assume this is an IPv6 client without a
	// host names itself but the network table tells us that this is the same
	// device where we have a host names for its IPv4 address. In this case,
	// we use the host name associated to the other address as this is the same
	// device. This behavior can be disabled using NAMES_FROM_NETDB=false
	// defaults to: true
	buffer = parseFTLconf(fp, "NAMES_FROM_NETDB");
	parseBool(buffer, &config.resolver.networkNames.v.b);

	// EDNS0_ECS
	// Should we overwrite the query source when client information is
	// provided through EDNS0 client subnet (ECS) information?
	// defaults to: true
	buffer = parseFTLconf(fp, "EDNS0_ECS");
	parseBool(buffer, &config.dns.EDNS0ECS.v.b);

	// REFRESH_HOSTNAMES
	// defaults to: IPV4
	buffer = parseFTLconf(fp, "REFRESH_HOSTNAMES");

	if(buffer != NULL && strcasecmp(buffer, "ALL") == 0)
		config.resolver.refreshNames.v.refresh_hostnames = REFRESH_ALL;
	else if(buffer != NULL && strcasecmp(buffer, "NONE") == 0)
		config.resolver.refreshNames.v.refresh_hostnames = REFRESH_NONE;
	else if(buffer != NULL && strcasecmp(buffer, "UNKNOWN") == 0)
		config.resolver.refreshNames.v.refresh_hostnames = REFRESH_UNKNOWN;
	else
		config.resolver.refreshNames.v.refresh_hostnames = REFRESH_IPV4_ONLY;

	// WEBDOMAIN
	config.http.domain.v.s = getPath(fp, "WEBDOMAIN", config.http.domain.v.s);

	// RATE_LIMIT
	// defaults to: 1000 queries / 60 seconds
	buffer = parseFTLconf(fp, "RATE_LIMIT");

	unsigned int count = 0, interval = 0;
	if(buffer != NULL && sscanf(buffer, "%u/%u", &count, &interval) == 2)
	{
		config.dns.rateLimit.count.v.ui = count;
		config.dns.rateLimit.interval.v.ui = interval;
	}

	// LOCAL_IPV4
	// Use a specific IP address instead of automatically detecting the
	// IPv4 interface address a query arrived on for A hostname queries
	// defaults to: not set
	config.dns.reply.host.overwrite_v4.v.b = false;
	config.dns.reply.host.v4.v.in_addr.s_addr = 0;
	buffer = parseFTLconf(fp, "LOCAL_IPV4");
	if(buffer != NULL && inet_pton(AF_INET, buffer, &config.dns.reply.host.v4.v.in_addr))
		config.dns.reply.host.overwrite_v4.v.b = true;

	// LOCAL_IPV6
	// Use a specific IP address instead of automatically detecting the
	// IPv6 interface address a query arrived on for AAAA hostname queries
	// defaults to: not set
	config.dns.reply.host.overwrite_v6.v.b = false;
	memset(&config.dns.reply.host.v6.v.in6_addr, 0, sizeof(config.dns.reply.host.v6.v.in6_addr));
	buffer = parseFTLconf(fp, "LOCAL_IPV6");
	if(buffer != NULL && inet_pton(AF_INET6, buffer, &config.dns.reply.host.v6.v.in6_addr))
		config.dns.reply.host.overwrite_v6.v.b = true;

	// BLOCK_IPV4
	// Use a specific IPv4 address for IP blocking mode replies
	// defaults to: REPLY_ADDR4 setting
	config.dns.reply.blocking.overwrite_v4.v.b = false;
	config.dns.reply.blocking.v4.v.in_addr.s_addr = 0;
	buffer = parseFTLconf(fp, "BLOCK_IPV4");
	if(buffer != NULL && inet_pton(AF_INET, buffer, &config.dns.reply.blocking.v4.v.in_addr))
		config.dns.reply.blocking.overwrite_v4.v.b = true;

	// BLOCK_IPV6
	// Use a specific IPv6 address for IP blocking mode replies
	// defaults to: REPLY_ADDR6 setting
	config.dns.reply.blocking.overwrite_v6.v.b = false;
	memset(&config.dns.reply.blocking.v6.v.in6_addr, 0, sizeof(config.dns.reply.host.v6.v.in6_addr));
	buffer = parseFTLconf(fp, "BLOCK_IPV6");
	if(buffer != NULL && inet_pton(AF_INET6, buffer, &config.dns.reply.blocking.v6.v.in6_addr))
		config.dns.reply.blocking.overwrite_v6.v.b = true;

	// REPLY_ADDR4 (deprecated setting)
	// Use a specific IP address instead of automatically detecting the
	// IPv4 interface address a query arrived on A hostname and IP blocked queries
	// defaults to: not set
	struct in_addr reply_addr4;
	buffer = parseFTLconf(fp, "REPLY_ADDR4");
	if(buffer != NULL && inet_pton(AF_INET, buffer, &reply_addr4))
	{
		if(config.dns.reply.host.overwrite_v4.v.b || config.dns.reply.blocking.overwrite_v4.v.b)
		{
			log_warn("Ignoring REPLY_ADDR4 as LOCAL_IPV4 or BLOCK_IPV4 has been specified.");
		}
		else
		{
			config.dns.reply.host.overwrite_v4.v.b = true;
			memcpy(&config.dns.reply.host.v4.v.in_addr, &reply_addr4, sizeof(reply_addr4));
			config.dns.reply.blocking.overwrite_v4.v.b = true;
			memcpy(&config.dns.reply.blocking.v4.v.in_addr, &reply_addr4, sizeof(reply_addr4));
		}
	}

	// REPLY_ADDR6 (deprecated setting)
	// Use a specific IP address instead of automatically detecting the
	// IPv4 interface address a query arrived on A hostname and IP blocked queries
	// defaults to: not set
	struct in6_addr reply_addr6;
	buffer = parseFTLconf(fp, "REPLY_ADDR6");
	if(buffer != NULL && inet_pton(AF_INET, buffer, &reply_addr6))
	{
		if(config.dns.reply.host.overwrite_v6.v.b || config.dns.reply.blocking.overwrite_v6.v.b)
		{
			log_warn("Ignoring REPLY_ADDR6 as LOCAL_IPV6 or BLOCK_IPV6 has been specified.");
		}
		else
		{
			config.dns.reply.host.overwrite_v6.v.b = true;
			memcpy(&config.dns.reply.host.v6.v.in6_addr, &reply_addr6, sizeof(reply_addr6));
			config.dns.reply.blocking.overwrite_v6.v.b = true;
			memcpy(&config.dns.reply.blocking.v6.v.in6_addr, &reply_addr6, sizeof(reply_addr6));
		}
	}

	// SHOW_DNSSEC
	// Should FTL analyze and include automatically generated DNSSEC queries in the Query Log?
	// defaults to: true
	buffer = parseFTLconf(fp, "SHOW_DNSSEC");
	parseBool(buffer, &config.dns.showDNSSEC.v.b);

	// MOZILLA_CANARY
	// Should FTL handle use-application-dns.net specifically and always return NXDOMAIN?
	// defaults to: true
	buffer = parseFTLconf(fp, "MOZILLA_CANARY");
	parseBool(buffer, &config.dns.specialDomains.mozillaCanary.v.b);

	// PIHOLE_PTR
	// Should FTL return "pi.hole" as name for PTR requests to local IP addresses?
	// defaults to: true
	buffer = parseFTLconf(fp, "PIHOLE_PTR");

	if(buffer != NULL)
	{
		if(strcasecmp(buffer, "none") == 0 ||
		   strcasecmp(buffer, "false") == 0)
			config.dns.piholePTR.v.ptr_type = PTR_NONE;
		else if(strcasecmp(buffer, "hostname") == 0)
			config.dns.piholePTR.v.ptr_type = PTR_HOSTNAME;
		else if(strcasecmp(buffer, "hostnamefqdn") == 0)
			config.dns.piholePTR.v.ptr_type = PTR_HOSTNAMEFQDN;
	}

	// ADDR2LINE
	// Should FTL try to call addr2line when generating backtraces?
	// defaults to: true
	buffer = parseFTLconf(fp, "ADDR2LINE");
	parseBool(buffer, &config.misc.addr2line.v.b);

	// REPLY_WHEN_BUSY
	// How should FTL handle queries when the gravity database is not available?
	// defaults to: BLOCK
	buffer = parseFTLconf(fp, "REPLY_WHEN_BUSY");

	if(buffer != NULL)
	{
		if(strcasecmp(buffer, "DROP") == 0)
			config.dns.replyWhenBusy.v.busy_reply = BUSY_DROP;
		else if(strcasecmp(buffer, "REFUSE") == 0)
			config.dns.replyWhenBusy.v.busy_reply = BUSY_REFUSE;
		else if(strcasecmp(buffer, "BLOCK") == 0)
			config.dns.replyWhenBusy.v.busy_reply = BUSY_BLOCK;
	}

	// BLOCK_TTL
	// defaults to: 2 seconds
	config.dns.blockTTL.v.ui = 2;
	buffer = parseFTLconf(fp, "BLOCK_TTL");

	unsigned int uval = 0;
	if(buffer != NULL && sscanf(buffer, "%u", &uval))
		config.dns.blockTTL.v.ui = uval;

	// BLOCK_ICLOUD_PR
	// Should FTL handle the iCloud privacy relay domains specifically and
	// always return NXDOMAIN??
	// defaults to: true
	buffer = parseFTLconf(fp, "BLOCK_ICLOUD_PR");
	parseBool(buffer, &config.dns.specialDomains.iCloudPrivateRelay.v.b);

	// CHECK_LOAD
	// Should FTL check the 15 min average of CPU load and complain if the
	// load is larger than the number of available CPU cores?
	// defaults to: true
	buffer = parseFTLconf(fp, "CHECK_LOAD");
	parseBool(buffer, &config.misc.check.load.v.b);

	// CHECK_SHMEM
	// Limit above which FTL should complain about a shared-memory shortage
	// defaults to: 90%
	config.misc.check.shmem.v.ui = 90;
	buffer = parseFTLconf(fp, "CHECK_SHMEM");

	if(buffer != NULL && sscanf(buffer, "%i", &ivalue) &&
	   ivalue >= 0 && ivalue <= 100)
		config.misc.check.shmem.v.ui = ivalue;

	// CHECK_DISK
	// Limit above which FTL should complain about disk shortage for checked files
	// defaults to: 90%
	config.misc.check.disk.v.b = 90;
	buffer = parseFTLconf(fp, "CHECK_DISK");

	if(buffer != NULL && sscanf(buffer, "%i", &ivalue) &&
	   ivalue >= 0 && ivalue <= 100)
			config.misc.check.disk.v.b = ivalue;

	// Read DEBUG_... setting from pihole-FTL.conf
	// This option should be the last one as it causes
	// some rather verbose output into the log when
	// listing all the enabled/disabled debugging options
	readDebugingSettingsLegacy(fp);

	// Release memory
	releaseConfigMemory();

	if(fp != NULL)
		fclose(fp);

	return path;
}

static char* getPath(FILE* fp, const char *option, char *ptr)
{
	// This subroutine is used to read paths from pihole-FTL.conf
	// fp:         File ptr to opened and readable config file
	// option:     Option string ("key") to try to read
	// ptr:        Location where read (or default) parameter is stored
	char *buffer = parseFTLconf(fp, option);

	errno = 0;
	// Use sscanf() to obtain filename from config file parameter only if buffer != NULL
	if(buffer == NULL || sscanf(buffer, "%127ms", &ptr) != 1)
	{
		// Use standard path if no custom path was obtained from the config file
		return ptr;
	}

	// Test if memory allocation was successful
	if(ptr == NULL)
	{
		log_crit("Allocating memory for %s failed (%s, %i). Exiting.", option, strerror(errno), errno);
		exit(EXIT_FAILURE);
	}
	else if(strlen(ptr) == 0)
	{
		log_info("   %s: Empty path is not possible, using default",
		         option);
	}

	return ptr;
}

static char *parseFTLconf(FILE *fp, const char * key)
{
	// Return NULL if fp is an invalid file pointer
	if(fp == NULL)
		return NULL;

	char *keystr = calloc(strlen(key)+2, sizeof(char));
	if(keystr == NULL)
	{
		log_crit("Could not allocate memory (keystr) in parseFTLconf()");
		return NULL;
	}
	sprintf(keystr, "%s=", key);

	// Lock mutex
	const int lret = pthread_mutex_lock(&lock);
	log_debug(DEBUG_LOCKS, "Obtained config lock");
	if(lret != 0)
		log_err("Error when obtaining config lock: %s", strerror(lret));

	// Go to beginning of file
	fseek(fp, 0L, SEEK_SET);

	errno = 0;
	while(getline(&conflinebuffer, &size, fp) != -1)
	{
		// Check if memory allocation failed
		if(conflinebuffer == NULL)
			break;

		// Skip comment lines
		if(conflinebuffer[0] == '#' || conflinebuffer[0] == ';')
			continue;

		// Skip lines with other keys
		if((strstr(conflinebuffer, keystr)) == NULL)
			continue;

		// otherwise: key found
		free(keystr);
		// Note: value is still a pointer into the conflinebuffer
		//       its memory will get released in releaseConfigMemory()
		char *value = find_equals(conflinebuffer) + 1;
		// Trim whitespace at beginning and end, this function
		// modifies the string inplace
		trim_whitespace(value);

		const int uret = pthread_mutex_unlock(&lock);
		log_debug(DEBUG_LOCKS, "Released config lock (match)");
		if(uret != 0)
			log_err("Error when releasing config lock (no match): %s", strerror(uret));

		return value;
	}

	if(errno == ENOMEM)
		log_crit("Could not allocate memory (getline) in parseFTLconf()");

	const int uret = pthread_mutex_unlock(&lock);
	log_debug(DEBUG_LOCKS, "Released config lock (no match)");
	if(uret != 0)
		log_err("Error when releasing config lock (no match): %s", strerror(uret));

	// Free keystr memory
	free(keystr);

	// Key not found or memory error -> return NULL
	return NULL;
}

void releaseConfigMemory(void)
{
	if(conflinebuffer != NULL)
	{
		free(conflinebuffer);
		conflinebuffer = NULL;
		size = 0;
	}
}

void init_config_mutex(void)
{
	// Initialize the lock attributes
	pthread_mutexattr_t lock_attr = {};
	pthread_mutexattr_init(&lock_attr);

	// Initialize the lock
	pthread_mutex_init(&lock, &lock_attr);

	// Destroy the lock attributes since we're done with it
	pthread_mutexattr_destroy(&lock_attr);
}

static void getPrivacyLevelLegacy(FILE *fp)
{
	// See if we got a file handle, if not we have to open
	// the config file ourselves
	bool opened = false;
	const char *path = NULL;
	if(fp == NULL)
	{
		if((fp = openFTLconf(&path)) == NULL)
			// Return silently if there is no config file available
			return;
		opened = true;
	}

	int value = 0;
	char *buffer = parseFTLconf(fp, "PRIVACYLEVEL");
	if(buffer != NULL && sscanf(buffer, "%i", &value) == 1)
	{
		// Check for change and validity of privacy level (set in FTL.h)
		if(value >= PRIVACY_SHOW_ALL &&
		   value <= PRIVACY_MAXIMUM &&
		   value > config.misc.privacylevel.v.privacy_level)
		{
			config.misc.privacylevel.v.privacy_level = value;
		}
	}

	// Release memory
	releaseConfigMemory();

	// Have to close the config file if we opened it
	if(opened)
		fclose(fp);
}

static void getBlockingModeLegacy(FILE *fp)
{
	// (Re-)set default value
	config.dns.blockingmode.v.blocking_mode = config.dns.blockingmode.d.blocking_mode;

	// See if we got a file handle, if not we have to open
	// the config file ourselves
	bool opened = false;
	const char *path = NULL;
	if(fp == NULL)
	{
		if((fp = openFTLconf(&path)) == NULL)
			// Return silently if there is no config file available
			return;
		opened = true;
	}

	// Get config string (if present)
	char *buffer = parseFTLconf(fp, "BLOCKINGMODE");
	if(buffer != NULL)
	{
		if(strcasecmp(buffer, "NXDOMAIN") == 0)
			config.dns.blockingmode.v.blocking_mode = MODE_NX;
		else if(strcasecmp(buffer, "NULL") == 0)
			config.dns.blockingmode.v.blocking_mode = MODE_NULL;
		else if(strcasecmp(buffer, "IP-NODATA-AAAA") == 0)
			config.dns.blockingmode.v.blocking_mode = MODE_IP_NODATA_AAAA;
		else if(strcasecmp(buffer, "IP") == 0)
			config.dns.blockingmode.v.blocking_mode = MODE_IP;
		else if(strcasecmp(buffer, "NODATA") == 0)
			config.dns.blockingmode.v.blocking_mode = MODE_NODATA;
		else
			log_warn("Unknown blocking mode, using NULL as fallback");
	}

	// Release memory
	releaseConfigMemory();

	// Have to close the config file if we opened it
	if(opened)
		fclose(fp);
}

// Routine for setting the debug flags in the config struct
static void setDebugOption(FILE* fp, const char* option, enum debug_flag flag)
{
	const char *buffer = parseFTLconf(fp, option);

	// Return early if the key has not been found in FTL's config file
	if(buffer == NULL)
		return;

	struct conf_item *debug = get_debug_item(flag);

	// Set bit if value equals "true", clear bit otherwise
	bool bit = false;
	if(parseBool(buffer, &bit))
		debug->v.b = bit;
}

static void readDebugingSettingsLegacy(FILE *fp)
{
	// Set default (no debug instructions set)
	set_all_debug(false);

	// See if we got a file handle, if not we have to open
	// the config file ourselves
	bool opened = false;
	const char *path = NULL;
	if(fp == NULL)
	{
		if((fp = openFTLconf(&path)) == NULL)
			// Return silently if there is no config file available
			return;
		opened = true;
	}

	// DEBUG_ALL
	// defaults to: false
	// ~0 is a shortcut for "all bits set"
	setDebugOption(fp, "DEBUG_ALL", ~(enum debug_flag)0);

	for(enum debug_flag flag = DEBUG_DATABASE; flag < DEBUG_EXTRA; flag <<= 1)
	{
		// DEBUG_DATABASE
		const char *name;
		debugstr(flag, &name);
		setDebugOption(fp, name, flag);
	}

	// Parse debug options
	parse_debug_options();

	if(debug_any)
	{
		// Enable debug logging in dnsmasq (only effective before starting the resolver)
		argv_dnsmasq[2] = "--log-debug";
	}

	// Have to close the config file if we opened it
	if(opened)
	{
		fclose(fp);

		// Release memory only when we opened the file
		// Otherwise, it may still be needed outside of
		// this function (initial config parsing)
		releaseConfigMemory();
	}
}

// Returns true if we found a setting
static bool parseBool(const char *option, bool *ptr)
{
	if(option == NULL)
		return false;

	else if(strcasecmp(option, "false") == 0 ||
	        strcasecmp(option, "no") == 0)
	{
		*ptr = false;
		return true;
	}

	else if(strcasecmp(option, "true") == 0 ||
	        strcasecmp(option, "yes") == 0)
	{
		*ptr = true;
		return true;
	}

	return false;
}