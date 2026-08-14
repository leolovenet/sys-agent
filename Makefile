.PHONY: all clean test

HOSTCC ?= cc

all:
	@$(MAKE) -C sys-botbase/

clean:
	@$(MAKE) clean -C sys-botbase/

test:
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/search_match_test.c sys-botbase/source/search_match.c -o /tmp/sys-botbase-search-match-test
	/tmp/sys-botbase-search-match-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/search_range_test.c sys-botbase/source/search_range.c -o /tmp/sys-botbase-search-range-test
	/tmp/sys-botbase-search-range-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -Isys-botbase/source tests/search_value_test.c sys-botbase/source/search_value.c -o /tmp/sys-botbase-search-value-test
	/tmp/sys-botbase-search-value-test
	python3 -m unittest -v tests/test_sysbot_search_client.py
