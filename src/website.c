#include "common.h"
#include "config.h"
#include "class.h"
#include "website.h"
#include "logging.h"

#include <ev.h>
#include <json-c/json.h>
#include <curl/curl.h>

LOGSET("website");

#define WEBSITE_BASE "https://abbeycroft.legendonlineservices.co.uk"
#define WEBSITE_LOGIN "/enterprise/account/login"
#define WEBSITE_SENDLOGIN "/enterprise/account/processloginrequest"
#define WEBSITE_MEMBERDETAILS "/enterprise/cschome/getaccountdetails"
#define WEBSITE_CONFIGURATION "/enterprise/Timetable/Configuration"
#define WEBSITE_TIMETABLE "/enterprise/Timetable/GetClassTimeTable"
#define WEBSITE_SPORTSTT "/enterprise/BookingsCentre/SportsHallTimeTable"
// "https://abbeycroft.legendonlineservices.co.uk/enterprise/BookingsCentre/SportsHallTimeTable?Activities=84&Activities=79&BookingFacilities=7&Start=2021-12-06&End=2021-12-06
#define WEBSITE_WAIT "/enterprise/ClassWaitingList/AddToWaitingList"
#define WEBSITE_BOOK "/enterprise/Timetable/AddClassBookingToBasket"
#define WEBSITE_PRICE "/Enterprise/api/OnlineBookingPrice"
#define WEBSITE_COMMIT "/enterprise/cart/confirmbasket"
#define WEBSITE_LOGOUT "/enterprise/account/logout"
#define WEBSITE_LOCATIONS "/enterprise/filteredlocationhierarchy"
#define WEBSITE_CLUB "/enterprise/FacilityLocation"
#define WEBSITE_SUBTYPES "/enterprise/Bookings/ActivitySubTypeCategories"

#define RELOGIN_TIMER 900.0

static CURL *site;
static char *buffer = NULL;
static char errbuf[CURL_ERROR_SIZE] = {0};
static size_t bufsz = 0;
static int time_diff;
static int memberid = -1;
static int clubid = -1;
static int courtid = -1;
static int facilitylistid = -1;
static ev_timer relog;

static void reset_buffer(
    void)
{
  free(buffer);
  bufsz = 0;
  buffer = NULL;
}


static size_t curl_write(
    char *data,
    size_t size, 
    size_t nmemb, 
    void *userp)
{
   size_t sz = size * nmemb;
 
   buffer = realloc(buffer, bufsz + sz + 1);
   if (buffer == NULL) {
     ELOGERR(ERROR, "Cannot allocate buffer");
     exit(EXIT_FAILURE);
   }
 
   memcpy(&buffer[bufsz], data, sz);
   bufsz += sz;
   buffer[bufsz] = 0;
 
   return sz;
}

static size_t curl_header_write(
    char *data,
    size_t size,
    size_t nmemb,
    void *userp)
{
  time_t us, them = 0;
  int l = 0;
  char *val = NULL, *eos = NULL;
  char timestr[256] = {0};
  struct tm them_tm = {0}, us_tm = {0};

  /* Work with the date header only .. */ 
  if (strncasecmp(data, "Date: ", 6) != 0)
    goto fin;

  val = &data[6];
  l = strlen(val);
  /* Remove carriage-return+newline */
  val[l - 2] = 0; val[l - 1] = 0;

  strncpy(timestr, val, 128);

  /* Format the date and time and convert to epoch */
  eos = strptime(timestr, "%a,%n%d%n%b%n%Y%n%H:%M:%S", &them_tm);
  if (!eos) {
    ELOG(WARNING, "Could not parse date/time from server. Skipping.");
    goto fin;
  }
  them_tm.tm_isdst = 0;

  /* Fetch our time, removing any DST hints */
  us = time(NULL);
  localtime_r(&us, &us_tm);
  us_tm.tm_isdst = 0;

  us = mktime(&us_tm);
  them = mktime(&them_tm);

  time_diff = us-them;

fin:
  return size * nmemb;
}

static int json_parse_location(
    json_object *lo,
    const char *target)
{
  json_object *val = NULL, *val2 = NULL;
  const char *str;
  int i;
  int location_id = -1;

  /* Locate name key */
  if (!json_object_object_get_ex(lo, "Name", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"Name\"");
    goto fail;
  }

  /* If found, retrieve Id, otherwise find children */
  str = json_object_get_string(val);
  //ELOG(VERBOSE, "Found location \"%s\". Looking for location \"%s\"",
  //     str, target);
  if (strcmp(target, str) == 0) {
    /* Can return location ID now */
    if (!json_object_object_get_ex(lo, "Id", &val)) {
      ELOG(WARNING, "JSON error. Cannot find JSON key \"ID\"");
      goto fail;
    }

    location_id = json_object_get_int(val);
  }
  else {
    if (!json_object_object_get_ex(lo, "Children", &val)) {
      ELOG(WARNING, "JSON error. CAnnot find JSON key \"Children\"");
      goto fail;
    }

    /* Iterate through children array looking for location */
    for (i=0; i < json_object_array_length(val); i++) {
      val2 = json_object_array_get_idx(val, i);

      location_id = json_parse_location(val2, target);
      if (location_id >= 0) {
        break;
      }
    }
  }

fail:
  return location_id;
}

static int parse_json_location(
    char *buffer,
    const char *location)
{
  /* Setup the initial array */
  json_object *js, *val = NULL;
  js = json_tokener_parse(buffer);
  int loc_id = -1;
  int i;

  ELOG(VERBOSE, "Location children parsing");
  if (!js) {
    ELOG(WARNING, "JSON tokner cannot parse location array");
    goto fail;
  }

  /* Iterate the array and parse locations */
  for (i=0; i < json_object_array_length(js); i++) {
    val = json_object_array_get_idx(js, i);
    loc_id = json_parse_location(val, location);
    if (loc_id >= 0) {
      break;
    }
  }

fail:
  if (js)
    json_object_put(js);
  return loc_id;
}


static float parse_json_price(
    char *buffer)
{
  json_object *js, *val;
  js = json_tokener_parse(buffer);
  float price = -1.;

  ELOG(VERBOSE, "Price parsing");

  if (!js) {
    ELOG(WARNING, "JSON tokener Cannot parse price output");
    goto fail;
  }

  if (!json_object_object_get_ex(js, "FeeTotal", &val))
    goto fail;

  price = json_object_get_double(val);

  json_object_put(js);
  return price;

fail:
  if (js)
    json_object_put(js);
  return -1.0;
}


static class_t parse_json_class(
    json_object *js)
{
  assert(js);

  const char *v;
  char *p;
  json_object *val;
  class_t cl = calloc(1, sizeof(struct class));
  if (!cl) {
    ELOGERR(WARNING, "Cannot allocate class");
    goto fail;
  }
  class_init(cl);

  /* ID */
  if (!json_object_object_get_ex(js, "ActivityInstanceID", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"ActivityInstanceID\"");
    goto fail;
  }

  cl->id = json_object_get_int(val);
  if (cl->id <= 0) {
    ELOG(WARNING, "JSON error. Invalid key \"ActivityInstanceID\" %d", cl->id);
    goto fail;
  }

  /* Club ID */
  if (!json_object_object_get_ex(js, "FacilityLocationID", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"FacilityLocationID\"");
    goto fail;
  }

  cl->clubid = json_object_get_int(val);
  if (cl->clubid < 0) {
    ELOG(WARNING, "JSON error. Invalid key \"FacilityLocationID\" %d", cl->clubid);
    goto fail;
  }

  /* Resource ID */
  if (!json_object_object_get_ex(js, "ResourceScheduleId", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"ResourceScheduleId\"");
    goto fail;
  }

  cl->resourceid = json_object_get_int(val);
  if (cl->resourceid < 0) {
    ELOG(WARNING, "JSON error. Invalid key \"ResourceScheduleId\" %d",
         cl->resourceid);
    goto fail;
  }

  /* Slots Available */
  if (!json_object_object_get_ex(js, "AvailibleSlots", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"AvailibleSlots\"");
    goto fail;
  }

  cl->slots_available = json_object_get_int(val);
  if (cl->slots_available < 0) {
    ELOG(WARNING, "JSON error. Invalid key \"AvailibleSlots\" %d", 
         cl->slots_available);
    goto fail;
  }

  /* Waiting Slots Available */
  if (!json_object_object_get_ex(js, "WaitingListCapacity", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"WaitingListCapacity\"");
    goto fail;
  }

  cl->waitslots_available = json_object_get_int(val);
  if (cl->waitslots_available < 0) {
    ELOG(WARNING, "JSON error. Invalid key \"WaitingListCapacity\" %d", 
         cl->waitslots_available);
    goto fail;
  }

  /* Booked */
  if (!json_object_object_get_ex(js, "OnClass", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"OnClass\"");
    goto fail;
  }

  cl->booked = json_object_get_boolean(val);

  /* Waiting */
  if (!json_object_object_get_ex(js, "OnWaitingList", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"OnWaitingList\"");
    goto fail;
  }

  cl->waiting = json_object_get_boolean(val);

  /* Name */
  if (!json_object_object_get_ex(js, "ActivityName", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"ActivityName\"");
    goto fail;
  }

  v = json_object_get_string(val);
  if (!v || strlen(v) <= 0) {
    ELOG(WARNING, "JSON error. Invalid key \"ActivityName\", Nil value");
    goto fail;
  }

  cl->class_name = strdup(v);
  if (!cl->class_name) {
    ELOGERR(WARNING, "Allocation of class_name failed");
    goto fail;
  }

  /* Class time */
  if (!json_object_object_get_ex(js, "StartDatetime", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"StartDatetime\"");
    goto fail;
  }

  v = json_object_get_string(val);
  if (!v || strlen(v) <= 0) {
    ELOG(WARNING, "JSON error. Invalid key \"StartDatetime\", Nil value");
    goto fail;
  }

  p = strptime(v, "%Y-%m-%dT%H:%M:%S", &cl->time);
  if (!p || *p != 0) {
    ELOG(WARNING, "JSON error. Invalid key \"StartDatetime\", time format "
         "invalid: %s", v);
    goto fail;
  }

  return cl;

fail:
  if (cl) {
    class_destroy(cl);
    free(cl);
  }
  return NULL;
}

static int parse_json_success(
    char *buffer)
{
  json_object *json = json_tokener_parse(buffer);
  json_object *val;

  ELOG(DEBUG, "Parsing json");

  /* Check parser worked */
  if (!json) {
    ELOG(WARNING, "Success JSON token was unparseable");
    goto fail;
  }

  if (!json_object_object_get_ex(json, "Success", &val)) {
    ELOG(WARNING, "JSON error. Cannot find JSON key \"Success\"");
    goto fail;
  }

  if (!json_object_get_boolean(val)) {
    /* Copy the error reason out */
    if (!json_object_object_get_ex(json, "ErrorMessage", &val)) {
      ELOG(WARNING, "JSON error. Cannot find JSON key \"ErrorMessage\"");
      goto fail;
    }
    memset(errbuf, 0, sizeof(errbuf));
    strncpy(errbuf, json_object_get_string(val), CURL_ERROR_SIZE-1);
    goto fail;
  }

  json_object_put(json);
  return 1;

fail:
  json_object_put(json);
  return 0;
}


static char * create_json_credentials(
    char *username,
    char *password)
{ 
  json_object *value;
  json_object *creds = json_object_new_object();
  char *json;

  ELOG(DEBUG, "Json Credentials parsing");

  value = json_object_new_string(username);
  json_object_object_add(creds, "login.email", value);

  value = json_object_new_string(password);
  json_object_object_add(creds, "login.password", value);

  json = (char *)json_object_to_json_string_ext(creds, JSON_C_TO_STRING_PRETTY);
  json = strdup(json);
  if (!json) 
    ELOGERR(WARNING, "Creating json string failed");

  json_object_put(creds);
  return json;
}

static int website_get_category(
    int fac_id,
    const char *category)
{
  int i, cat_id = -1;
  json_object *json = NULL, *val, *val2;
  CURL *cu;
  CURLcode rc;
  char url[1024] = {0};
  const char *target;

  snprintf(url, 1024, "%s/%s?LocationIds=%d", WEBSITE_BASE, WEBSITE_SUBTYPES, fac_id);
  ELOG(VERBOSE, "Website Subtypes");

  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(ERROR, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc),
         errbuf);
    goto fail;
  }

  /* Attempt to parse result as a json buffer */
  json = json_tokener_parse(buffer);
  if (!json) {
    ELOG(WARNING, "JSON parse error. Cannot parse configuration buffer");
    goto fail;
  }

  /* Returns an array */
  for (i=0; i < json_object_array_length(json); i++) {
    val = json_object_array_get_idx(json, i);

    if (!json_object_object_get_ex(val, "Name", &val2)) {
      ELOG(ERROR, "Cannot find key \"Name\" in category object");  
      goto fail;
    }

    /* Find the matching named category */
    target = json_object_get_string(val2);
    if (strcmp(target, category) == 0) {
      if (!json_object_object_get_ex(val, "ResourceSubTypeCategoryId", &val2)) {
        ELOG(ERROR, "Cannot find key \"ResourceSubTypeCategoryId\" in category object");
        goto fail;
      }

      cat_id = json_object_get_int(val2);
      break;
    }
  }

fail:
  json_object_put(json);
  reset_buffer();
  curl_easy_cleanup(cu);
  return cat_id;
}

static int website_logout(
    void)
{
  CURL *cu;
  CURLcode rc;
  char url[1024] = {0};

  snprintf(url, 1024, "%s/%s", WEBSITE_BASE, WEBSITE_LOGOUT);
  ELOG(VERBOSE, "Website logout");

  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(ERROR, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc),
         errbuf);
    goto fail;
  }

  reset_buffer();
  curl_easy_cleanup(cu);
  return 1;

fail:
  reset_buffer();
  curl_easy_cleanup(cu);
  return 0;
}

static int website_login(
    void)
{
  CURL *cu;
  CURLcode rc;
  int redirects = 0;
  char url[1024] = {0};
  char *post = NULL;
  struct curl_slist *hdrs = NULL;
  snprintf(url, 1024, "%s/%s", WEBSITE_BASE, WEBSITE_LOGIN);

  ELOG(VERBOSE, "Website login");

  /* Fetch the initial URL to create a session cookie, or check
     if we are already logged on */
  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(ERROR, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc), 
         errbuf);
    goto fail;
  }

  /* Check if the URL did a redirect */
  rc = curl_easy_getinfo(cu, CURLINFO_REDIRECT_COUNT, &redirects);
  if (rc != CURLE_OK) {
    ELOG(WARNING, "Cannot get redirect count for login: %s, %s", 
         curl_easy_strerror(rc), errbuf);
    goto fail;
  }

  /* If we did redirect, our creds already work */
  if (redirects > 0) {
    ELOG(VERBOSE, "Cookie still valid.");
    goto fin;
  }

  ELOG(VERBOSE, "Cookie now invalid. Logging in again");
  reset_buffer();

  /* Configure content type as json */
  hdrs = curl_slist_append(hdrs, "Content-type: application/json");
  curl_easy_setopt(cu, CURLOPT_HTTPHEADER, hdrs);

  /* Set the new url */
  memset(url, 0, sizeof(url));
  snprintf(url, 1024, "%s/%s", WEBSITE_BASE, WEBSITE_SENDLOGIN);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  /* Create login output */
  post = create_json_credentials(config_get_login(), config_get_password());
  if (!post)
    goto fail;
  curl_easy_setopt(cu, CURLOPT_POSTFIELDS, post);

  /* Perform the URL and check result */
  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(ERROR, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc), 
         errbuf);
    goto fail;
  }

  /* Parse the output */
  if (!parse_json_success(buffer)) {
    ELOG(ERROR, "Login to website failed: %s", errbuf);
    exit(EXIT_FAILURE);
  }

fin:
  reset_buffer();
  free(post);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(cu);
  return 1;

fail:
  reset_buffer();
  if (hdrs)
    curl_slist_free_all(hdrs);
  if (post)
    free(post);
  curl_easy_cleanup(cu);
  return 0;
}

static int website_memberstate(
    void)
{
  CURL *cu;
  CURLcode rc;
  char url[1024] = {0};
  json_object *json = NULL, *val = NULL;

  ELOG(VERBOSE, "website locationids");

  /* Try to locate website locations */
  snprintf(url, 1024, "%s/%s", WEBSITE_BASE, WEBSITE_LOCATIONS);
  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(ERROR, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc), 
         errbuf);
    goto fail;
  }

  facilitylistid = parse_json_location(buffer, config_get_location());
  if (facilitylistid < 0) {
    ELOG(ERROR, "Cannot find location \"%s\" in list of locations available.",
         config_get_location());
    goto fail;
  }
  json_object_put(json);
  reset_buffer();

  /* Fetch the club ID */
  snprintf(url, 1024, "%s/%s?request=%d", WEBSITE_BASE, WEBSITE_CLUB, facilitylistid);
  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(ERROR, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc),
         errbuf);
    goto fail;
  }

  /* Attempt to parse result as a json buffer */
  json = json_tokener_parse(buffer);
  if (!json) {
    ELOG(WARNING, "JSON parse error. Cannot parse configuration buffer");
    goto fail;
  }

  /* Returns a singular array with one element */
  if (json_object_array_length(json) != 1) {
    ELOG(ERROR, "Expected a 1 element array for the club we are inspecting but got %d",
         json_object_array_length(json));
    goto fail;
  }
  val = json_object_array_get_idx(json, 0);
  clubid = json_object_get_int(val);
  if (clubid <= 0) {
    ELOG(ERROR, "ClubId must be zero or above");
    goto fail;
  }
  json_object_put(json);
  reset_buffer(); json = NULL;

//  courtid = website_get_category(facilitylistid, "Court Bookings");
//  if (courtid < 0) {
//    ELOG(ERROR, "Cannot retrieve Sports Hall category ID for court in %s",
//         config_get_location());
//    goto fail;
//  }

  /* Fetch the configuration page */
  memset(url, 0, sizeof(url));
  snprintf(url, 1024, "%s/%s", WEBSITE_BASE, WEBSITE_CONFIGURATION);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(ERROR, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc),
         errbuf);
    goto fail;
  }

  /* Attempt to parse result as a json buffer */
  json = json_tokener_parse(buffer);
  if (!json) {
    ELOG(WARNING, "JSON parse error. Cannot parse configuration buffer");
    goto fail;
  }

  if (!json_object_object_get_ex(json, "OnlineUserId", &val)) {
    ELOG(WARNING, "JSON parse error. Invalid key \"OnlineUserId\"");
    goto fail;
  }
  memberid = json_object_get_int(val);
  reset_buffer();

  curl_easy_cleanup(cu);
  json_object_put(json);
  return 1;

fail:
  reset_buffer();
  curl_easy_cleanup(cu);
  if (json)
    json_object_put(json);
  return 0;
}

static void website_relogin(
    EV_P_ ev_timer *w,
    int revents)
{
  ELOG(VERBOSE, "Relogging into website (refresh cookies)");
  website_logout();
  website_login();
}

void website_init(
    void)
{
  if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
    ELOG(ERROR, "curl_global_init");
    exit(EXIT_FAILURE);
  }

  site = curl_easy_init();
  if (!site) {
    ELOG(ERROR, "Cannot initialize website handle");
    exit(EXIT_FAILURE);
  }

  /* Set cookies */
  curl_easy_setopt(site, CURLOPT_COOKIEFILE, config_get_cookies());
  curl_easy_setopt(site, CURLOPT_COOKIEJAR, config_get_cookies());
  curl_easy_setopt(site, CURLOPT_FOLLOWLOCATION, 1l);
  curl_easy_setopt(site, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(site, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(site, CURLOPT_WRITEFUNCTION, curl_write);
  curl_easy_setopt(site, CURLOPT_HEADERFUNCTION, curl_header_write);
  curl_easy_setopt(site, CURLOPT_WRITEDATA, buffer);
  curl_easy_setopt(site, CURLOPT_HEADERDATA, NULL);
  curl_easy_setopt(site, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(site, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(site, CURLOPT_URL, WEBSITE_BASE);
  //curl_easy_setopt(site, CURLOPT_VERBOSE, config_get_verbose());
  curl_easy_setopt(site, CURLOPT_VERBOSE, 0);
  curl_easy_setopt(site, CURLOPT_USERAGENT, "Abbey");

  if (!website_login()) {
    ELOG(CRITICAL, "Initial login failed. Exiting.");
    exit(EXIT_FAILURE);
  }

  if (!website_memberstate()) {
    ELOG(CRITICAL, "Initial login failed. Exiting.");
    exit(EXIT_FAILURE);
  }

  ev_timer_init(&relog, website_relogin, RELOGIN_TIMER, RELOGIN_TIMER);
  ev_timer_start(EV_DEFAULT_ &relog);

  return;
}


class_list_t website_get_timetable(
    int ndays)
{
  CURL *cu;
  CURLcode rc;
  int i, len;
  char url[1024] = {0};
  json_object *json = NULL, *array = NULL, *val;
  struct tm *t = NULL, now, when;
  char nowstr[64] = {0};
  char whenstr[64] = {0};
  time_t epoch = time(NULL);
  class_list_t head = NULL;
  class_t cl;

  ELOG(INFO, "Fetching timetable");

  /* Get the time now */
  t = localtime(&epoch);
  memcpy(&now, t, sizeof(struct tm));
  memcpy(&when, t, sizeof(struct tm));

  /* Adjust the number of days in the when (should overflow correctly) */
  when.tm_mday += ndays+1;
  mktime(&when);

  /* Convert to strings compatible with website */
  strftime(nowstr, 64, "%Y-%m-%d", &now);
  strftime(whenstr, 64, "%Y-%m-%d", &when);

  /* Create the URL to push */

  /* Fetch the timetable */
  snprintf(url, 1024, "%s/%s?FacilityLocationIdList=%d&DateFrom=%s&DateTo=%s", WEBSITE_BASE, 
                      WEBSITE_TIMETABLE, facilitylistid, nowstr, whenstr);
  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);
  ELOG(VERBOSE, "Timetable URL: %s", url);

  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(ERROR, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc),
         errbuf);
    goto fail;
  }

  /* Attempt to parse result as json */
  json = json_tokener_parse(buffer);
  if (!json) {
    ELOG(WARNING, "JSON parse error. Cannot parse timetable buffer");
    goto fail;
  }

  if (!json_object_object_get_ex(json, "Results", &array)) {
    ELOG(WARNING, "JSON parse error. Invalid key \"Results\"");
    goto fail;
  }

  head = calloc(1, sizeof(struct class_list));
  if (!head) {
    ELOG(WARNING, "Cannot create list head");
    goto fail;
  }
  LIST_INIT(head);

  len = json_object_array_length(array);
  ELOG(VERBOSE, "Timetable has %d entries", len);

  for (i=0; i < len; i++) {
    val = json_object_array_get_idx(array, i);
    cl = parse_json_class(val);
    /* Ignore invalid classes */
    if (!cl)
      continue;

    LIST_INSERT_HEAD(head, cl, l);
  }

  reset_buffer();
  curl_easy_cleanup(cu);
  json_object_put(json);
  return head;

fail:
  reset_buffer();
  curl_easy_cleanup(cu);
  if (json)
    json_object_put(json);
  if (head)
    class_free_timetable(head);
  free(head);
  return NULL;
}


int website_wait(
    class_t cl)
{
  assert(cl);
  CURL *cu;
  CURLcode rc;
  char url[1024] = {0};
  char post[1024] = {0};

  /* Submit the URL */
  snprintf(url, 1024, "%s/%s", WEBSITE_BASE, WEBSITE_WAIT);
  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  /* Create form output */
  snprintf(post, 1023, "ResourceScheduleId=%d", cl->resourceid);
  /* Attach as data to request */
  curl_easy_setopt(cu, CURLOPT_POSTFIELDS, post);
  ELOG(VERBOSE, "form: %s\n", post);

  /* Submit */
  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(WARNING, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc),
         errbuf);
    goto fail;
  }

  /* Attempt to check if successful */
  if (!parse_json_success(buffer)) {
    goto fail;
  }

  reset_buffer();
  curl_easy_cleanup(cu);
  return 1;

fail:
  reset_buffer();
  curl_easy_cleanup(cu);
  return 0;
}

int website_server_time_diff(
    void)
{
  return time_diff;
}

float website_price(
    class_t cl)
{
  assert(cl);
  CURL *cu;
  CURLcode rc;
  char url[1024] = {0};
  float price;

  /* Submit the URL */
  snprintf(url, 1024, "%s/%s?ActiveInstanceId=%d&OnlineUserId=%d", 
                      WEBSITE_BASE, WEBSITE_PRICE, cl->id, memberid);
  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  /* Submit */
  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(WARNING, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc),
         errbuf);
    goto fail;
  }

  price = parse_json_price(buffer);
  if (price < 0)
    goto fail;

  reset_buffer();
  curl_easy_cleanup(cu);
  return price;

fail:
  reset_buffer();
  curl_easy_cleanup(cu);
  return 0;
}


int website_book(
    class_t cl)
{
  assert(cl);
  CURL *cu;
  CURLcode rc;
  char url[1024] = {0};
  char post[1024] = {0};

  /* Submit the URL */
  snprintf(url, 1024, "%s/%s", WEBSITE_BASE, WEBSITE_BOOK);
  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);

  /* Create form output */
  snprintf(post, 1023, "ActivityInstanceId=%d", cl->id);

  /* Attach as data to request */
  curl_easy_setopt(cu, CURLOPT_POSTFIELDS, post);

  /* Submit */
  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(WARNING, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc),
         errbuf);
    goto fail;
  }

  /* Attempt to check if successful */
  if (!parse_json_success(buffer)) {
    goto fail;
  }

  reset_buffer();
  curl_easy_cleanup(cu);
  return 1;

fail:
  reset_buffer();
  curl_easy_cleanup(cu);
  return 0;
}


int website_commit(
    void)
{
  CURL *cu;
  CURLcode rc;
  char url[1024] = {0};
  struct curl_slist *hdrs = NULL;

  ELOG(VERBOSE, "Website commit");

  /* Submit the URL */
  snprintf(url, 1024, "%s/%s", WEBSITE_BASE, WEBSITE_COMMIT);
  cu = curl_easy_duphandle(site);
  curl_easy_setopt(cu, CURLOPT_URL, url);
  curl_easy_setopt(cu, CURLOPT_POSTFIELDS, "");

  /* Configure content type as json */
  hdrs = curl_slist_append(hdrs, "Content-type: application/json");
  curl_easy_setopt(cu, CURLOPT_HTTPHEADER, hdrs);

  /* Submit */
  rc = curl_easy_perform(cu);
  if (rc != CURLE_OK) {
    ELOG(ERROR, "Cannot fetch URL %s: %s, %s", url, curl_easy_strerror(rc),
         errbuf);
    goto fail;
  }

  reset_buffer();
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(cu);
  return 1;

fail:
  reset_buffer();
  if (hdrs)
    curl_slist_free_all(hdrs);
  curl_easy_cleanup(cu);
  return 0;
}


void website_update_config(
    void)
{
  curl_easy_setopt(site, CURLOPT_COOKIEFILE, config_get_cookies());
  curl_easy_setopt(site, CURLOPT_COOKIEJAR, config_get_cookies());
  // curl_easy_setopt(site, CURLOPT_VERBOSE, config_get_verbose());
}


void website_destroy(
    void)
{
  ev_timer_stop(EV_DEFAULT, &relog);
  clubid = -1;
  memberid = -1;
  bufsz = 0;
  if (buffer)
    free(buffer);
  memset(errbuf, 0, CURL_ERROR_SIZE);

  curl_easy_cleanup(site);

  ELOG(VERBOSE, "Website object destroyed");
  return;
}


char * website_errbuf(
    void)
{
  return errbuf;
}
