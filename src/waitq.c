#include "common.h"
#include "config.h"
#include "database.h"
#include "website.h"
#include "waitq.h"
#include "logging.h"
#include <ev.h>

LOGSET("waitq");

static ev_timer timer;
static struct class_list head;
static int list_size = 0;

static void rebook_waitlist(
    EV_P_ ev_timer *w,
    int revents)
{
  ELOG(VERBOSE, "Checking rebookings");
  bool commit = false;
  class_t cl, ne;
  cl = LIST_FIRST(&head);

  if (!database_start())
    return;

  /* Iterate the list trying to book each entry */
  while (cl) {
    ne = LIST_NEXT(cl, l);

    /* Remove from queue if we booked it */
    if (website_book(cl)) {
      commit = true;
      database_add(cl);

      ELOG(INFO, "%s has been booked", class_print(cl));

      LIST_REMOVE(cl, l);
      list_size--;
      ELOG(VERBOSE, "Number of bookings left: %d", list_size);
      class_destroy(cl);
      free(cl);
    }
    else {
      ELOG(VERBOSE, "%s booking failed: %s", class_print(cl), website_errbuf());
    }
    cl = ne;
  }

  /* Disable if theres nothing to wait on */
  if (list_size <= 0) {
    ELOG(INFO, "No more waitlist bookings. Stopped rebooker");
    ev_timer_stop(EV_A_ w);
  }

  if (commit) {
    database_commit();
    website_commit();
  }
  else {
    database_rollback();
  }
}


static bool waitq_exists(
    class_t cl)
{
  class_t en;
  LIST_FOREACH(en, &head, l) {
    if (en->id == cl->id)
      return true;
  }

  return false;
}


bool waitq_add(
    class_t in)
{
  if (waitq_exists(in))
    return false;

  ELOG(VERBOSE, "Adding entry to waitq");

  class_t cl = class_dup(in);
  if (!cl) 
    return false;

  LIST_INSERT_HEAD(&head, cl, l);
  if (list_size == 0)
    ev_timer_start(EV_DEFAULT, &timer);
  list_size++;

  return true;
}


void waitq_flush(
    void)
{
  class_t cl, ne;
  ELOG(VERBOSE, "Flushing wait queue");

  ev_timer_stop(EV_DEFAULT, &timer);

  cl = LIST_FIRST(&head);
  while (cl) {
    ne = LIST_NEXT(cl, l);

    LIST_REMOVE(cl, l);
    list_size--;
    class_destroy(cl);
    free(cl);

    cl = ne;
  }
}


void waitq_init(
    void)
{
  ELOG(VERBOSE, "Initializing");
  float retry = (float)config_get_waitlist_timeout();
  ev_timer_init(&timer, rebook_waitlist, retry, retry);
  ev_set_priority(&timer, EV_MINPRI);
  LIST_INIT(&head);
}
