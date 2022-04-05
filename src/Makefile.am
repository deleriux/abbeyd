SUBDIRS = ini

bin_PROGRAMS = abbeyd

libdir = $(PAMDIR)

abbeyd_SOURCES = config.c class.c database.c website.c waitq.c logging.c main.c \
                 config.h class.h database.h website.h waitq.h logging.h common.h \
                 config.ini periodic.h periodic.c bookings.h bookings.c signals.h \
                 signals.c
abbeyd_CFLAGS = $(CURL_CFLAGS) $(SQLITE3_CFLAGS) $(JSON_CFLAGS) -Iini ini/libini.la
abbeyd_LDADD = $(CURL_LIBS) $(SQLITE3_LIBS) $(JSON_LIBS) 
