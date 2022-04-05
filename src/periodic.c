#include "common.h"
#include "config.h"
#include "logging.h"
#include "website.h"
#include "bookings.h"
#include "periodic.h"
#include <ev.h>

LOGSET("periodic");

static ev_timer tz = {0};
static ev_periodic pe = {0};
static int periodic_timer_adjustment = 0;

static void log_next_wakeup(void);
static void calibrate_periodic_timer(EV_P_ ev_timer *w, int revents);
static void check_bookings_event(EV_P_ ev_periodic *w,  int revents);



static void log_next_wakeup(
    void)
{
  struct tm tm;
  char timestr[48] = {0};
  time_t next = (time_t)ev_periodic_at(&pe);

  localtime_r(&next, &tm);
  strftime(timestr, 48, TIME_FORMAT " %Z", &tm);

  ELOG(INFO, "Next wake up is at: %s", timestr);
}


static void calibrate_periodic_timer(
    EV_P_ ev_timer *w,
    int revents)
{
  /* Fetch the current time difference reported by website */
  int td = website_server_time_diff();
  td -= periodic_timer_adjustment;

  /* Something is horribly wrong if we reach this */
  if (abs(td) > 86400) {
    ELOG(ERROR, "Detected timeskew is over a day! (%d seconds). "
                "May god have mercy on your soul..", td);
    return;
  }

  /* If, when accounting for our periodic time adjustment the 
   * skew is greater than 5 seconds, then reset our periodic timer to
   * compensate */
  if (abs(td) > 5) {
    ELOG(WARNING, "Detected timeskew of %d seconds. "
                  "Adjusting periodic timer to compensate.", td);

    periodic_reset();
  }
}

static void check_bookings_event(
    EV_P_ ev_periodic *w,
    int revents)
{
  bookings_check();
}



void periodic_init(
    void)
{
  /* Load the periodic timer */
  struct tm *wake = config_get_waketime();
  time_t waket = mktime(wake) % 86400;

  /* Arm the periodical timer */
  ev_periodic_init(&pe, check_bookings_event, (ev_tstamp)waket, 86400.0, 0);
  ev_periodic_start(EV_DEFAULT, &pe);
  log_next_wakeup();

  /* Arm the timezone adjustment poller */
  ev_timer_init(&tz, calibrate_periodic_timer, 10.0, TIMEZONE_CHECK);
  ev_timer_start(EV_DEFAULT, &tz);
}


void periodic_reset(
    void)
{
  /* Fetch the current time difference reported by website */
  int td = website_server_time_diff();
  struct tm *wake = config_get_waketime();
  time_t waket = mktime(wake) % 86400;

  /* Add our skew */
  waket += td;

  /* Re-arm the timer */
  ev_periodic_stop(EV_DEFAULT, &pe);
  ev_periodic_set(&pe, (float)waket, 86400.0, 0);
  ev_periodic_start(EV_DEFAULT, &pe);
  log_next_wakeup();

  periodic_timer_adjustment = website_server_time_diff();
  log_next_wakeup();
}


void periodic_destroy(
    void)
{
  ev_timer_stop(EV_DEFAULT, &tz);
  ev_periodic_stop(EV_DEFAULT, &pe);
  ELOG(VERBOSE, "Stopped periodic wakeup");
}
