#include <assert.h>
#include <stddef.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102

esp_err_t parse_wttr_weather_response(const char *response,
                                      char *city,
                                      size_t city_size,
                                      char *summary,
                                      size_t summary_size);

static int valid_utf8(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p <= 0x7F) {
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) != 0x80) return 0;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
            p += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

static void parses_city_and_summary_lines(void)
{
    char city[32] = "old";
    char summary[32] = "old";

    esp_err_t err = parse_wttr_weather_response("北京\n晴 +25°C\n", city, sizeof(city), summary, sizeof(summary));

    assert(err == ESP_OK);
    assert(strcmp(city, "北京") == 0);
    assert(strcmp(summary, "晴 +25°C") == 0);
}

static void trims_blank_and_space_around_city_and_summary(void)
{
    char city[32] = "old";
    char summary[32] = "old";

    esp_err_t err = parse_wttr_weather_response("\n\t 上海  \r\n  多云 +18°C \t\n", city, sizeof(city), summary, sizeof(summary));

    assert(err == ESP_OK);
    assert(strcmp(city, "上海") == 0);
    assert(strcmp(summary, "多云 +18°C") == 0);
}

static void single_line_city_succeeds_with_empty_summary(void)
{
    char city[32] = "old";
    char summary[32] = "old";

    esp_err_t err = parse_wttr_weather_response("深圳\n", city, sizeof(city), summary, sizeof(summary));

    assert(err == ESP_OK);
    assert(strcmp(city, "深圳") == 0);
    assert(strcmp(summary, "") == 0);
}

static void whitespace_only_response_fails_and_clears_outputs(void)
{
    char city[32] = "old";
    char summary[32] = "old";

    esp_err_t err = parse_wttr_weather_response(" \t\r\n\n  ", city, sizeof(city), summary, sizeof(summary));

    assert(err == ESP_FAIL);
    assert(strcmp(city, "") == 0);
    assert(strcmp(summary, "") == 0);
}

static void small_city_buffer_trims_incomplete_utf8_tail(void)
{
    char city[8] = "old";
    char summary[32] = "old";

    esp_err_t err = parse_wttr_weather_response("北京朝阳\n晴朗\n", city, sizeof(city), summary, sizeof(summary));

    assert(err == ESP_OK);
    assert(strcmp(city, "北京") == 0);
    assert(valid_utf8(city));
    assert(strcmp(summary, "晴朗") == 0);
}

static void small_summary_buffer_trims_incomplete_utf8_tail(void)
{
    char city[32] = "old";
    char summary[8] = "old";

    esp_err_t err = parse_wttr_weather_response("北京\n晴朗多云\n", city, sizeof(city), summary, sizeof(summary));

    assert(err == ESP_OK);
    assert(strcmp(city, "北京") == 0);
    assert(strcmp(summary, "晴朗") == 0);
    assert(valid_utf8(summary));
}

static void one_byte_output_buffers_are_safe(void)
{
    char city[1] = {'x'};
    char summary[1] = {'y'};

    esp_err_t err = parse_wttr_weather_response("北京\n晴朗\n", city, sizeof(city), summary, sizeof(summary));

    assert(err == ESP_OK);
    assert(strcmp(city, "") == 0);
    assert(strcmp(summary, "") == 0);
}

static void invalid_args_return_invalid_arg(void)
{
    char city[32];
    char summary[32];

    assert(parse_wttr_weather_response(NULL, city, sizeof(city), summary, sizeof(summary)) == ESP_ERR_INVALID_ARG);
    assert(parse_wttr_weather_response("北京", NULL, sizeof(city), summary, sizeof(summary)) == ESP_ERR_INVALID_ARG);
    assert(parse_wttr_weather_response("北京", city, 0, summary, sizeof(summary)) == ESP_ERR_INVALID_ARG);
    assert(parse_wttr_weather_response("北京", city, sizeof(city), NULL, sizeof(summary)) == ESP_ERR_INVALID_ARG);
    assert(parse_wttr_weather_response("北京", city, sizeof(city), summary, 0) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    parses_city_and_summary_lines();
    trims_blank_and_space_around_city_and_summary();
    single_line_city_succeeds_with_empty_summary();
    whitespace_only_response_fails_and_clears_outputs();
    small_city_buffer_trims_incomplete_utf8_tail();
    small_summary_buffer_trims_incomplete_utf8_tail();
    one_byte_output_buffers_are_safe();
    invalid_args_return_invalid_arg();
    return 0;
}
