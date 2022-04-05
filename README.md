abbeyd
=======

Automated booking system for local gym.

This is a project I wrote to guarantee me spots on my gym bookings.
Many gyms (especially during covid) are limiting booking places for either gyms, or their classes.

Some classes can be almost impossible to get onto, all being allocated within a quarter of an hour of coming up..

This program is designed to always get you the booking for the class you want at the time you wanted it as soon as it comes up.

It runs as a daemon permanently in the background. I currently run it in a container.

Requirements
------------

abbeyd uses libev for event management, libcurl for HTTP parsing and libjson-c for API communication with the endpoint.
State is kept in a sqlite database. It contains a list of all the bookings, the times they were booked and if they were cancelled.
A static INI file parser is internally used for configuration (example config.ini given).

If you cancel a class manually on the website or in the app, the program wont book it again and it never books a class that costs money.

# Implementation

This program is really quite over-engineered but it performs a few tasks very well.

It parses and understands the concept of a timetable for the gym website.

With the timetables available, a set union is formed between the times for bookings wanted and the times available on the timetable.

The union of these two sets are then booked if not booked already.

A periodic timer occurs at a preconfigured time to refresh the site and look for new classes.

Another timer keeps the cookie we got for the website from expiring.

If you change the config file, it will detect and update to the new config automatically (uses inotify to accomplish this).

Finally, to adjust for daylight savings, we treat the endpoints absolute time (as given by the Date header in http transactions) as authoritative, and adjust our periodic timer to always use their time.

# Utility success

This program tries its upmost best to get you on your class.

1. It wakes up at 5 seconds past midnight (new classes become available at midnight) and tries the bookings.
2. If it fails to book you on (due to website or client network being down), it will retry for the next hour every few minutes.
3. If it fails to book you on because the class is already full, THEN add the class to the waiting list AND every minute idempotently attempt to rebook until you successfully get on or the class is no longer in the future.

I've missed a class using this, always the first to make a booking even when a class was full (because I changed the config file to just add it) it got me on the first available slot before anyone else.
