.PHONY: all clean test

HOSTCC ?= cc

all:
	@$(MAKE) -C sys-agent/

clean:
	@$(MAKE) clean -C sys-agent/

test:
	$(HOSTCC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Isys-agent/source tests/hekate_config_test.c sys-agent/source/hekate_config.c -o /tmp/sys-agent-hekate-config-test
	/tmp/sys-agent-hekate-config-test
	$(HOSTCC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Isys-agent/source tests/ftp_config_test.c sys-agent/source/ftp_config.c -o /tmp/sys-agent-ftp-config-test
	/tmp/sys-agent-ftp-config-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-agent/source tests/ftp_path_test.c sys-agent/source/ftp_path.c -o /tmp/sys-agent-ftp-path-test
	/tmp/sys-agent-ftp-path-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-agent/source tests/search_match_test.c sys-agent/source/search_match.c -o /tmp/sys-agent-search-match-test
	/tmp/sys-agent-search-match-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-agent/source tests/search_range_test.c sys-agent/source/search_range.c -o /tmp/sys-agent-search-range-test
	/tmp/sys-agent-search-range-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-agent/source tests/search_value_test.c sys-agent/source/search_value.c -o /tmp/sys-agent-search-value-test
	/tmp/sys-agent-search-value-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-agent/source tests/process_memory_select_test.c sys-agent/source/process_memory_select.c -o /tmp/sys-agent-process-memory-select-test
	/tmp/sys-agent-process-memory-select-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-agent/source tests/search_compare_test.c sys-agent/source/search_compare.c -o /tmp/sys-agent-search-compare-test
	/tmp/sys-agent-search-compare-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-agent/source tests/search_store_test.c sys-agent/source/search_store.c -o /tmp/sys-agent-search-store-test
	/tmp/sys-agent-search-store-test
	python3 -m unittest -v tests/test_sysagent_client.py
