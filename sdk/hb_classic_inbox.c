#include "hb_classic_inbox.h"
#include "hb_sdk.h"

bool classic_inbox_pending_name(char name[CLASSIC_FILENAME_CAPACITY])
{
    name[0] = 0;
    uint32_t size = hb_fs_size(CLASSIC_PENDING);
    if (!size || size >= CLASSIC_FILENAME_CAPACITY ||
        hb_fs_read(CLASSIC_PENDING, name, size) != size)
        return false;
    name[size] = 0;
    if (name[0] == '.')
        return false;
    for (uint32_t i = 0; i < size; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}
