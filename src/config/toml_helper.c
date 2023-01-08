/* Pi-hole: A black hole for Internet advertisements
*  (c) 2021 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  TOML config writer routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "toml_helper.h"
#include "log.h"
#include "config/config.h"
// get_refresh_hostnames_str()
#include "datastructure.h"

// Open the TOML file for reading or writing
FILE * __attribute((malloc)) __attribute((nonnull(1))) openFTLtoml(const char *mode)
{
	FILE *fp;
	// If reading: first check if there is a local file
	if(strcmp(mode, "r") == 0 &&
	   (fp = fopen("pihole-FTL.toml", mode)) != NULL)
		return fp;

	// No readable local file found, try global file
	fp = fopen(GLOBALTOMLPATH, mode);

	return fp;
}

// Print a string to a TOML file, escaping special characters as necessary
static void printTOMLstring(FILE *fp, const char *s)
{
	// Substitute empty string if pointer is NULL
	if(s == NULL)
		 s = "";

	bool ok = true;
	// Check if string is printable and does not contain any special characters
	for (const char* p = s; *p && ok; p++)
	{
		int ch = *p;
		ok = isprint(ch) && ch != '"' && ch != '\\';
	}

	// If string is printable and does not contain any special characters, we can
	// print it as is without furhter escaping
	if (ok)
	{
		fprintf(fp, "\"%s\"", s);
		return;
	}

	// Otherwise, we need to escape special characters, this is more work
	int len = strlen(s);
	fprintf(fp, "\"");
	for ( ; len; len--, s++)
	{
		int ch = *s;
		if (isprint(ch) && ch != '"' && ch != '\\')
		{
			putc(ch, fp);
			continue;
		}

		// Escape special characters
		switch (ch) {
			case 0x08: fprintf(fp, "\\b"); continue;
			case 0x09: fprintf(fp, "\\t"); continue;
			case 0x0a: fprintf(fp, "\\n"); continue;
			case 0x0c: fprintf(fp, "\\f"); continue;
			case 0x0d: fprintf(fp, "\\r"); continue;
			case '"':  fprintf(fp, "\\\""); continue;
			case '\\': fprintf(fp, "\\\\"); continue;
			default:   fprintf(fp, "\\0x%02x", ch & 0xff); continue;
		}
	}
	fprintf(fp, "\"");
}

// Indentation (tabs and/or spaces) is allowed but not required, we use it for
// the sake of readability
void indentTOML(FILE *fp, const unsigned int indent)
{
	for (unsigned int i = 0; i < 2*indent; i++)
		fputc(' ', fp);
}

// Write a TOML value to a file depending on its type
void writeTOMLvalue(FILE * fp, const enum conf_type t, union conf_value *v)
{
	switch(t)
	{
		case CONF_BOOL:
			fprintf(fp, "%s", v->b ? "true" : "false");
			break;
		case CONF_INT:
			fprintf(fp, "%i", v->i);
			break;
		case CONF_UINT:
		case CONF_ENUM_PRIVACY_LEVEL:
			fprintf(fp, "%u", v->ui);
			break;
		case CONF_LONG:
			fprintf(fp, "%li", v->l);
			break;
		case CONF_ULONG:
			fprintf(fp, "%lu", v->ul);
			break;
		case CONF_STRING:
			printTOMLstring(fp, v->s);
			break;
		case CONF_ENUM_PTR_TYPE:
			printTOMLstring(fp, get_ptr_type_str(v->ptr_type));
			break;
		case CONF_ENUM_BUSY_TYPE:
			printTOMLstring(fp, get_busy_reply_str(v->busy_reply));
			break;
		case CONF_ENUM_BLOCKING_MODE:
			printTOMLstring(fp, get_blocking_mode_str(v->blocking_mode));
			break;
		case CONF_ENUM_REFRESH_HOSTNAMES:
			printTOMLstring(fp, get_refresh_hostnames_str(v->refresh_hostnames));
			break;
		case CONF_STRUCT_IN_ADDR:
		{
			char addr4[INET_ADDRSTRLEN] = { 0 };
			inet_ntop(AF_INET, &v->in_addr, addr4, INET_ADDRSTRLEN);
			printTOMLstring(fp, addr4);
			break;
		}
		case CONF_STRUCT_IN6_ADDR:
		{
			char addr6[INET6_ADDRSTRLEN] = { 0 };
			inet_ntop(AF_INET6, &v->in6_addr, addr6, INET6_ADDRSTRLEN);
			printTOMLstring(fp, addr6);
			break;
		}
	}
}

// Read a TOML value from a table depending on its type
void readTOMLvalue(struct conf_item *conf_item, const char* key, toml_table_t *toml)
{
	switch(conf_item->t)
	{
		case CONF_BOOL:
		{
			const toml_datum_t val = toml_bool_in(toml, key);
			if(val.ok)
				conf_item->v.b = val.u.b;
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type bool", conf_item->k);
			break;
		}
		case CONF_INT:
		{
			const toml_datum_t val = toml_int_in(toml, key);
			if(val.ok)
				conf_item->v.i = val.u.i;
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type integer", conf_item->k);
			break;
		}
		case CONF_UINT:
		{
			const toml_datum_t val = toml_int_in(toml, key);
			if(val.ok && val.u.i >= 0)
				conf_item->v.ui = val.u.i;
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type unsigned integer", conf_item->k);
			break;
		}
		case CONF_LONG:
		{
			const toml_datum_t val = toml_int_in(toml, key);
			if(val.ok)
				conf_item->v.l = val.u.i;
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type long", conf_item->k);
			break;
		}
		case CONF_ULONG:
		{
			const toml_datum_t val = toml_int_in(toml, key);
			if(val.ok && val.u.i >= 0)
				conf_item->v.ul = val.u.i;
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type unsigned long", conf_item->k);
			break;
		}
		case CONF_STRING:
		{
			const toml_datum_t val = toml_string_in(toml, key);
			if(val.ok)
				conf_item->v.s = val.u.s;
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type string", conf_item->k);
			break;
		}
		case CONF_ENUM_PTR_TYPE:
		{
			const toml_datum_t val = toml_string_in(toml, key);
			if(val.ok)
			{
				const int ptr_type = get_ptr_type_val(val.u.s);
				if(ptr_type != -1)
					conf_item->v.ptr_type = ptr_type;
				else
					log_warn("Config setting %s is invalid, allowed options are: %s", conf_item->k, conf_item->h);
			}
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type string", conf_item->k);
			break;
		}
		case CONF_ENUM_BUSY_TYPE:
		{
			const toml_datum_t val = toml_string_in(toml, key);
			if(val.ok)
			{
				const int busy_reply = get_busy_reply_val(val.u.s);
				if(busy_reply != -1)
					conf_item->v.busy_reply = busy_reply;
				else
					log_warn("Config setting %s is invalid, allowed options are: %s", conf_item->k, conf_item->h);
			}
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type string", conf_item->k);
			break;
		}
		case CONF_ENUM_BLOCKING_MODE:
		{
			const toml_datum_t val = toml_string_in(toml, key);
			if(val.ok)
			{
				const int blocking_mode = get_blocking_mode_val(val.u.s);
				if(blocking_mode != -1)
					conf_item->v.blocking_mode = blocking_mode;
				else
					log_warn("Config setting %s is invalid, allowed options are: %s", conf_item->k, conf_item->h);
			}
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type string", conf_item->k);
			break;
		}
		case CONF_ENUM_REFRESH_HOSTNAMES:
		{
			const toml_datum_t val = toml_string_in(toml, key);
			if(val.ok)
			{
				const int refresh_hostnames = get_refresh_hostnames_val(val.u.s);
				if(refresh_hostnames != -1)
					conf_item->v.refresh_hostnames = refresh_hostnames;
				else
					log_warn("Config setting %s is invalid, allowed options are: %s", conf_item->k, conf_item->h);
			}
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is not of type string", conf_item->k);
			break;
		}
		case CONF_ENUM_PRIVACY_LEVEL:
		{
			const toml_datum_t val = toml_int_in(toml, key);
			if(val.ok && val.u.i >= PRIVACY_SHOW_ALL && val.u.i <= PRIVACY_MAXIMUM)
				conf_item->v.i = val.u.i;
			else
				log_debug(DEBUG_CONFIG, "%s does not exist or is invalid", conf_item->k);
			break;
		}
		case CONF_STRUCT_IN_ADDR:
		{
			struct in_addr addr4 = { 0 };
			const toml_datum_t val = toml_string_in(toml, key);
			if(val.ok && inet_pton(AF_INET, val.u.s, &addr4))
				memcpy(&conf_item->v.in_addr, &addr4, sizeof(addr4));
			break;
		}
		case CONF_STRUCT_IN6_ADDR:
		{
			struct in6_addr addr6 = { 0 };
			const toml_datum_t val = toml_string_in(toml, key);
			if(val.ok && inet_pton(AF_INET6, val.u.s, &addr6))
				memcpy(&conf_item->v.in6_addr, &addr6, sizeof(addr6));
			break;
		}
	}
}