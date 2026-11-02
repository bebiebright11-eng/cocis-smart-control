#ifndef STORE_H
#define STORE_H

/* Load the saved headcounts of Room 1 and Room 2 from the SD card (if any). */
void store_load_rooms(void);

/* Start the background task that saves changes to the SD card:
 * the registry and the room headcounts, about once a second. */
void store_start(void);

#endif