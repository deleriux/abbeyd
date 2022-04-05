#include "common.h"
#include "signals.h"
#include "bookings.h"
#include "logging.h"
#include "config.h"
#include <ev.h>

LOGSET("bookings");

static ev_signal intr = {0};
static ev_signal usr1 = {0};
static ev_signal term = {0};
static ev_signal hup = {0};

static void recheck_bookings_event(EV_P_ ev_signal *w, int revents);
static void end_service_event(EV_P_ ev_signal *w, int revents);
static void reload_config_event(EV_P_ ev_signal *w, int revents);



static void recheck_bookings_event(
    EV_P_ ev_signal *w,
    int revents)
{
   ELOG(INFO, "Received signal to recheck bookings");
   bookings_check();
}

static void end_service_event(
    EV_P_ ev_signal *w,
    int revents)
{
  ELOG(INFO, "Received signal to terminate");
  ev_break(EV_A_ EVBREAK_ALL);
}

static void reload_config_event(
    EV_P_ ev_signal *w,
    int revents)
{
  ELOG(INFO, "Received signal to reload config file");
  config_reload();
}




void signals_init(
    void)
{
  ev_signal_init(&usr1, recheck_bookings_event, SIGUSR1);
  ev_signal_init(&hup, reload_config_event, SIGHUP);
  ev_signal_init(&term, end_service_event, SIGTERM);
  ev_signal_init(&intr, end_service_event, SIGINT);

  ev_signal_start(EV_DEFAULT, &usr1);
  ev_signal_start(EV_DEFAULT, &hup);
  ev_signal_start(EV_DEFAULT, &term);
  ev_signal_start(EV_DEFAULT, &intr);
}

void signals_destroy(
    void)
{
  ev_signal_stop(EV_DEFAULT, &usr1);
  ev_signal_stop(EV_DEFAULT, &hup);
  ev_signal_stop(EV_DEFAULT, &term);
  ev_signal_stop(EV_DEFAULT, &intr);
}
