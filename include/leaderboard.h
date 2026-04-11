#pragma once

#include <stddef.h>
#include <stdint.h>

void leaderboardInit();

/**
 * Submit a finished run: merges into top 3 by score (descending), persists.
 * @param initials3 exactly three display characters (A–Z, 0–9, or space stored as '_' in NVS optional — use A-Z0-9)
 * @return true if the stored leaderboard changed
 */
bool leaderboardSubmitEntry(uint32_t score, const char initials3[3]);

/** Format one row for the EPD, e.g. "1. 1200 ABC" or "2. ---". */
void leaderboardFormatRow(int rank1Based, char *buf, size_t buflen);
