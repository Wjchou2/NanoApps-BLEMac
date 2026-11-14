#ifndef CLASSIC_INBOX_H
#define CLASSIC_INBOX_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define CLASSIC_INBOX "/Data/ClassicInbox"
#define CLASSIC_FILENAME_CAPACITY 60
#define CLASSIC_FILE_LIMIT (44u + 44100u * 4u * 300u)
#define CLASSIC_BUFFERED_PLAY_LIMIT (256u * 1024u)
#define CLASSIC_PENDING CLASSIC_INBOX "/.receiving"

/* The receive journal names the file that is still being written. */
bool classic_inbox_pending_name(char name[CLASSIC_FILENAME_CAPACITY]);

static inline bool classic_inbox_wav_name(const char *name)
{
    size_t length = strlen(name);
    return length >= 5 && name[length - 4] == '.' &&
           (name[length - 3] | 32) == 'w' && (name[length - 2] | 32) == 'a' &&
           (name[length - 1] | 32) == 'v';
}

#endif
