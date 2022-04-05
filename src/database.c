#include "common.h"
#include "config.h"
#include "class.h"
#include "logging.h"
#include <sqlite3.h>

#define DB_START    "BEGIN"
#define DB_ROLLBACK "ROLLBACK"
#define DB_COMMIT   "COMMIT"
#define DB_GET      "SELECT bookingid, name, date FROM BOOKINGS WHERE bookingid = ?" 
#define DB_ADD      "INSERT INTO bookings (username, bookingid, name, date, booked, slots) VALUES (?, ?, ?, ?, ?, ?)"

static sqlite3 *db = NULL;

LOGSET("database");

static int database_simple_exec(
    const char *sql)
{
  sqlite3_stmt *st = NULL;
  int rc;

  rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
  if (rc != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement \"%s\": %s", sql, sqlite3_errmsg(db));
    goto fail;
  }

  sqlite3_finalize(st);
  return 1;

fail:
  if (st)
    sqlite3_finalize(st);
  return 0;
}



void database_init(
    void)
{
  int rc;
  rc = sqlite3_open_v2(config_get_db_path(),
                       &db,
                       SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,
                       NULL);
  if (rc != SQLITE_OK) {
    ELOG(ERROR, "Cannot initialize database: %s (%s)", sqlite3_errmsg(db),
         config_get_db_path());
    exit(EXIT_FAILURE);
  }
  sqlite3_busy_timeout(db, 5000);
}


void database_destroy(
    void)
{
  assert(db);
  sqlite3_close_v2(db);
  db = NULL;
  ELOG(VERBOSE, "Database closed");
}


int database_start(
    void)
{
  ELOG(DEBUG, DB_START);
  return database_simple_exec(DB_START);
}


int database_rollback(void)
{
  ELOG(DEBUG, DB_ROLLBACK);
  return database_simple_exec(DB_ROLLBACK);
}


int database_commit(
    void)
{
  ELOG(DEBUG, DB_COMMIT);
  return database_simple_exec(DB_COMMIT);
}


class_t database_get(
    int classid)
{
  struct class cl, *ret = NULL;
  sqlite3_stmt *st = NULL;
  int rc;
  const char *v;
  char *p;

  class_init(&cl);

  rc = sqlite3_prepare_v2(db, DB_GET, -1, &st, NULL);
  if (rc != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement \"%s\": %s", DB_GET,
         sqlite3_errmsg(db));
    goto fail;
  }

  if (sqlite3_bind_int(st, 1, classid) != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement (bind) \"%s\": %s", DB_GET,
         sqlite3_errmsg(db));
    goto fail;
  }

  /* Fetch parameter */
  rc = sqlite3_step(st);

  if (rc == SQLITE_DONE) {
    /* No rows */
    goto fail;
  }
  else if (rc != SQLITE_ROW) {
    ELOG(WARNING, "Cannot execute SQL statement (step) \"%s\": %s", DB_GET,
         sqlite3_errmsg(db));
    goto fail;
  }

  /* Fill the object with data */
  cl.id = sqlite3_column_int(st, 0);
  if (cl.id <= 0) {
    ELOG(WARNING, "Cannot retrieve column bookingid in row \"%s\": %s", DB_GET,
        "bookingid invalid value");
    goto fail;
  }

  v = sqlite3_column_text(st, 1);
  cl.class_name = v ? strdup(v) : NULL;
  if (!cl.class_name) {
    ELOG(WARNING, "Cannot retrieve column name in row \"%s\"", DB_GET);
    goto fail;
  }

  v = sqlite3_column_text(st, 2);
  p = strptime(v, TIME_FORMAT, &cl.time);
  if (*p != 0) {
    ELOG(WARNING, "Cannot retrieve column date in row \"%s\"", DB_GET);
    goto fail;
  }

  /* Allocate result */
  ret = calloc(1, sizeof(struct class));
  if (!ret) {
    ELOGERR(WARNING, "Cannot copy result");
    goto fail;
  }
  memcpy(ret, &cl, sizeof(struct class));

  sqlite3_finalize(st);
  return ret;

fail:
  class_destroy(&cl);

  if (st)
    sqlite3_finalize(st);
  return NULL;
}


int database_add(
    class_t cl)
{
  sqlite3_stmt *st = NULL;
  char classtime[128] = {0};
  char booktime[128] = {0};
  time_t tnow = time(NULL);
  struct tm *now = localtime(&tnow);
  int rc;

  rc = sqlite3_prepare_v2(db, DB_ADD, -1, &st, NULL);
  if (rc != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement \"%s\": %s", DB_ADD,
         sqlite3_errmsg(db));
    goto fail;
  }

  /* Bind values */
  /* Username */
  if (sqlite3_bind_text(st, 1, config_get_login(), -1, NULL) != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement (bind) \"%s\": %s", DB_ADD,
         sqlite3_errmsg(db));
    goto fail;
  }

  /* Booking ID */
  if (sqlite3_bind_int(st, 2, cl->id) != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement (bind) \"%s\": %s", DB_ADD,
         sqlite3_errmsg(db));
    goto fail;
  }

  /* Name */
  if (sqlite3_bind_text(st, 3, cl->class_name, -1, NULL) != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement (bind) \"%s\": %s", DB_ADD,
         sqlite3_errmsg(db));
    goto fail;
  }

  /* Class time */
  strftime(classtime, 127, "%Y-%m-%d %H:%M:%S", &cl->time);
  if (sqlite3_bind_text(st, 4, classtime, -1, NULL) != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement (bind) \"%s\": %s", DB_ADD,
         sqlite3_errmsg(db));
    goto fail;
  }

  /* Book time */
  strftime(booktime, 127, "%Y-%m-%dT%H:%M:%S.000000", now);
  if (sqlite3_bind_text(st, 5, booktime, -1, NULL) != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement (bind) \"%s\": %s", DB_ADD,
         sqlite3_errmsg(db));
    goto fail;
  }

  /* Slots */
  if (sqlite3_bind_int(st, 6, cl->slots_available) != SQLITE_OK) {
    ELOG(WARNING, "Cannot execute SQL statement (bind) \"%s\": %s", DB_ADD,
         sqlite3_errmsg(db));
    goto fail;
  }

  /* Fetch parameter */
  rc = sqlite3_step(st);
  if (rc != SQLITE_DONE) {
    ELOG(WARNING, "Cannot execute SQL statement (step) \"%s\": %s", DB_ADD,
         sqlite3_errmsg(db));
    goto fail;
  }

  sqlite3_finalize(st);
  return 1;


fail:
  if (st)
    sqlite3_finalize(st);
  return 0;
}
