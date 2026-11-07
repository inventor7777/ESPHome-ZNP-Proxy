#!/bin/sh
set -eu
cd "$(dirname "$0")"
test_build=$(mktemp -d)
trap 'rm -rf "$test_build"' EXIT HUP INT TERM

# Empty include shims let fakes.h supply the ESPHome interfaces to the real adapter.
for header in components/socket/tcp_client_link.h components/socket/tcp_listener.h \
  components/uart/uart.h core/component.h core/application.h core/log.h core/automation.h; do
  mkdir -p "$test_build/include/esphome/$(dirname "$header")"
  : > "$test_build/include/esphome/$header"
done
for suite in protocol transport; do
  "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -g -I"$test_build/include" "test_$suite.cpp" -o "$test_build/test_$suite"
  "$test_build/test_$suite"
done
