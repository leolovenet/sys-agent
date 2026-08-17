#include "ftp_config.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    FtpConfig config;
    ftpConfigDefaults(&config);
    assert(config.enabled);
    assert(config.port == 6001);
    assert(config.anonymous);
    assert(config.timeout == 30);

    const char* valid =
        "[other]\nport=1\n[ftp]\n enabled = 0\nport=2121\nanonymous=false\n"
        "username=\"leo\"\npassword = test\ntimeout=0\n";
    assert(ftpConfigParseText(valid, &config) == FtpConfigOk);
    assert(!config.enabled && !config.anonymous && config.port == 2121 && config.timeout == 0);
    assert(!strcmp(config.username, "leo") && !strcmp(config.password, "test"));

    assert(ftpConfigParseText("[ftp]\nport=0\n", &config) == FtpConfigInvalidValue);
    assert(ftpConfigParseText("[ftp]\nport=65536\n", &config) == FtpConfigInvalidValue);
    assert(ftpConfigParseText("[ftp]\nanonymous=maybe\n", &config) == FtpConfigInvalidValue);
    assert(ftpConfigParseText("[ftp]\nanonymous=0\n", &config)
        == FtpConfigCredentialsRequired);
    assert(ftpConfigParseText("[ftp]\nanonymous=0\nusername=a\npassword=b\n", &config)
        == FtpConfigOk);

    puts("ftp_config_test: ok");
    return 0;
}
