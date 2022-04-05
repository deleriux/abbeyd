#include "common.h"
#include "config.h"
#include "waitq.h"
#include "database.h"
#include "website.h"
#include "logging.h"
#include "periodic.h"
#include "bookings.h"
#include <ev.h>

LOGSET("abbeyd")

#define DEFAULT_CONFIG "/etc/abbeyconfig.ini"

int main(
    int argc,
    char **argv)
{
  char *configfile = NULL;

  if (argc < 2)
    configfile = DEFAULT_CONFIG;
  else
    configfile = argv[1];

  config_parse(configfile);
  database_init();
  website_init();
  periodic_init();
  waitq_init();
  signals_init();

  bookings_check();

  ev_run(EV_DEFAULT, 0);

  ELOG(VERBOSE, "Exited main loop");

  waitq_flush();
  signals_destroy();
  periodic_destroy();
  database_destroy();
  website_destroy();
  config_unload();

  ELOG(INFO, "Service is finished");
  return 0;
}
