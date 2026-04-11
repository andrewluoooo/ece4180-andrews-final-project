#include "leaderboard.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <string.h>

struct LbEntry {
  uint32_t score;
  char name[4];
};

static Preferences prefs;
static LbEntry entries[3];

static constexpr const char *kNs = "sk_lb";

static void sortEntriesDesc(LbEntry *e, int n) {
  for (int i = 0; i < n; ++i) {
    int best = i;
    for (int j = i + 1; j < n; ++j) {
      if (e[j].score > e[best].score) {
        best = j;
      }
    }
    if (best != i) {
      const LbEntry t = e[i];
      e[i] = e[best];
      e[best] = t;
    }
  }
}

static bool entriesTripleEqual(const LbEntry a[3], const LbEntry b[3]) {
  for (int i = 0; i < 3; ++i) {
    if (a[i].score != b[i].score || strncmp(a[i].name, b[i].name, 3) != 0) {
      return false;
    }
  }
  return true;
}

static void clearEntry(LbEntry &e) {
  e.score = 0;
  memcpy(e.name, "---", 4);
}

void leaderboardInit() {
  for (int i = 0; i < 3; ++i) {
    clearEntry(entries[i]);
  }
  if (!prefs.begin(kNs, true)) {
    return;
  }
  for (int i = 0; i < 3; ++i) {
    char keyS[4] = {'s', static_cast<char>('0' + i), '\0'};
    char keyN[4] = {'n', static_cast<char>('0' + i), '\0'};
    const uint32_t s = prefs.getUInt(keyS, 0);
    entries[i].score = s;
    String ns = prefs.getString(keyN, "");
    if (ns.length() >= 3) {
      entries[i].name[0] = static_cast<char>(ns[0]);
      entries[i].name[1] = static_cast<char>(ns[1]);
      entries[i].name[2] = static_cast<char>(ns[2]);
      entries[i].name[3] = '\0';
    } else if (s > 0) {
      memcpy(entries[i].name, "???", 4);
    } else {
      memcpy(entries[i].name, "---", 4);
    }
  }
  prefs.end();
}

static void persist() {
  if (!prefs.begin(kNs, false)) {
    return;
  }
  for (int i = 0; i < 3; ++i) {
    char keyS[4] = {'s', static_cast<char>('0' + i), '\0'};
    char keyN[4] = {'n', static_cast<char>('0' + i), '\0'};
    prefs.putUInt(keyS, entries[i].score);
    if (entries[i].score == 0) {
      prefs.putString(keyN, "---");
    } else {
      prefs.putString(keyN, String(entries[i].name));
    }
  }
  prefs.end();
}

bool leaderboardSubmitEntry(uint32_t score, const char initials3[3]) {
  if (score == 0) {
    return false;
  }
  char nm[4];
  for (int i = 0; i < 3; ++i) {
    char c = initials3[i];
    if (c == ' ') {
      c = '_';
    }
    c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
      c = '?';
    }
    nm[i] = c;
  }
  nm[3] = '\0';

  LbEntry buf[4];
  int n = 0;
  for (int i = 0; i < 3; ++i) {
    if (entries[i].score > 0) {
      buf[n++] = entries[i];
    }
  }
  LbEntry neu;
  neu.score = score;
  memcpy(neu.name, nm, 4);
  buf[n++] = neu;

  sortEntriesDesc(buf, n);

  LbEntry newTop[3];
  for (int i = 0; i < 3; ++i) {
    if (i < n) {
      newTop[i] = buf[i];
    } else {
      clearEntry(newTop[i]);
    }
  }

  const bool changed = !entriesTripleEqual(newTop, entries);
  memcpy(entries, newTop, sizeof(entries));
  if (changed) {
    persist();
  }
  return changed;
}

void leaderboardFormatRow(int rank1Based, char *buf, size_t buflen) {
  if (rank1Based < 1 || rank1Based > 3 || buflen == 0) {
    if (buflen > 0) {
      buf[0] = '\0';
    }
    return;
  }
  const LbEntry &e = entries[rank1Based - 1];
  if (e.score == 0) {
    snprintf(buf, buflen, "%d. ---", rank1Based);
  } else {
    char disp[4];
    memcpy(disp, e.name, 3);
    disp[3] = '\0';
    for (int i = 0; i < 3; ++i) {
      if (disp[i] == '_') {
        disp[i] = ' ';
      }
    }
    snprintf(buf, buflen, "%d. %lu %.3s", rank1Based, static_cast<unsigned long>(e.score), disp);
  }
}
