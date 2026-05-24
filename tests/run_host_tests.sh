#!/bin/sh
set -eu

cc -Wall -Wextra -Werror -I main tests/test_utf8_trim.c main/util/utf8.c -o /tmp/mimiclaw_test_utf8_trim
/tmp/mimiclaw_test_utf8_trim

: "${IDF_PATH:?Run get_idf before tests}"
cc -Wall -Wextra -Werror -I main -I "$IDF_PATH/components/json/cJSON" \
    tests/test_llm_openai_conversion.c main/llm/message_convert.c \
    "$IDF_PATH/components/json/cJSON/cJSON.c" \
    -o /tmp/mimiclaw_test_llm_openai_conversion
/tmp/mimiclaw_test_llm_openai_conversion

cc -Wall -Wextra -Werror -I main \
    tests/test_display_lvgl_pack.c main/display/display_lvgl_pack.c \
    -o /tmp/mimiclaw_test_display_lvgl_pack
/tmp/mimiclaw_test_display_lvgl_pack

cc -Wall -Wextra -Werror -DMIMI_DISPLAY_SERVICE_WEATHER_PARSE_TEST -I main \
    tests/test_display_service_weather_parse.c main/display/display_service.c main/util/utf8.c \
    -o /tmp/mimiclaw_test_display_service_weather_parse
/tmp/mimiclaw_test_display_service_weather_parse

