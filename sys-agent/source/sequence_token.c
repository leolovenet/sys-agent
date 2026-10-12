#include "sequence_token.h"

#include <stddef.h>

char* sequenceTokenNext(char** cursor)
{
    if (cursor == NULL || *cursor == NULL)
        return NULL;

    char* token = *cursor;
    while (*token == ',')
        token++;
    if (*token == '\0') {
        *cursor = NULL;
        return NULL;
    }

    char* end = token;
    while (*end != '\0' && *end != ',')
        end++;
    if (*end == ',') {
        *end = '\0';
        *cursor = end + 1;
    }
    else {
        *cursor = NULL;
    }
    return token;
}
