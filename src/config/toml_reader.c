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
#include "toml_reader.h"
#include "config.h"
#include "setupVars.h"
#include "log.h"
// getprio(), setprio()
#include <sys/resource.h>
// argv_dnsmasq
#include "args.h"
// INT_MAX
#include <limits.h>

#include "tomlc99/toml.h"
#include "../datastructure.h"
// openFTLtoml()
#include "toml_helper.h"

// Private prototypes
static toml_table_t *parseTOML(void);
static void reportDebugConfig(void);

bool readFTLtoml(void)
{
	// Initialize config with default values
	initConfig();

	// Parse lines in the config file
	toml_table_t *conf = parseTOML();
	if(!conf)
		return false;

	// Try to read debug config. This is done before the full config
	// parsing to allow for debug output further down
	toml_table_t *conf_debug = toml_table_in(conf, "debug");
	if(conf_debug)
		readTOMLvalue(&config.debug.config, "config", conf_debug);

	log_debug(DEBUG_CONFIG, "Reading TOML config file: full config");

	// Read all known config items
	for(unsigned int i = 0; i < CONFIG_ELEMENTS; i++)
	{
		// Get pointer to memory location of this conf_item
		struct conf_item *conf_item = get_conf_item(i);
		toml_table_t *table[MAX_CONFIG_PATH_DEPTH] = { 0 };
		unsigned int level = config_path_depth(conf_item);

		// Parse tree of properties
		for(unsigned int j = 0; j < level-1; j++)
		{
			// Get table at this level
			table[j] = toml_table_in(j > 0 ? table[j-1] : conf, conf_item->p[j]);
			if(!table[j])
			{
				log_debug(DEBUG_CONFIG, "%s DOES NOT EXIST", conf_item->k);
				break;
			}
		}

		// Try to parse config item
		readTOMLvalue(conf_item, conf_item->p[level-1], table[level-2]);
	}

	// Report debug config if enabled
	parse_debug_options();
	reportDebugConfig();

	// Free memory allocated by the TOML parser and return success
	toml_free(conf);
	return true;
}

// Parse TOML config file
static toml_table_t *parseTOML(void)
{
	// Try to open default config file. Use fallback if not found
	FILE *fp;
	if((fp = openFTLtoml("r")) == NULL)
	{
		log_debug(DEBUG_CONFIG, "No config file available (%s), using defaults",
		          strerror(errno));
		return NULL;
	}

	// Parse lines in the config file
	char errbuf[200];
	toml_table_t *conf = toml_parse_file(fp, errbuf, sizeof(errbuf));
	fclose(fp);

	// Check for errors
	if(conf == NULL)
	{
		log_err("Cannot parse config file: %s", errbuf);
		return NULL;
	}

	log_debug(DEBUG_CONFIG, "TOML file parsing: OK");
	return conf;
}

bool getPrivacyLevel(void)
{
	log_debug(DEBUG_CONFIG, "Reading TOML config file: privacy level");

	// Parse config file
	toml_table_t *conf = parseTOML();
	if(!conf)
		return false;

	// Get [misc]
	toml_table_t *misc = toml_table_in(conf, "misc");
	if(!misc)
	{
		log_debug(DEBUG_CONFIG, "misc does not exist");
		toml_free(conf);
		return false;
	}

	// Get misc.privacyLevel
	toml_datum_t privacylevel = toml_int_in(misc, "privacyLevel");
	if(!privacylevel.ok)
	{
		log_debug(DEBUG_CONFIG, "misc.privacyLevel does not exist");
		toml_free(conf);
		return false;
	}

	// Check if privacy level is valid
	if(privacylevel.u.i >= PRIVACY_SHOW_ALL && privacylevel.u.i <= PRIVACY_MAXIMUM)
		config.misc.privacylevel.v.privacy_level = privacylevel.u.i;
	else
		log_warn("Invalid setting for misc.privacyLevel");

	toml_free(conf);
	return true;
}

bool getBlockingMode(void)
{
	log_debug(DEBUG_CONFIG, "Reading TOML config file: DNS blocking mode");

	// Parse config file
	toml_table_t *conf = parseTOML();
	if(!conf)
		return false;

	// Get [dns]
	toml_table_t *dns = toml_table_in(conf, "dns");
	if(!dns)
	{
		log_debug(DEBUG_CONFIG, "dns does not exist");
		toml_free(conf);
		return false;
	}

	// Get dns.blocking mode
	toml_datum_t blockingmode = toml_string_in(dns, "blockingmode");
	if(!blockingmode.ok)
	{
		log_debug(DEBUG_CONFIG, "dns.blockingmode DOES NOT EXIST");
		toml_free(conf);
		return false;
	}

	// Iterate over possible blocking modes and check if it applies
	const int blocking_mode = get_blocking_mode_val(blockingmode.u.s);
	if(blocking_mode != -1)
		config.dns.blockingmode.v.blocking_mode = blocking_mode;
	else
		log_warn("Config setting %s is invalid, allowed options are: %s",
		         config.dns.blockingmode.k, config.dns.blockingmode.h);
	free(blockingmode.u.s);

	// Free memory and return success
	toml_free(conf);
	return true;
}

bool getLogFilePathTOML(void)
{
	log_debug(DEBUG_CONFIG, "Reading TOML config file: log file path");

	toml_table_t *conf = parseTOML();
	if(!conf)
		return false;

	toml_table_t *files = toml_table_in(conf, "files");
	if(!files)
	{
		log_debug(DEBUG_CONFIG, "files does not exist");
		toml_free(conf);
		return false;
	}

	toml_datum_t log = toml_string_in(files, "log");
	if(!log.ok)
	{
		log_debug(DEBUG_CONFIG, "files.log DOES NOT EXIST");
		toml_free(conf);
		return false;
	}

	// Only replace string when it is different
	if(strcmp(config.files.log.v.s,log.u.s) != 0)
		config.files.log.v.s = log.u.s; // Allocated string
	else
		free(log.u.s);

	toml_free(conf);
	return true;
}

static void reportDebugConfig(void)
{
	log_debug(DEBUG_ANY, "***********************");
	log_debug(DEBUG_ANY, "*    DEBUG SETTINGS   *");

	// Read all known debug config items
	for(unsigned int debug_flag = 0; debug_flag < DEBUG_ELEMENTS; debug_flag++)
	{
		const char *name;
		debugstr(debug_flag, &name);
		unsigned int spaces = 20 - strlen(name);
		log_debug(DEBUG_ANY, "* %s:%*s %s  *", name+6, spaces, "", debug_flags[debug_flag] ? "YES" : "NO ");
	}
	log_debug(DEBUG_ANY, "***********************");
}