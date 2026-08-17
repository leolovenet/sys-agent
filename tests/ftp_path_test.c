#include "ftp_path.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char output[800];
    assert(ftpPathResolve("/", output, sizeof(output)) && !strcmp(output, "sdmc:/"));
    assert(ftpPathResolve("/switch/中文 file.txt", output, sizeof(output)));
    assert(!strcmp(output, "sdmc:/switch/中文 file.txt"));
    assert(!ftpPathResolve("switch/file", output, sizeof(output)));
    assert(!ftpPathResolve("/sdmc:/file", output, sizeof(output)));
    assert(!ftpPathResolve("/a/../b", output, sizeof(output)));
    assert(ftpPathResolve("/a/./b", output, sizeof(output)));

    assert(ftpPathIsSearchStorage("/switch/sys-botbase/search"));
    assert(ftpPathIsSearchStorage("/switch/sys-botbase/search/session.dat"));
    assert(!ftpPathIsSearchStorage("/switch/sys-botbase/search-old"));
    assert(!ftpPathIsSearchStorage("/switch/sys-botbase"));
    assert(!ftpPathCanMutate("/switch/sys-botbase/search/session.dat", true));
    assert(ftpPathCanMutate("/switch/sys-botbase/search/session.dat", false));
    assert(ftpPathCanMutate("/atmosphere/contents/test", true));

    puts("ftp_path_test: ok");
    return 0;
}
