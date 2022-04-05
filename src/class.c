#include "common.h"
#include "class.h"
#include "logging.h"

LOGSET("class");

static char classbuf[1024];

void class_init(
     class_t cl)
{
  ELOG(DEBUG, "Initializing");
  assert(cl);

  memset(cl, 0, sizeof(struct class));
  cl->id = -1;
}

void class_destroy(
    class_t cl)
{
  ELOG(DEBUG, "Destroy");
  if (!cl)
    return;

  if (cl->entry_name)
    free(cl->entry_name);

  if (cl->class_name)
    free(cl->class_name);
}

void class_free_timetable(
    class_list_t he)
{
  ELOG(DEBUG, "Freeing timetable");
  class_t cl, ne;

  if (!he)
    return;

  cl = LIST_FIRST(he);
  while (cl) {
    ne = LIST_NEXT(cl, l);

    if ((cl)->l.le_next != NULL)
      (cl)->l.le_next->l.le_prev = (cl)->l.le_prev;
    *(cl)->l.le_prev = (cl)->l.le_next;

    class_destroy(cl);
    free(cl);

    cl = ne;
  }
}


int class_compare(
    class_t a,
    class_t b)
{
  assert(a);
  assert(b);

  ELOG(DEBUG, "Comparing %s / %s\n", a->class_name, b->class_name);

  if (strcmp(a->class_name, b->class_name) != 0)
    return -1;

  if (a->time.tm_wday != b->time.tm_wday)
    return 1;

  if (a->time.tm_min != b->time.tm_min)
    return 2;

  if (a->time.tm_hour != b->time.tm_hour)
    return 3;

  return 0;
}


class_t class_dup(
    class_t in)
{
  assert(in);

  ELOG(DEBUG, "Duplicating class");

  class_t cl = NULL;

  cl = calloc(1, sizeof(struct class));
  if (!cl) {
    ELOGERR(WARNING, "Cannot allocate class");
    return NULL;
  }

  cl->entry_name = in->entry_name ? strdup(in->entry_name) : NULL;
  cl->class_name = in->class_name ? strdup(in->class_name) : NULL;
  cl->id = in->id;
  cl->clubid = in->clubid;
  cl->resourceid = in->resourceid;
  cl->slots_available = in->slots_available;
  cl->waitslots_available = in->waitslots_available;
  cl->booked = in->booked;
  cl->waiting = in->waiting;
  memcpy(&cl->time, &in->time, sizeof(struct tm));

  return cl;
}


char * class_print(
    class_t cl)
{
  char timebuf[64] = {0};
  strftime(timebuf, 64, "%a %d %b %H:%M %Y", &cl->time);
  memset(classbuf, 0, sizeof(classbuf));
  snprintf(classbuf, 1023, "%s at %s", cl->class_name, timebuf);
  return classbuf;
}
