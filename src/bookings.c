#include "common.h"
#include "config.h"
#include "logging.h"
#include "periodic.h"
#include "website.h"
#include "class.h"
#include "waitq.h"
#include "database.h"
#include "bookings.h"
#include <ev.h>

LOGSET("bookings");

#define BOOKINGS_RETRY 180.0
#define BOOKINGS_RETRY_MAX 20

static ev_timer rb = {0};

static void stop_rebooker(void);
static void start_rebooker(void);
static void recheck_bookings_event(EV_P_ ev_timer *w, int revents);



static void recheck_bookings_event(
    EV_P_ ev_timer *w, 
    int revents)
{
  bookings_check();
}

static void start_rebooker(
    void)
{
  /* Dont start an active timer */
  if (ev_is_active(&rb))
    return;

  ev_timer_init(&rb, recheck_bookings_event, BOOKINGS_RETRY, BOOKINGS_RETRY);
  ev_timer_start(EV_DEFAULT, &rb);
}

static void stop_rebooker(
    void)
{
  if (ev_is_active(&rb))
    ev_timer_stop(EV_DEFAULT, &rb);
}


void bookings_check(
    void)
{
  static int bookings_retry_counter = 0;
  class_list_t ttwe = website_get_timetable(config_get_max_days());
  class_list_t ttco = config_get_classes();
  class_t co = LIST_FIRST(ttco);
  class_t db = NULL;
  class_t we;
  bool commit = false;

  if (!ttwe) {
    ELOG(ERROR, "Unable to get timetable");
    /* Retry the timetable every BOOKINGS_RETRY seconds until this eventually works */
    bookings_retry_counter++;
    if (bookings_retry_counter < BOOKINGS_RETRY_MAX) {
      start_rebooker();
    }
    else {
      ELOG(CRITICAL, "Failed to get timetable %d times. Giving up. Exiting!", bookings_retry_counter);
      exit(EXIT_FAILURE);
    }
    return; 
  }

  /* We got there eventually. Reset the counter and periodic timer */
  stop_rebooker();

  if (!ttco)
    return;

  if (!database_start())
    return;

  /* Suspend and flush the wait queue at this point */
  waitq_flush();

  /* Loop over each config entry */
  while (co) {
    we = LIST_FIRST(ttwe);

    /* Loop over each website timetable entry */
    while (we) {
      if (class_compare(co, we) == 0) {
        /* Dont attempt to book on a class we are already booked on */
        if (we->booked) {
          goto next;
        }

        /* Dont attempt to book on a class we previously cancelled */
        if ((db = database_get(we->id))) {
          ELOG(INFO, "%s already in the database", class_print(we));
          class_destroy(db);
          free(db);
          db = NULL;
          goto next;
        }

        /* Dont book items that cost money */
        if (website_price(we) > 0.) {
          ELOG(INFO, "%s has a price. Not booking", class_print(we));
          goto next;
        }

        /* If no slots are available */
        if (we->slots_available <= 0) {
          /* And not on the waiting list */
          if (!we->waiting) {
            website_wait(we);
            ELOG(INFO, "%s is full. Put onto waiting list and will attempt to rebook", 
                   class_print(we));
          }
          if (waitq_add(we)) {
            ELOG(INFO, "%s is scheduled to rebook on the waiting list", 
                 class_print(we));
          }
        }
        else {
          /* Book the class and update the db */
          if (!website_book(we)) {
            ELOG(INFO, "%s could not be booked: %s", class_print(we), website_errbuf());
          }
          else {
            commit = true;
            database_add(we);
            ELOG(INFO, "%s has been booked", class_print(we));
          }
        }
      }
    next:
      we = LIST_NEXT(we, l);
    }
    co = LIST_NEXT(co, l);
  }

  class_free_timetable(ttwe);
  free(ttwe);

  if (commit) {
    if (!website_commit()) {
      database_rollback();
    }
    else {
      database_commit();
    }
  }
  else {
    database_rollback();    
  }
}

