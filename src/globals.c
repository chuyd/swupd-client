/*
 *   Software Updater - client side
 *
 *      Copyright © 2014-2016 Intel Corporation.
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, version 2 or later of the License.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Authors:
 *         Regis Merlino <regis.merlino@intel.com>
 *         Tim Pepper <timothy.c.pepper@linux.intel.com>
 *
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "swupd.h"

bool force = false;
bool sigcheck = true;
bool timecheck = true;
bool verify_esp_only;
bool verify_bundles_only = false;
int update_count = 0;
int update_skip = 0;
bool need_update_boot = false;
bool need_update_bootloader = false;
bool need_systemd_reexec = false;
bool update_complete = false;
char *post_update_action = NULL;
#if 0
/* disabled unused global variables */
bool ignore_config = true;
bool ignore_state = true;
#endif
bool ignore_orphans = true;
char *format_string = NULL;
char *path_prefix = NULL; /* must always end in '/' */
char *mounted_dirs = NULL;
char *bundle_to_add = NULL;
struct timeval start_time;
char *state_dir = NULL;

/* NOTE: Today the content and version server urls are the same in
 * all cases.  It is highly likely these will eventually differ, eg:
 * swupd-version.01.org and swupd-files.01.org as this enables
 * different quality of server and control of the servers
 */
bool download_only;
bool verbose_time = false;
bool local_download = false;
bool have_manifest_diskspace = false; /* assume no until checked */
bool have_network = false;	    /* assume no access until proved */
char *version_url = NULL;
char *content_url = NULL;
char *cert_path = NULL;
long update_server_port = -1;

static const char *default_version_url_path = "/usr/share/defaults/swupd/versionurl";
static const char *default_content_url_path = "/usr/share/defaults/swupd/contenturl";
static const char *default_format_path = "/usr/share/defaults/swupd/format";

timelist init_timelist(void)
{
	timelist head = TAILQ_HEAD_INITIALIZER(head);
	TAILQ_INIT(&head);
	return head;
}

static struct time *alloc_time(timelist *head)
{
	struct time *t = calloc(1, sizeof(struct time));
	if (t == NULL) {
		fprintf(stderr, "ERROR: grab_time: Failed to to allocate memory...freeing and removing timing\n");
		while (!TAILQ_EMPTY(head)) {
			struct time *iter = TAILQ_FIRST(head);
			TAILQ_REMOVE(head, iter, times);
			free(iter);
		}
		/* Malloc failed...something bad happened, stop trying and let swupd attempt to finish */
		verbose_time = false;
		return NULL;
	}
	return t;
}

/* Fill the time struct for later processing */
void grabtime_start(timelist *head, const char *name)
{
	if (verbose_time == false) {
		return;
	}

	/* Only create one element for each start/stop block */
	struct time *t = alloc_time(head);
	if (t == NULL) {
		return;
	}
	clock_gettime(CLOCK_MONOTONIC_RAW, &t->rawstart);
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t->procstart);
	t->name = name;
	t->complete = false;
	TAILQ_INSERT_HEAD(head, t, times);
}

void grabtime_stop(timelist *head)
{
	if (verbose_time == false) {
		return;
	}

	struct time *t = TAILQ_FIRST(head);

	if (t->complete == true) {
		TAILQ_FOREACH(t, head, times)
		{
			if (t->complete != true) {
				clock_gettime(CLOCK_MONOTONIC_RAW, &t->rawstop);
				clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t->procstop);
				t->complete = true;
			}
		}
	} else {
		clock_gettime(CLOCK_MONOTONIC_RAW, &t->rawstop);
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t->procstop);
		t->complete = true;
	}
}

void print_time_stats(timelist *head)
{
	if (verbose_time == false) {
		return;
	}

	double delta = 0;
	struct time *t;

	fprintf(stderr, "\nRaw elapsed time stats:\n");
	TAILQ_FOREACH_REVERSE(t, head, timelist, times)
	{
		if (t->complete == true) {
			delta = t->rawstop.tv_sec - t->rawstart.tv_sec + (t->rawstop.tv_nsec / 1000000.0) - (t->rawstart.tv_nsec / 1000000.0);
			fprintf(stderr, "%.4f\tms: %s\n", delta, t->name);
		}
	}
	fprintf(stderr, "\nCPU process time stats:\n");
	while (!TAILQ_EMPTY(head)) {
		t = TAILQ_LAST(head, timelist);
		if (t->complete == true) {
			delta = t->procstop.tv_sec - t->procstart.tv_sec + (t->procstop.tv_nsec / 1000000.0) - (t->procstart.tv_nsec / 1000000.0);
			fprintf(stderr, "%.4f\tms: %s\n", delta, t->name);
		}
		TAILQ_REMOVE(head, t, times);
		free(t);
	}
}

static int set_default_value_from_path(char **global, const char *path)
{
	char line[LINE_MAX];
	FILE *file;
	char *c;
	char *rel_path;
	int ret = -1;

	string_or_die(&rel_path, "%s%s", path_prefix, path);

	file = fopen(rel_path, "r");
	if (!file) {
		free(rel_path);
		return ret;
	}

	/* the file should contain exactly one line */
	line[0] = 0;
	if (fgets(line, LINE_MAX, file) == NULL) {
		if (ferror(file)) {
			fprintf(stderr, "Error: Unable to read data from %s\n", rel_path);
		} else if (feof(file)) {
			fprintf(stderr, "Error: Contents of %s are empty\n", rel_path);
		}
		goto fail;
	}

	/* remove newline if present */
	c = strchr(line, '\n');
	if (c) {
		*c = '\0';
	}

	string_or_die(global, "%s", line);
	ret = 0;
fail:
	fclose(file);
	free(rel_path);
	return ret;
}

static int set_url(char **global, char *url, const char *path)
{
	int ret = 0;

	if (url) {
		if (*global) {
			free(*global);
		}
		string_or_die(global, "%s", url);
	} else {
		if (*global) {
			/* option passed on command line previously */
			return ret;
		} else {
			/* no option passed; use the default value */
			ret = set_default_value_from_path(global, path);
			return ret;
		}
	}

	return ret;
}

/* Initializes the content_url global variable. If the url parameter is not
 * NULL, content_url will be set to its value. Otherwise, the value is read
 * from the 'contenturl' configuration file.
 */
int set_content_url(char *url)
{
	if (content_url) {
		/* Only set once; we assume the first successful set is the best choice */
		return 0;
	}

	return set_url(&content_url, url, default_content_url_path);
}

/* Initializes the version_url global variable. If the url parameter is not
 * NULL, version_url will be set to its value. Otherwise, the value is read
 * from the 'versionurl' configuration file.
 */
int set_version_url(char *url)
{
	if (version_url) {
		/* Only set once; we assume the first successful set is the best choice */
		return 0;
	}

	return set_url(&version_url, url, default_version_url_path);
}

static bool is_valid_integer_format(char *str)
{
	unsigned long long int version;
	errno = 0;

	version = strtoull(str, NULL, 10);
	if ((errno < 0) || (version == 0)) {
		return false;
	}

	return true;
}

/* Initializes the state_dir global variable. If the path parameter is not
 * NULL, state_dir will be set to its value. Otherwise, the value is the
 * build-time default (STATE_DIR).
 */
bool set_state_dir(char *path)
{
	if (path) {
		if (path[0] != '/') {
			fprintf(stderr, "statepath must be a full path starting with '/', not '%c'\n", path[0]);
			return false;
		}

		if (state_dir) {
			free(state_dir);
		}

		string_or_die(&state_dir, "%s", path);
	} else {
		if (state_dir) {
			return true;
		}
		string_or_die(&state_dir, "%s", STATE_DIR);
	}

	return true;
}

/* Initializes the format_string global variable. If the userinput parameter is
 * not NULL, format_string will be set to its value, but only if it is a
 * positive integer or the special value "staging". Otherwise, the value is
 * read from the 'format' configuration file.
 */
bool set_format_string(char *userinput)
{
	int ret;

	if (format_string) {
		return true;
	}

	if (userinput) {
		// allow "staging" as a format string
		if ((strcmp(userinput, "staging") == 0)) {
			if (format_string) {
				free(format_string);
			}
			string_or_die(&format_string, "%s", userinput);
			return true;
		}

		// otherwise, expect a positive integer
		if (!is_valid_integer_format(userinput)) {
			return false;
		}
		if (format_string) {
			free(format_string);
		}
		string_or_die(&format_string, "%s", userinput);
	} else {
		/* no option passed; use the default value */
		ret = set_default_value_from_path(&format_string, default_format_path);
		if (ret < 0) {
			return false;
		}
		if (!is_valid_integer_format(format_string)) {
			return false;
		}
	}

	return true;
}

/* Initializes the path_prefix global variable. If the path parameter is not
 * NULL, path_prefix will be set to its value.
 * Otherwise, the default value of '/'
 * is used. Note that the given path must exist.
 */
bool set_path_prefix(char *path)
{
	struct stat statbuf;
	int ret;

	if (path != NULL) {
		int len;
		char *tmp;

		/* in case multiple -p options are passed */
		if (path_prefix) {
			free(path_prefix);
		}

		string_or_die(&tmp, "%s", path);

		/* ensure path_prefix is absolute, at least '/', ends in '/',
		 * and is a valid dir */
		if (tmp[0] != '/') {
			char *cwd;

			cwd = get_current_dir_name();
			if (cwd == NULL) {
				fprintf(stderr, "Unable to get current directory name (%s)\n", strerror(errno));
				free(tmp);
				return false;
			}

			free(tmp);
			string_or_die(&tmp, "%s/%s", cwd, path);

			free(cwd);
		}

		len = strlen(tmp);
		if (!len || (tmp[len - 1] != '/')) {
			char *tmp_old = tmp;
			string_or_die(&tmp, "%s/", tmp_old);
			free(tmp_old);
		}

		path_prefix = tmp;

	} else {
		if (path_prefix) {
			/* option passed on command line previously */
			return true;
		} else {
			string_or_die(&path_prefix, "/");
		}
	}
	ret = stat(path_prefix, &statbuf);
	if (ret != 0 || !S_ISDIR(statbuf.st_mode)) {
		fprintf(stderr, "Bad path_prefix %s (%s), cannot continue.\n",
			path_prefix, strerror(errno));
		return false;
	}

	return true;
}

/* Initializes the cert_path global variable. If the path parameter is not
 * NULL, cert_path will be set to its value. Otherwise, the default build-time
 * value is used (CERT_PATH). Note that only the first call to this function
 * sets the variable.
 */
#ifdef SIGNATURES
void set_cert_path(char *path)
{
	// Early exit if the function was called previously.
	if (cert_path) {
		return;
	}

	if (path) {
		string_or_die(&cert_path, "%s", path);
	} else {
		// CERT_PATH is guaranteed to be valid at this point.
		string_or_die(&cert_path, "%s", CERT_PATH);
	}
}
#else
void set_cert_path(char UNUSED_PARAM *path)
{
	return;
}
#endif

bool init_globals(void)
{
	int ret;

	gettimeofday(&start_time, NULL);

	ret = set_state_dir(NULL);
	if (!ret) {
		return false;
	}

	ret = set_path_prefix(NULL);
	/* a valid path prefix must be set to continue */
	if (!ret) {
		return false;
	}

	/* Set defaults with following order of preference:
	1. Runtime flags
	2. State dir configuration files
	3. Configure time settings

	Calling with NULL means use the default config file value
*/
	if (!set_format_string(NULL)) {
#ifdef FORMATID
		/* Fallback to configure time format_string if other sources fail */
		set_format_string(FORMATID);
#else
		fprintf(stderr, "Unable to determine format id. Use the -F option instead.\n");
		exit(EXIT_FAILURE);
#endif
	}

	/* Calling with NULL means use the default config file value */
	if (set_version_url(NULL)) {
#ifdef VERSIONURL
		/* Fallback to configure time version_url if other sources fail */
		ret = set_version_url(VERSIONURL);
#else
		/* We have no choice but to fail */
		ret = -1;
#endif
		if (ret) {
			fprintf(stderr, "\nDefault version URL not found. Use the -v option instead.\n");
			exit(EXIT_FAILURE);
		}
	}

	/* Calling set_() with NULL means load the default config file value */
	if (set_content_url(NULL)) {
#ifdef CONTENTURL
		/* Fallback to configure time content_url if other sources fail */
		ret = set_content_url(CONTENTURL);
#else
		/* We have no choice but to fail */
		ret = -1;
#endif
		if (ret) {
			fprintf(stderr, "\nDefault content URL not found. Use the -c option instead.\n");
			exit(EXIT_FAILURE);
		}
	}

	/* must set this global after version_url and content_url */
	set_local_download();

#ifdef SIGNATURES
	set_cert_path(NULL);
#endif

	return true;
}

void free_globals(void)
{
	/* freeing all globals and set ALL them to NULL
	 * to avoid memory corruption on multiple calls
	 * to swupd_init() */
	free(content_url);
	content_url = NULL;

	free(version_url);
	version_url = NULL;

	free(path_prefix);
	path_prefix = NULL;

	free(format_string);
	format_string = NULL;

	free(mounted_dirs);
	mounted_dirs = NULL;

	free(state_dir);
	state_dir = NULL;

	if (bundle_to_add != NULL) {
		free(bundle_to_add);
		bundle_to_add = NULL;
	}
}
