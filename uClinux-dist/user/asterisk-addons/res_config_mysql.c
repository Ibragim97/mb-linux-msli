/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Copyright (C) 1999-2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>  - Asterisk Author
 * Matthew Boehm <mboehm@cytelcom.com> - MySQL RealTime Driver Author
 *
 * res_config_mysql.c <mysql plugin for RealTime configuration engine>
 *
 * v2.0   - (10-07-05) - mutex_lock fixes (bug #4973, comment #0034602)
 *
 * v1.9   - (08-19-05) - Added support to correctly honor the family database specified
 *                       in extconfig.conf (bug #4973)
 *
 * v1.8   - (04-21-05) - Modified return values of update_mysql to better indicate
 *                       what really happened.
 *
 * v1.7   - (01-28-05) - Fixed non-initialization of ast_category struct
 *                       in realtime_multi_mysql function which caused segfault. 
 *
 * v1.6   - (00-00-00) - Skipped to bring comments into sync with version number in CVS.
 *
 * v1.5.1 - (01-26-05) - Added better(?) locking stuff
 *
 * v1.5   - (01-26-05) - Brought up to date with new config.h changes (bug #3406)
 *                     - Added in extra locking provided by georg (bug #3248)
 *
 * v1.4   - (12-02-04) - Added realtime_multi_mysql function
 *                        This function will return an ast_config with categories,
 *                        unlike standard realtime_mysql which only returns
 *                        a linked list of ast_variables
 *
 * v1.3   - (12-01-04) - Added support other operators
 *                       Ex: =, !=, LIKE, NOT LIKE, RLIKE, etc...
 *
 * v1.2   - (11-DD-04) - Added reload. Updated load and unload.
 *                       Code beautification (doc/CODING-GUIDELINES)
 */

#include <asterisk.h>

#include <asterisk/channel.h>
#include <asterisk/logger.h>
#include <asterisk/config.h>
#include <asterisk/module.h>
#include <asterisk/lock.h>
#include <asterisk/options.h>
#include <asterisk/cli.h>
#include <asterisk/utils.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <mysql/mysql.h>
#include <mysql/mysql_version.h>
#include <mysql/errmsg.h>

#define AST_MODULE "res_config_mysql"

AST_MUTEX_DEFINE_STATIC(mysql_lock);
#define RES_CONFIG_MYSQL_CONF "res_mysql.conf"
MYSQL         mysql;
static char   dbhost[50];
static char   dbuser[50];
static char   dbpass[50];
static char   dbname[50];
static char   dbsock[50];
static int    dbport;
static int    connected;
static time_t connect_time;

static int parse_config(void);
static int mysql_reconnect(const char *database);
static int realtime_mysql_status(int fd, int argc, char **argv);

static char cli_realtime_mysql_status_usage[] =
"Usage: realtime mysql status\n"
"       Shows connection information for the MySQL RealTime driver\n";

static struct ast_cli_entry cli_realtime_mysql_status = {
        { "realtime", "mysql", "status", NULL }, realtime_mysql_status,
        "Shows connection information for the MySQL RealTime driver", cli_realtime_mysql_status_usage, NULL };

static struct ast_variable *realtime_mysql(const char *database, const char *table, va_list ap)
{
	MYSQL_RES *result;
	MYSQL_ROW row;
	MYSQL_FIELD *fields;
	int numFields, i, valsz;
	char sql[512];
	char buf[511]; /* Keep this size uneven as it is 2n+1. */
	char *stringp;
	char *chunk;
	char *op;
	const char *newparam, *newval;
	struct ast_variable *var=NULL, *prev=NULL;

	if(!table) {
		ast_log(LOG_WARNING, "MySQL RealTime: No table specified.\n");
		return NULL;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if(!newparam || !newval)  {
		ast_log(LOG_WARNING, "MySQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		return NULL;
	}

	/* Must connect to the server before anything else, as the escape function requires the mysql handle. */
	ast_mutex_lock(&mysql_lock);
	if (!mysql_reconnect(database)) {
		ast_mutex_unlock(&mysql_lock);
		return NULL;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	if(!strchr(newparam, ' ')) op = " ="; else op = "";

	if ((valsz = strlen (newval)) * 2 + 1 > sizeof(buf))
		valsz = (sizeof(buf) - 1) / 2;
	mysql_real_escape_string(&mysql, buf, newval, valsz);
	snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s%s '%s'", table, newparam, op, buf);
	while((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if(!strchr(newparam, ' ')) op = " ="; else op = "";
		if ((valsz = strlen (newval)) * 2 + 1 > sizeof(buf))
			valsz = (sizeof(buf) - 1) / 2;
		mysql_real_escape_string(&mysql, buf, newval, valsz);
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " AND %s%s '%s'", newparam, op, buf);
	}
	va_end(ap);

	ast_log(LOG_DEBUG, "MySQL RealTime: Retrieve SQL: %s\n", sql);

	/* Execution. */
	if(mysql_real_query(&mysql, sql, strlen(sql))) {
		ast_log(LOG_WARNING, "MySQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_log(LOG_DEBUG, "MySQL RealTime: Query: %s\n", sql);
		ast_log(LOG_DEBUG, "MySQL RealTime: Query Failed because: %s\n", mysql_error(&mysql));
		ast_mutex_unlock(&mysql_lock);
		return NULL;
	}

	if((result = mysql_store_result(&mysql))) {
		numFields = mysql_num_fields(result);
		fields = mysql_fetch_fields(result);

		while((row = mysql_fetch_row(result))) {
			for(i = 0; i < numFields; i++) {
				stringp = row[i];
				while(stringp) {
					chunk = strsep(&stringp, ";");
					if(chunk && !ast_strlen_zero(ast_strip(chunk))) {
						if(prev) {
							prev->next = ast_variable_new(fields[i].name, chunk);
							if (prev->next) {
								prev = prev->next;
							}
						} else {
							prev = var = ast_variable_new(fields[i].name, chunk);
						}
					}
				}
			}
		}
	} else {                                
		ast_log(LOG_WARNING, "MySQL RealTime: Could not find any rows in table %s.\n", table);
	}

	ast_mutex_unlock(&mysql_lock);
	mysql_free_result(result);

	return var;
}

static struct ast_config *realtime_multi_mysql(const char *database, const char *table, va_list ap)
{
	MYSQL_RES *result;
	MYSQL_ROW row;
	MYSQL_FIELD *fields;
	int numFields, i, valsz;
	char sql[512];
	char buf[511]; /* Keep this size uneven as it is 2n+1. */
	const char *initfield = NULL;
	char *stringp;
	char *chunk;
	char *op;
	const char *newparam, *newval;
	struct ast_realloca ra;
	struct ast_variable *var=NULL;
	struct ast_config *cfg = NULL;
	struct ast_category *cat = NULL;

	if(!table) {
		ast_log(LOG_WARNING, "MySQL RealTime: No table specified.\n");
		return NULL;
	}
	
	memset(&ra, 0, sizeof(ra));

	cfg = ast_config_new();
	if (!cfg) {
		/* If I can't alloc memory at this point, why bother doing anything else? */
		ast_log(LOG_WARNING, "Out of memory!\n");
		return NULL;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if(!newparam || !newval)  {
		ast_log(LOG_WARNING, "MySQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		ast_config_destroy(cfg);
		return NULL;
	}

	initfield = ast_strdupa(newparam);
	if(initfield && (op = strchr(initfield, ' '))) {
		*op = '\0';
	}

	/* Must connect to the server before anything else, as the escape function requires the mysql handle. */
	ast_mutex_lock(&mysql_lock);
	if (!mysql_reconnect(database)) {
		ast_mutex_unlock(&mysql_lock);
		return NULL;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	if(!strchr(newparam, ' ')) op = " ="; else op = "";

	if ((valsz = strlen (newval)) * 2 + 1 > sizeof(buf))
		valsz = (sizeof(buf) - 1) / 2;
	mysql_real_escape_string(&mysql, buf, newval, valsz);
	snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s%s '%s'", table, newparam, op, buf);
	while((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if(!strchr(newparam, ' ')) op = " ="; else op = "";
		if ((valsz = strlen (newval)) * 2 + 1 > sizeof(buf))
			valsz = (sizeof(buf) - 1) / 2;
		mysql_real_escape_string(&mysql, buf, newval, valsz);
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " AND %s%s '%s'", newparam, op, buf);
	}

	if(initfield) {
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " ORDER BY %s", initfield);
	}

	va_end(ap);

	ast_log(LOG_DEBUG, "MySQL RealTime: Retrieve SQL: %s\n", sql);

	/* Execution. */
	if(mysql_real_query(&mysql, sql, strlen(sql))) {
		ast_log(LOG_WARNING, "MySQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_log(LOG_DEBUG, "MySQL RealTime: Query: %s\n", sql);
		ast_log(LOG_DEBUG, "MySQL RealTime: Query Failed because: %s\n", mysql_error(&mysql));
		ast_mutex_unlock(&mysql_lock);
		ast_config_destroy(cfg);
		return NULL;
	}

	if((result = mysql_store_result(&mysql))) {
		numFields = mysql_num_fields(result);
		fields = mysql_fetch_fields(result);

		while((row = mysql_fetch_row(result))) {
			var = NULL;
			cat = ast_category_new("");
			if(!cat) {
				ast_log(LOG_WARNING, "Out of memory!\n");
				continue;
			}
			for(i = 0; i < numFields; i++) {
				stringp = row[i];
				while(stringp) {
					chunk = strsep(&stringp, ";");
					if(chunk && !ast_strlen_zero(ast_strip(chunk))) {
						if(initfield && !strcmp(initfield, fields[i].name)) {
							ast_category_rename(cat, chunk);
						}
						var = ast_variable_new(fields[i].name, chunk);
						ast_variable_append(cat, var);
					}
				}
			}
			ast_category_append(cfg, cat);
		}
	} else {
		ast_log(LOG_WARNING, "MySQL RealTime: Could not find any rows in table %s.\n", table);
	}

	ast_mutex_unlock(&mysql_lock);
	mysql_free_result(result);

	return cfg;
}

static int update_mysql(const char *database, const char *table, const char *keyfield, const char *lookup, va_list ap)
{
	my_ulonglong numrows;
	char sql[512];
	char buf[511]; /* Keep this size uneven as it is 2n+1. */
	int valsz;
	const char *newparam, *newval;

	if(!table) {
		ast_log(LOG_WARNING, "MySQL RealTime: No table specified.\n");
               return -1;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if(!newparam || !newval)  {
		ast_log(LOG_WARNING, "MySQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
               return -1;
	}

	/* Must connect to the server before anything else, as the escape function requires the mysql handle. */
	ast_mutex_lock(&mysql_lock);
	if (!mysql_reconnect(database)) {
		ast_mutex_unlock(&mysql_lock);
		return -1;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	if ((valsz = strlen (newval)) * 1 + 1 > sizeof(buf))
		valsz = (sizeof(buf) - 1) / 2;
	mysql_real_escape_string(&mysql, buf, newval, valsz);
	snprintf(sql, sizeof(sql), "UPDATE %s SET %s = '%s'", table, newparam, buf);
	while((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if ((valsz = strlen (newval)) * 2 + 1 > sizeof(buf))
			valsz = (sizeof(buf) - 1) / 2;
		mysql_real_escape_string(&mysql, buf, newval, valsz);
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), ", %s = '%s'", newparam, buf);
	}
	va_end(ap);
	if ((valsz = strlen (lookup)) * 1 + 1 > sizeof(buf))
		valsz = (sizeof(buf) - 1) / 2;
	mysql_real_escape_string(&mysql, buf, lookup, valsz);
	snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " WHERE %s = '%s'", keyfield, buf);

	ast_log(LOG_DEBUG,"MySQL RealTime: Update SQL: %s\n", sql);

	/* Execution. */
	if(mysql_real_query(&mysql, sql, strlen(sql))) {
		ast_log(LOG_WARNING, "MySQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_log(LOG_DEBUG, "MySQL RealTime: Query: %s\n", sql);
		ast_log(LOG_DEBUG, "MySQL RealTime: Query Failed because: %s\n", mysql_error(&mysql));
		ast_mutex_unlock(&mysql_lock);
		return -1;
	}

	numrows = mysql_affected_rows(&mysql);
	ast_mutex_unlock(&mysql_lock);

	ast_log(LOG_DEBUG,"MySQL RealTime: Updated %llu rows on table: %s\n", numrows, table);

	/* From http://dev.mysql.com/doc/mysql/en/mysql-affected-rows.html
	 * An integer greater than zero indicates the number of rows affected
	 * Zero indicates that no records were updated
	 * -1 indicates that the query returned an error (although, if the query failed, it should have been caught above.)
	*/

	if(numrows >= 0)
		return (int)numrows;

	return -1;
}

static struct ast_config *config_mysql(const char *database, const char *table, const char *file, struct ast_config *cfg, int withcomments)
{
	MYSQL_RES *result;
	MYSQL_ROW row;
	my_ulonglong num_rows;
	struct ast_variable *new_v;
	struct ast_category *cur_cat;
	char sql[250] = "";
	char last[80] = "";
	int last_cat_metric = 0;

	last[0] = '\0';

	if(!file || !strcmp(file, RES_CONFIG_MYSQL_CONF)) {
		ast_log(LOG_WARNING, "MySQL RealTime: Cannot configure myself.\n");
		return NULL;
	}

	snprintf(sql, sizeof(sql), "SELECT category, var_name, var_val, cat_metric FROM %s WHERE filename='%s' and commented=0 ORDER BY filename, cat_metric desc, var_metric asc, category, var_name, var_val, id", table, file);

	ast_log(LOG_DEBUG, "MySQL RealTime: Static SQL: %s\n", sql);

	/* We now have our complete statement; Lets connect to the server and execute it. */
	ast_mutex_lock(&mysql_lock);
	if(!mysql_reconnect(database)) {
		ast_mutex_unlock(&mysql_lock);
		return NULL;
	}

	if(mysql_real_query(&mysql, sql, strlen(sql))) {
		ast_log(LOG_WARNING, "MySQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_log(LOG_DEBUG, "MySQL RealTime: Query: %s\n", sql);
		ast_log(LOG_DEBUG, "MySQL RealTime: Query Failed because: %s\n", mysql_error(&mysql));
		ast_mutex_unlock(&mysql_lock);
		return NULL;
	}

	if((result = mysql_store_result(&mysql))) {
		num_rows = mysql_num_rows(result);
		ast_log(LOG_DEBUG, "MySQL RealTime: Found %llu rows.\n", num_rows);

		/* There might exist a better way to access the column names other than counting,
                   but I believe that would require another loop that we don't need. */

		while((row = mysql_fetch_row(result))) {
			if(!strcmp(row[1], "#include")) {
				if (!ast_config_internal_load(row[2], cfg, 0)) {
					mysql_free_result(result);
					ast_mutex_unlock(&mysql_lock);
					return NULL;
				}
				continue;
			}

			if(strcmp(last, row[0]) || last_cat_metric != atoi(row[3])) {
				cur_cat = ast_category_new(row[0]);
				if (!cur_cat) {
					ast_log(LOG_WARNING, "Out of memory!\n");
					break;
				}
				strcpy(last, row[0]);
				last_cat_metric = atoi(row[3]);
				ast_category_append(cfg, cur_cat);
			}
			new_v = ast_variable_new(row[1], row[2]);
			ast_variable_append(cur_cat, new_v);
		}
	} else {
		ast_log(LOG_WARNING, "MySQL RealTime: Could not find config '%s' in database.\n", file);
	}

	mysql_free_result(result);
	ast_mutex_unlock(&mysql_lock);

	return cfg;
}

static struct ast_config_engine mysql_engine = {
	.name = "mysql",
	.load_func = config_mysql,
	.realtime_func = realtime_mysql,
	.realtime_multi_func = realtime_multi_mysql,
	.update_func = update_mysql
};

static int load_module(void)
{
	parse_config();

	ast_mutex_lock(&mysql_lock);

	if(!mysql_reconnect(NULL)) {
		ast_log(LOG_WARNING, "MySQL RealTime: Couldn't establish connection. Check debug.\n");
		ast_log(LOG_DEBUG, "MySQL RealTime: Cannot Connect: %s\n", mysql_error(&mysql));
	}

	ast_config_engine_register(&mysql_engine);
	if(option_verbose) {
		ast_verbose("MySQL RealTime driver loaded.\n");
	}
	ast_cli_register(&cli_realtime_mysql_status);

	ast_mutex_unlock(&mysql_lock);

	return 0;
}

static int unload_module(void)
{
	/* Aquire control before doing anything to the module itself. */
	ast_mutex_lock(&mysql_lock);

	mysql_close(&mysql);
	ast_cli_unregister(&cli_realtime_mysql_status);
	ast_config_engine_deregister(&mysql_engine);
	if(option_verbose) {
		ast_verbose("MySQL RealTime unloaded.\n");
	}

	ast_module_user_hangup_all();

	/* Unlock so something else can destroy the lock. */
	ast_mutex_unlock(&mysql_lock);

	return 0;
}

static int reload(void)
{
	/* Aquire control before doing anything to the module itself. */
	ast_mutex_lock(&mysql_lock);

	mysql_close(&mysql);
	connected = 0;
	parse_config();

	if(!mysql_reconnect(NULL)) {
		ast_log(LOG_WARNING, "MySQL RealTime: Couldn't establish connection. Check debug.\n");
		ast_log(LOG_DEBUG, "MySQL RealTime: Cannot Connect: %s\n", mysql_error(&mysql));
	}

	ast_verbose(VERBOSE_PREFIX_2 "MySQL RealTime reloaded.\n");

	/* Done reloading. Release lock so others can now use driver. */
	ast_mutex_unlock(&mysql_lock);

	return 0;
}

static int parse_config (void)
{
	struct ast_config *config;
	const char *s;

	config = ast_config_load(RES_CONFIG_MYSQL_CONF);

	if(config) {
		if(!(s=ast_variable_retrieve(config, "general", "dbuser"))) {
			ast_log(LOG_WARNING, "MySQL RealTime: No database user found, using 'asterisk' as default.\n");
			strncpy(dbuser, "asterisk", sizeof(dbuser) - 1);
		} else {
			strncpy(dbuser, s, sizeof(dbuser) - 1);
		}

		if(!(s=ast_variable_retrieve(config, "general", "dbpass"))) {
                        ast_log(LOG_WARNING, "MySQL RealTime: No database password found, using 'asterisk' as default.\n");
                        strncpy(dbpass, "asterisk", sizeof(dbpass) - 1);
                } else {
                        strncpy(dbpass, s, sizeof(dbpass) - 1);
                }

		if(!(s=ast_variable_retrieve(config, "general", "dbhost"))) {
                        ast_log(LOG_WARNING, "MySQL RealTime: No database host found, using localhost via socket.\n");
			dbhost[0] = '\0';
                } else {
                        strncpy(dbhost, s, sizeof(dbhost) - 1);
                }

		if(!(s=ast_variable_retrieve(config, "general", "dbname"))) {
                        ast_log(LOG_WARNING, "MySQL RealTime: No database name found, using 'asterisk' as default.\n");
			strncpy(dbname, "asterisk", sizeof(dbname) - 1);
                } else {
                        strncpy(dbname, s, sizeof(dbname) - 1);
                }

		if(!(s=ast_variable_retrieve(config, "general", "dbport"))) {
                        ast_log(LOG_WARNING, "MySQL RealTime: No database port found, using 3306 as default.\n");
			dbport = 3306;
                } else {
			dbport = atoi(s);
                }

		if(dbhost && !(s=ast_variable_retrieve(config, "general", "dbsock"))) {
                        ast_log(LOG_WARNING, "MySQL RealTime: No database socket found, using '/tmp/mysql.sock' as default.\n");
                        strncpy(dbsock, "/tmp/mysql.sock", sizeof(dbsock) - 1);
                } else {
                        strncpy(dbsock, s, sizeof(dbsock) - 1);
                }
	}
	ast_config_destroy(config);

	if(dbhost) {
		ast_log(LOG_DEBUG, "MySQL RealTime Host: %s\n", dbhost);
		ast_log(LOG_DEBUG, "MySQL RealTime Port: %i\n", dbport);
	} else {
		ast_log(LOG_DEBUG, "MySQL RealTime Socket: %s\n", dbsock);
	}
	ast_log(LOG_DEBUG, "MySQL RealTime User: %s\n", dbuser);
	ast_log(LOG_DEBUG, "MySQL RealTime Password: %s\n", dbpass);

	return 1;
}

static int mysql_reconnect(const char *database)
{
	char my_database[50];
#ifdef MYSQL_OPT_RECONNECT
	my_bool trueval = 1;
#endif

	if(!database || ast_strlen_zero(database))
		ast_copy_string(my_database, dbname, sizeof(my_database));
	else
		ast_copy_string(my_database, database, sizeof(my_database));

	/* mutex lock should have been locked before calling this function. */

reconnect_tryagain:
	if((!connected) && (dbhost || dbsock) && dbuser && dbpass && my_database) {
		if(!mysql_init(&mysql)) {
			ast_log(LOG_WARNING, "MySQL RealTime: Insufficient memory to allocate MySQL resource.\n");
			connected = 0;
			return 0;
		}
		if(mysql_real_connect(&mysql, dbhost, dbuser, dbpass, my_database, dbport, dbsock, 0)) {
#ifdef MYSQL_OPT_RECONNECT
			/* The default is no longer to automatically reconnect on failure,
			 * (as of 5.0.3) so we have to set that option here. */
			mysql_options(&mysql, MYSQL_OPT_RECONNECT, &trueval);
#endif
			ast_log(LOG_DEBUG, "MySQL RealTime: Successfully connected to database.\n");
			connected = 1;
			connect_time = time(NULL);
			return 1;
		} else {
			ast_log(LOG_ERROR, "MySQL RealTime: Failed to connect database server %s on %s (err %d). Check debug for more info.\n", dbname, dbhost, mysql_errno(&mysql));
			ast_log(LOG_DEBUG, "MySQL RealTime: Cannot Connect (%d): %s\n", mysql_errno(&mysql), mysql_error(&mysql));
			connected = 0;
			return 0;
		}
	} else {
		/* MySQL likes to return an error, even if it reconnects successfully.
		 * So the postman pings twice. */
		if (mysql_ping(&mysql) != 0 && mysql_ping(&mysql) != 0) {
			connected = 0;
			ast_log(LOG_ERROR, "MySQL RealTime: Ping failed (%d).  Trying an explicit reconnect.\n", mysql_errno(&mysql));
			ast_log(LOG_DEBUG, "MySQL RealTime: Server Error (%d): %s\n", mysql_errno(&mysql), mysql_error(&mysql));
			goto reconnect_tryagain;
		}

		connected = 1;
		connect_time = time(NULL);

		if(mysql_select_db(&mysql, my_database) != 0) {
			ast_log(LOG_WARNING, "MySQL RealTime: Unable to select database: %s. Still Connected (%u).\n", my_database, mysql_errno(&mysql));
			ast_log(LOG_DEBUG, "MySQL RealTime: Database Select Failed (%u): %s\n", mysql_errno(&mysql), mysql_error(&mysql));
			return 0;
		}

		ast_log(LOG_DEBUG, "MySQL RealTime: Everything is fine.\n");
		return 1;
	}
}

static int realtime_mysql_status(int fd, int argc, char **argv)
{
	char status[256], status2[100] = "";
	int ctime = time(NULL) - connect_time;

	if(mysql_reconnect(NULL)) {
		if(dbhost) {
			snprintf(status, 255, "Connected to %s@%s, port %d", dbname, dbhost, dbport);
		} else if(dbsock) {
			snprintf(status, 255, "Connected to %s on socket file %s", dbname, dbsock);
		} else {
			snprintf(status, 255, "Connected to %s@%s", dbname, dbhost);
		}

		if(dbuser && *dbuser) {
			snprintf(status2, 99, " with username %s", dbuser);
		}

		if (ctime > 31536000) {
			ast_cli(fd, "%s%s for %d years, %d days, %d hours, %d minutes, %d seconds.\n", status, status2, ctime / 31536000, (ctime % 31536000) / 86400, (ctime % 86400) / 3600, (ctime % 3600) / 60, ctime % 60);
		} else if (ctime > 86400) {
			ast_cli(fd, "%s%s for %d days, %d hours, %d minutes, %d seconds.\n", status, status2, ctime / 86400, (ctime % 86400) / 3600, (ctime % 3600) / 60, ctime % 60);
		} else if (ctime > 3600) {
			ast_cli(fd, "%s%s for %d hours, %d minutes, %d seconds.\n", status, status2, ctime / 3600, (ctime % 3600) / 60, ctime % 60);
		} else if (ctime > 60) {
			ast_cli(fd, "%s%s for %d minutes, %d seconds.\n", status, status2, ctime / 60, ctime % 60);
		} else {
			ast_cli(fd, "%s%s for %d seconds.\n", status, status2, ctime);
		}

		return RESULT_SUCCESS;
	} else {
		return RESULT_FAILURE;
	}
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "MySQL RealTime Configuration Driver");
