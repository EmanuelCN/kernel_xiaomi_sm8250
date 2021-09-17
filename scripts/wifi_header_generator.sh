#!/bin/bash

if [ ! -e "$@" ] || [ -z "$@" ]; then
  echo "Usage: ./wifi_header_generator.sh WCNSS_qcom_cfg.ini > drivers/staging/qcacld-3.0/core/hdd/src/wlan_cfg_ini.h"
  exit 1
fi

echo "static const char wlan_cfg[] __initconst = {"
cat "$@" | grep -ve '^$\|^#' | sed 's@\"@\\\\"@g' | while read line; do printf '\t\"%s\\n\"\n' "$line"; done
echo "};"
