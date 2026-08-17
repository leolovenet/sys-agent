#include "hekate_config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void writeConfig(const char* path, const char* contents)
{
    FILE* file = fopen(path, "wb");
    assert(file);
    assert(fwrite(contents, 1, strlen(contents), file) == strlen(contents));
    fclose(file);
}

int main(int argc, char** argv)
{
    if (argc == 2) {
        unsigned actualIndex = 0;
        HekateConfigResult actual = hekateFindMainConfigId(argv[1], "Atm-Emu", &actualIndex);
        printf("result=%s index=%u\n", hekateConfigResultName(actual), actualIndex);
        return actual == HekateConfigOk ? 0 : 1;
    }
    char path[] = "/tmp/sys-botbase-hekate-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unsigned index = 0;
    writeConfig(path,
        "[config]\r\nautoboot=0\r\n\r\n"
        "[Virtual]\r\nemummcforce=1\r\nid=Atm-Emu\r\n"
        "[Real]\r\nid=Atm-Sys\r\n");
    assert(hekateFindMainConfigId(path, "Atm-Emu", &index) == HekateConfigOk);
    assert(index == 1);
    assert(hekateFindMainConfigId(path, "missing", &index) == HekateConfigNotFound);
    writeConfig(path, "[One]\nid=Atm-Emu\n[Two]\nid=Atm-Emu\n");
    assert(hekateFindMainConfigId(path, "Atm-Emu", &index) == HekateConfigDuplicateId);
    unlink(path);
    puts("hekate_config_test: ok");
    return 0;
}
