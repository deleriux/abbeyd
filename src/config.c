#include "common.h"
#include "dictionary.h"
#include "iniparser.h"
#include "config.h"
#include "class.h"
#include "database.h"
#include "website.h"
#include "logging.h"
#include "periodic.h"
#include <pwd.h>
#include <grp.h>
#include <ev.h>
#include <sys/inotify.h>
#include <libgen.h>
#include <limits.h>

LOGSET("config");

#define MAX_WATCHES              512

#define DEFAULT_LOCATION         "Newmarket"
#define DEFAULT_DB_PATH          "/var/lib/abbeybooker/bookings.db"
#define DEFAULT_MAX_DAYS         8
#define DEFAULT_COOKIES          ""
#define DEFAULT_WAITLIST_TIMEOUT 50 
#define DEFAULT_VERBOSE          0
#define DEFAULT_WAKETIME         "00:00:05"
#define DEFAULT_LOGFILE          "stderr"

struct config {
  char *path;
  char *db_path;
  char *location;
  char *login;
  char *pass;
  char *cookies;
  char *logfile;
  int max_days;
  int num_classes;
  int waitlist_retry_timeout;
  int verbose;
  struct class_list *classes;
  struct tm waketime;
  int ifd;
  int wd[2];
  ev_io io;
} config = {0};

static char keybuf[512];

static char * mk(char *section, char *key);
static void load_defaults(struct config *config);
static void switch_error_log(char *filename);
static int dayofweek(char *dow);
static bool parse_main(struct config *config, dictionary *d);
static bool parse_class(struct config *conf, dictionary *d, char *secname);
static void update_config(struct config *new);
static bool reread_config(void);
static void config_changed_event(EV_P_ ev_io *w, int revents);
static void monitor_config_file(const char *config_path);

static void load_defaults(
    struct config *config)
{
  char *p = NULL;

  /* Set the defaults */
  config->path = NULL;
  config->db_path = strdup(DEFAULT_DB_PATH);
  config->location = strdup(DEFAULT_LOCATION);
  config->max_days = DEFAULT_MAX_DAYS;
  config->login = NULL;
  config->pass = NULL;
  config->logfile = strdup(DEFAULT_LOGFILE);
  config->cookies = strdup(DEFAULT_COOKIES);
  config->waitlist_retry_timeout = DEFAULT_WAITLIST_TIMEOUT;
  config->verbose = DEFAULT_VERBOSE;
  config->ifd = -1;
  config->wd[0] = -1;
  config->wd[1] = -1;

  assert(config->db_path);
  assert(config->location);
  assert(config->cookies);
  assert(config->logfile);

  p = strptime(DEFAULT_WAKETIME, "%H:%M:%S", &config->waketime);
  assert(p && *p == 0);
}

static char * mk(
    char *section,
    char *key)
{
  assert(section && key);

  memset(keybuf, 0, sizeof(keybuf));
  snprintf(keybuf, 511, "%s:%s", section, key);
  return keybuf;
}


static void switch_error_log(
     char *filename)
{
  ELOG(INFO, "Switching to logfile %s", filename);
  if (!freopen(filename, "a+", stderr)) {
    ELOGERR(CRITICAL, "Cannot switch log file to %s", filename);
  }

  ELOG(INFO, "Logging to file %s", filename);
  fflush(stderr);
  setlinebuf(stderr);
}


static int dayofweek(
    char *dow)
{
  int i, l;
  char dayofweek[512] = {0};
  l = strlen(dow);

  if (l > 32 || l < 3)
    return -1;

  for (i=0; i < 3; i++)
    dayofweek[i] = tolower(dow[i]);

  if (strcmp(dayofweek, "sun") == 0)
    return 0;

  else if (strcmp(dayofweek, "mon") == 0)
    return 1;

  else if (strcmp(dayofweek, "tue") == 0)
    return 2;

  else if (strcmp(dayofweek, "wed") == 0)
    return 3;

  else if (strcmp(dayofweek, "thu") == 0)
    return 4;

  else if (strcmp(dayofweek, "fri") == 0)
    return 5;

  else if (strcmp(dayofweek, "sat") == 0)
    return 6;

  return -1;
}

static bool parse_main(
    struct config *config,
    dictionary *d)
{
  char *val = NULL;
  char *p;

  /* Database Path */
  val = iniparser_getstring(d, mk("main", "db_path"), NULL);
  if (val) {
    free(config->db_path);
    config->db_path = strdup(val);
    if (!config->db_path) {
      ELOG(ERROR, "Cannot set \"db_path\" in [main]");
      return false;
    }
    val = NULL;
  }

  val = iniparser_getstring(d, mk("main", "location"), NULL);
  if (val) {
    free(config->location);
    config->location = strdup(val);
    if (!config->location) {
      ELOG(ERROR, "Cannot set \"location\" in [main]");
      return false;
    }
    val = NULL;
  }

  val = iniparser_getstring(d, mk("main", "cookies"), NULL);
  if (val) {
    free(config->cookies);
    config->cookies = strdup(val);
    if (!config->cookies) {
      ELOG(ERROR, "Cannot set \"cookies\" in [main]");
      return false;
    }
    val = NULL;
  }

  val = iniparser_getstring(d, mk("main", "logfile"), NULL);
  if (val) {
    free(config->logfile);
    config->logfile = strdup(val);
    if (!config->logfile) {
      ELOG(ERROR, "Cannot set \"logfile\" in [main]");
      return false;
    }
    val = NULL;
  }

  val = iniparser_getstring(d, mk("main", "waketime"), NULL);
  if (val) {
    p = strptime(val, "%H:%M:%S", &config->waketime);
    if (!p || *p != 0) {
      ELOG(ERROR, "\"waketime\" field in [main] was invalid (hh:mm:ss)");
      return false;
    }

    val = NULL;
  }

  config->login = iniparser_getstring(d, mk("main", "login"), NULL);
  config->pass = iniparser_getstring(d, mk("main", "password"), NULL);

  if (!config->login) {
    ELOG(ERROR, "\"login\" field in [main] was not found");
    return false;
  }

  if (!config->pass) {
    ELOG(ERROR, "\"password\" field in [main] was not found");
    return false;
  }

  config->pass = strdup(config->pass);
  config->login = strdup(config->login);
  config->max_days = iniparser_getint(d, mk("main", "max_days"), DEFAULT_MAX_DAYS);
  config->waitlist_retry_timeout = 
                    iniparser_getint(d, mk("main", "waiting_list_retry_timeout"), 
                                                        DEFAULT_WAITLIST_TIMEOUT);
  config->verbose = iniparser_getint(d, mk("main", "verbose"), DEFAULT_VERBOSE);

  return true;
}

static bool parse_class(
    struct config *conf,
    dictionary *d,
    char *secname)
{
  char *p;
  struct tm tmp = {0};
  char *tstr = NULL;
  class_t cl = calloc(1, sizeof(struct class));
  if (!cl) {
    ELOG(ERROR, "Cannot make class for section [%s]", secname);
    return false;
  }
  class_init(cl);

  /* Entry name */
  cl->entry_name = strdup(secname);
  if (!cl->entry_name) {
    ELOG(ERROR, "Cannot create entry name for section [%s]", secname);
    return false;
  }

  /* Class name */
  tstr = iniparser_getstring(d, mk(secname, "name"), NULL);
  if (!tstr) {
    ELOG(ERROR, "Cannot create class name for section [%s]", secname);
    return false;
  }

  cl->class_name = strdup(tstr);
  if (!cl->class_name) {
    ELOG(ERROR, "Cannot create class name for section [%s]", secname);
    return false;
  }
  tstr = NULL;

  /* Get day of week */
  tstr = iniparser_getstring(d, mk(secname, "day"), NULL);
  if (!tstr) {
    ELOG(ERROR, "Incorrect day of week defined in section [%s]", secname);
    return false;
  }

  cl->time.tm_wday = dayofweek(tstr);
  if (cl->time.tm_wday < 0) {
    ELOG(ERROR, "Incorrect day of week defined in section [%s]", secname);
    return false;
  }
  tstr = NULL;

  /* Get the time of day */
  tstr = iniparser_getstring(d, mk(secname, "time"), NULL);
  if (!tstr) {
    ELOG(ERROR, "Incorrect time defined in section [%s]", secname);
    return false;
  }

  p = strptime(tstr, "%H:%M", &tmp);
  if (!p || *p != 0) {
    ELOG(ERROR, "Incorrect time defined in section [%s]", secname);
    return false;
  }
  cl->time.tm_min = tmp.tm_min;
  cl->time.tm_hour = tmp.tm_hour;

  /* Append to head of queue */
  LIST_INSERT_HEAD(conf->classes, cl, l);

  conf->num_classes++;
  return true;
}

static void update_config(
    struct config *new)
{

  free(config.path);
  free(config.db_path);
  free(config.location);
  free(config.login);
  free(config.pass);
  free(config.cookies);
  free(config.logfile);
  class_free_timetable(config.classes);

  config.path = new->path;
  config.db_path = new->db_path;
  config.location = new->location;
  config.login = new->login;
  config.pass = new->pass;
  config.cookies = new->cookies;
  config.logfile = new->logfile;
  config.max_days = new->max_days;
  config.num_classes = new->num_classes;
  config.waitlist_retry_timeout = new->waitlist_retry_timeout;
  config.verbose = new->verbose;
  config.classes = new->classes;

  memcpy(&config.waketime, &new->waketime, sizeof(struct tm));
}

static bool reread_config(
    void)
{
  int i, nsecs;
  char *section = NULL;
  dictionary *ini = NULL;
  struct config newconf = {0};

  newconf.classes = calloc(1, sizeof(struct class_list));
  if (!newconf.classes) {
    ELOG(ERROR, "Cannot allocate memory for class list");
    goto fail;
  }
  LIST_INIT(newconf.classes);

  assert(config.path);
  /* Load the INI file */
  ini = iniparser_load(config.path);
  if (!ini) {
    ELOG(ERROR, "INI file parsing error");
    goto fail;
  }

  if ((nsecs = iniparser_getnsec(ini)) < 1) {
    ELOG(ERROR, "Config file contains no [main] section. Aborting.");
    goto fail;
  }

  for (i=0; i < nsecs; i++) {
    section = iniparser_getsecname(ini, i);
    if (strcmp(section, "main") == 0) {
      if (!parse_main(&newconf, ini))
        goto fail;
    }
    else {
      if (!parse_class(&newconf, ini, section))
        goto fail;
    }
  }

  newconf.path = strdup(config.path);
  if (!newconf.path) {
    ELOG(ERROR, "Cannot set config path in config object");
    goto fail;
  }

  /* Overwrite old config */
  update_config(&newconf);

  /* Do post steps */
  if (strcmp(config.logfile, "stderr") != 0) {
    switch_error_log(config.logfile);
  }

  if (config.verbose)
    log_setlevel(VERBOSE);
  else
    log_setlevel(INFO);

  /* Fix the database */
  database_destroy();
  database_init();

  /* Fix the cookie path */
  website_update_config();

  /* Reset the periodic wake time */
  periodic_reset();

  iniparser_freedict(ini);
  return true;

fail:
  if (newconf.path)
    free(newconf.path);
  if (newconf.db_path)
    free(newconf.db_path);
  if (newconf.location)
    free(newconf.location);
  if (newconf.login)
    free(newconf.login);
  if (newconf.pass)
    free(newconf.pass);
  if (newconf.cookies)
    free(newconf.cookies);
  if (newconf.logfile)
    free(newconf.logfile);
  class_free_timetable(newconf.classes);
  iniparser_freedict(ini);
  return false;
}


static void config_changed(
    EV_P_ ev_io *w,
    int revents)
{
  unsigned char buf[4096] = {0};
  size_t len = 0;
  struct inotify_event *ev;
  char bn[4096] = {0};
  char *base;
  int rc;
  bool reread_conf = false;

  strncpy(bn, config.path, 4096);
  base = basename(bn);

  rc = read(config.ifd, buf, 4096);
  if (rc < 0)
    ELOG(ERROR, "Error reading inotify watcher");

  len = 0;
  ev = (struct inotify_event *)buf;

  while (len < rc) {
    len += sizeof(struct inotify_event) + ev->len;

    if (ev->mask & IN_CREATE) {
      ELOG(VERBOSE, "Reading Inotify CREATE for file watcher %d (%s want: %s)",
                    ev->wd, ev->name, base);
      if (strcmp(base, ev->name) == 0) {
        config.wd[1] = inotify_add_watch(config.ifd, config.path, 
                           IN_MODIFY|IN_DELETE_SELF|IN_MOVE_SELF);
        if (config.wd[1] < 0) {
          ELOGERR(WARNING, "Error adding watch of config file");
        }
        else {
          reread_conf = true;
        }
      }
    }

    if (ev->mask & IN_DELETE_SELF) {
      ELOG(VERBOSE, "Reading Inotify DELETE for file watcher %d",
                    ev->wd);

      if (ev->wd == config.wd[0])
        config.wd[0] = -1;
      else
        config.wd[1] = -1;
    }

    if (ev->mask & IN_MOVE_SELF) {
      ELOG(VERBOSE, "Reading Inotify MOVE_SELF for watcher %d",
                    ev->wd);

      if (inotify_rm_watch(config.ifd, ev->wd) < 0) {
        ELOG(ERROR, "Cannot remove watch on moved config file.");
      }
      else {
        if (ev->wd == config.wd[0])
          config.wd[0] = -1;
        else if (ev->wd == config.wd[1])
          config.wd[1] = -1;
        else
          ELOG(WARNING, "Watcher not found to remove from index");
      }
    }

    if (ev->mask & IN_MODIFY) {
      ELOG(VERBOSE, "Reading Inotify MODIFY for watcher %d",
                    ev->wd);

      reread_conf = true;
    }

    if (ev->mask & IN_MOVED_TO) {
      ELOG(VERBOSE, "Reading Inotify MOVED_TO for watcher %d: (%s want %s)",
                    ev->wd, ev->name, base);
      if (strcmp(base, ev->name) == 0) {
        if (inotify_add_watch(config.ifd, config.path, IN_MODIFY|IN_DELETE_SELF|IN_MOVE_SELF) < 0) {
          ELOGERR(WARNING, "Error adding watch of config file");
        }
        else {
          reread_conf = true;
        }
      }
    }


    ev = (struct inotify_event *)(buf + len);
  }

  if (reread_conf) {
    ELOG(INFO, "Detected configuration file change. Re-reading configuration");
    if (!reread_config())
      ELOG(ERROR, "Configuration file error. Config not applied");
  }
}

static void monitor_config_file(
    const char *config_path)
{
  struct stat st = {0};
  char tmp[PATH_MAX];
  char *dir = NULL;

  strncpy(tmp, config_path, PATH_MAX);

  config.ifd = inotify_init1(IN_CLOEXEC);
  if (config.ifd < 0)
    err(EXIT_FAILURE, "Cannot setup inotify FD");

  /* Check path is a file */
  if (lstat(config_path, &st) < 0)
    err(EXIT_FAILURE, "Cannot monitor config path");
  if (!S_ISREG(st.st_mode))
    errx(EXIT_FAILURE, "CAnnot monitor config path: %s is not a file", config_path);

  /* Fetch name of directory */
  dir = dirname(tmp);

  /* Setup watches */
  config.wd[0] = inotify_add_watch(config.ifd, dir,  IN_DELETE_SELF|IN_MOVE_SELF|IN_MOVED_TO|IN_CREATE);
  if (config.wd[0] < 0)
    err(EXIT_FAILURE, "Canot add monitor of directory %s", dir);
  ELOG(VERBOSE, "Monitoring config directory: %s", dir);

  config.wd[1] = inotify_add_watch(config.ifd, config_path, IN_MODIFY|IN_DELETE_SELF|IN_MOVE_SELF);
  if (config.wd[1] < 0)
    err(EXIT_FAILURE, "Cannot add monitor to file %s", config_path);  
  ELOG(VERBOSE, "Monitoring config file %s", config_path);

  /* Setup event handler */
  ev_io_init(&config.io, config_changed, config.ifd, EV_READ);
  ev_io_start(EV_DEFAULT, &config.io);
}

void config_parse(
    const char *path)
{
  int i, nsecs;
  char *section = NULL;
  dictionary *ini = NULL;
  struct config newconf = {0};

  newconf.classes = calloc(1, sizeof(struct class_list));
  if (!newconf.classes)
    err(EXIT_FAILURE, "Cannot allocate memory for class list");
  LIST_INIT(newconf.classes);

  load_defaults(&newconf);

  /* Load the INI file */
  ini = iniparser_load(path);
  if (!ini)
    exit(EXIT_FAILURE);

  config.path = strdup(path);
  if (!config.path)
    err(EXIT_FAILURE, "Invalid config file path");

  newconf.path = strdup(path);
  if (!newconf.path)
    err(EXIT_FAILURE, "Invalid config file path");

  if ((nsecs = iniparser_getnsec(ini)) < 1)
    err(EXIT_FAILURE, "Config file contains no [main] section. Aborting.");

  for (i=0; i < nsecs; i++) {
    section = iniparser_getsecname(ini, i);
    if (strcmp(section, "main") == 0) {
      if (!parse_main(&newconf, ini))
        exit(EXIT_FAILURE);
    }
    else {
      if (!parse_class(&newconf, ini, section))
        exit(EXIT_FAILURE);
    }
  }

  update_config(&newconf);

  /* Do post steps */
  if (strcmp(config.logfile, "stderr") != 0) {
    switch_error_log(config.logfile);
  }

  if (config.verbose)
    log_setlevel(VERBOSE);
  else
    log_setlevel(INFO);

  iniparser_freedict(ini);
  monitor_config_file(path);
  return;
}

char * config_get_login(
    void)
{
  return config.login;
}

char * config_get_password(
    void)
{
  return config.pass;
}

char * config_get_db_path(
    void)
{
  return config.db_path;
}

char * config_get_location(
    void)
{
  return config.location;
}

int config_get_num_classes(
    void)
{
  return config.num_classes;
}

int config_get_max_days(
    void)
{
  return config.max_days;
}

char * config_get_cookies(
    void)
{
  return config.cookies;
}

class_list_t config_get_classes(
    void)
{
  return config.classes;
}

int config_get_waitlist_timeout(
    void)
{
  return config.waitlist_retry_timeout;
}

int config_get_verbose(
    void)
{
  return config.verbose;
}

struct tm * config_get_waketime(
    void)
{
  static struct tm tm2 = {0};
  struct tm tm1 = {0};
  time_t time_sec = time(NULL);

  /* Get the time now */
  localtime_r(&time_sec, &tm1);

  /* Copy the waketime over the top of the localtime */
  tm1.tm_hour = config.waketime.tm_hour;
  tm1.tm_min = config.waketime.tm_min;
  tm1.tm_sec = config.waketime.tm_sec;

  /* Confirm that the specified time is now or later */
  time_sec = mktime(&tm1);
  if (time_sec < time(NULL)) // add a day if time has passed
    config.waketime.tm_mday++;

  /* Copy the time to the static result */
  memcpy(&tm2, &tm1, sizeof(struct tm));

  return &tm2;
}

void config_unload(
    void)
{
  int i;

  if (config.path)
    free(config.path);
  if (config.db_path)
    free(config.db_path);
  if (config.location)
    free(config.location);
  if (config.login)
    free(config.login);
  if (config.pass)
    free(config.pass);
  if (config.cookies)
    free(config.cookies);
  if (config.logfile)
    free(config.logfile);

  if (config.classes) {
    class_free_timetable(config.classes);
    free(config.classes);
  }

  ev_io_stop(EV_DEFAULT, &config.io);
  for (i=0; i < 2; i++) {
    if (config.wd[i] > -1)
      inotify_rm_watch(config.ifd, config.wd[i]);
  }
  if (config.ifd > -1)
    close(config.ifd);

  memset(&config, 0, sizeof(config));
  config.ifd = -1;
  config.wd[0] = -1;
  config.wd[1] = -1;

  ELOG(VERBOSE, "Config unloaded");
}

void config_reload(
    void)
{
  reread_config();
}
