.DEFAULT_GOAL := check

.PHONY: check build test

test:
	@mkdir -p .agent/local/tests
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
		-I tests/stubs -I firmware/main tests/test_quota.c firmware/main/quota_protocol.c -o .agent/local/tests/quota
	.agent/local/tests/quota
	$(CC) -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
		-I tests/stubs -I firmware/main -I firmware/components/bsp/include \
		tests/test_music.c firmware/main/rtttl.c firmware/main/meow_delivery.c -lm -o .agent/local/tests/music
	.agent/local/tests/music
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
		-I firmware/main tests/test_meow_delivery.c firmware/main/meow_delivery.c -o .agent/local/tests/meow
	.agent/local/tests/meow
	$(CC) -std=c11 -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
		-I tests/stubs -I firmware/main tests/test_storage.c firmware/main/preferences.c -o .agent/local/tests/storage
	.agent/local/tests/storage
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
		-I tests/stubs -I firmware/main -I firmware/components/bsp/include tests/test_power.c -o .agent/local/tests/power
	.agent/local/tests/power
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
		-I tests/stubs -I firmware/main tests/test_portal.c firmware/main/portal_dns.c firmware/main/web_code.c -o .agent/local/tests/portal
	.agent/local/tests/portal
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
		-I firmware/main tests/test_web_peer.c firmware/main/web_peer.c -o .agent/local/tests/web-peer
	.agent/local/tests/web-peer
	node tests/test_web_transport.mjs
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
		-I tests/network_stubs -I tests/stubs -I firmware/main tests/test_network.c -o .agent/local/tests/network
	.agent/local/tests/network

build:
	cd firmware && idf.py build
	python3 tools/check_stack_budget.py firmware/build

check:
	@ports=$$(ioreg -r -n 'USB JTAG/serial debug unit' -l -w0 | awk -F '"' '/"IOCalloutDevice".*\/dev\/cu\.usbmodem/ { print $$4 }' | sort -u); \
	if [ -z "$$ports" ]; then \
		printf '%s\n' '未检测到乐鑫 USB Serial/JTAG 串口，请检查设备电源和 USB-C 数据线。' >&2; \
		exit 1; \
	fi; \
	printf '%s\n' '检测到乐鑫 USB Serial/JTAG 串口' "$$ports"
