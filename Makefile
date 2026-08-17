.PHONY: all clean test

HOSTCC ?= cc

all:
	@$(MAKE) -C sys-botbase/

clean:
	@$(MAKE) clean -C sys-botbase/

test:
	$(HOSTCC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Isys-botbase/source tests/hekate_config_test.c sys-botbase/source/hekate_config.c -o /tmp/sys-botbase-hekate-config-test
	/tmp/sys-botbase-hekate-config-test
	$(HOSTCC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Isys-botbase/source tests/ftp_config_test.c sys-botbase/source/ftp_config.c -o /tmp/sys-botbase-ftp-config-test
	/tmp/sys-botbase-ftp-config-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/ftp_path_test.c sys-botbase/source/ftp_path.c -o /tmp/sys-botbase-ftp-path-test
	/tmp/sys-botbase-ftp-path-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/search_match_test.c sys-botbase/source/search_match.c -o /tmp/sys-botbase-search-match-test
	/tmp/sys-botbase-search-match-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/search_range_test.c sys-botbase/source/search_range.c -o /tmp/sys-botbase-search-range-test
	/tmp/sys-botbase-search-range-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/search_value_test.c sys-botbase/source/search_value.c -o /tmp/sys-botbase-search-value-test
	/tmp/sys-botbase-search-value-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/process_memory_select_test.c sys-botbase/source/process_memory_select.c -o /tmp/sys-botbase-process-memory-select-test
	/tmp/sys-botbase-process-memory-select-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/search_compare_test.c sys-botbase/source/search_compare.c -o /tmp/sys-botbase-search-compare-test
	/tmp/sys-botbase-search-compare-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/search_store_test.c sys-botbase/source/search_store.c -o /tmp/sys-botbase-search-store-test
	/tmp/sys-botbase-search-store-test
	python3 -m unittest -v tests/test_sysbot_search_client.py
