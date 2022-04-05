#ifndef _CLASS_H_
#define _CLASS_H_

typedef struct class * class_t;
typedef struct class class;

LIST_HEAD(class_list, class);

typedef struct class_list * class_list_t;

struct class {
  char *entry_name;
  char *class_name;
  int id;
  int clubid;
  int resourceid;
  int slots_available;
  int waitslots_available;
  bool booked;
  bool waiting;
  struct tm time;
  LIST_ENTRY(class) l;
};

void class_init(class_t cl);
void class_destroy(class_t cl);
void class_free_timetable(class_list_t he);

int class_compare(class_t a, class_t b);
char * class_print(class_t cl);
class_t class_dup(class_t in);
#endif
