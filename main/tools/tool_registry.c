#include "tool_registry.h"
#include "mimi_config.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_gpio.h"
#include "tools/tool_pwm.h"
#include "tools/tool_script.h"
#include "tools/tool_rule.h"
#include "tools/tool_ota.h"
#include "tools/tool_sensors.h"
#include "tools/tool_nav.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 48

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information via Tavily (preferred) or Brave when configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. " MIMI_SPIFFS_BASE "/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). If omitted, current turn channel is used when available\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel='telegram'. If omitted during a Telegram turn, current chat_id is used\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    /* Register GPIO tools */
    tool_gpio_init();

    mimi_tool_t gw = {
        .name = "gpio_write",
        .description = "Set a GPIO pin HIGH or LOW. Controls LEDs, relays, and other digital outputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"},"
            "\"state\":{\"type\":\"integer\",\"description\":\"1 for HIGH, 0 for LOW\"}},"
            "\"required\":[\"pin\",\"state\"]}",
        .execute = tool_gpio_write_execute,
    };
    register_tool(&gw);

    mimi_tool_t gr = {
        .name = "gpio_read",
        .description = "Read a GPIO pin state. Returns HIGH or LOW. Use for checking switches, sensors, and digital inputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"}},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_read_execute,
    };
    register_tool(&gr);

    mimi_tool_t ga = {
        .name = "gpio_read_all",
        .description = "Read all allowed GPIO pin states in a single call. Returns each pin's HIGH/LOW state.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_gpio_read_all_execute,
    };
    register_tool(&ga);

    /* Register PWM/Servo tools */
    tool_pwm_init();

    mimi_tool_t ps = {
        .name = "pwm_set",
        .description = "Set PWM pulse width on a GPIO pin. Generates 50Hz signal. "
        "Use pulse_us (microseconds) to control servos, ESCs, or any PWM device. "
        "Standard servo range: 500-2500 us. Standard ESC range: 1000-2000 us.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"gpio\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"},"
            "\"pulse_us\":{\"type\":\"integer\",\"description\":\"Pulse width in microseconds (0-20000)\"}},"
            "\"required\":[\"gpio\",\"pulse_us\"]}",
        .execute = tool_pwm_set_execute,
    };
    register_tool(&ps);

    mimi_tool_t pr = {
        .name = "pwm_release",
        .description = "Stop PWM output and release a channel on a GPIO pin.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"gpio\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"}},"
            "\"required\":[\"gpio\"]}",
        .execute = tool_pwm_release_execute,
    };
    register_tool(&pr);

    mimi_tool_t rs = {
        .name = "rc_steer",
        .description = "Control RC car steering. -100 = full left, 0 = center, +100 = full right. "
        "Uses steering servo GPIO and pulse calibration from rc.json config.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"steer_pct\":{\"type\":\"integer\",\"description\":\"Steering percentage: -100 (full left) to 100 (full right), 0 = center\"}},"
            "\"required\":[\"steer_pct\"]}",
        .execute = tool_rc_steer_execute,
    };
    register_tool(&rs);

    mimi_tool_t rt = {
        .name = "rc_throttle",
        .description = "Control RC car motor speed and direction. -100 = full reverse, 0 = stop, +100 = full forward. "
        "Uses ESC GPIO and pulse calibration from rc.json config.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"throttle_pct\":{\"type\":\"integer\",\"description\":\"Throttle percentage: -100 (full reverse) to 100 (full forward), 0 = stop\"}},"
            "\"required\":[\"throttle_pct\"]}",
        .execute = tool_rc_throttle_execute,
    };
    register_tool(&rt);

    /* Register Script tools */
    tool_script_init();

    mimi_tool_t sc = {
        .name = "script_create",
        .description = "Create a named script with an ordered list of tool calls (max 20 steps). Scripts run without LLM, saving tokens on repeated actions. Keep scripts small and focused.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Script name. MUST be ASCII letters, digits, underscore only. No CJK, spaces, or special chars. Example: servo_wave\"},"
            "\"steps\":{\"type\":\"array\",\"description\":\"Ordered list of tool calls (max 20 steps). Keep scripts small.\",\"items\":{\"type\":\"object\","
            "\"properties\":{\"tool\":{\"type\":\"string\",\"description\":\"Tool name to call\"},"
            "\"input\":{\"type\":\"object\",\"description\":\"Tool input as JSON object\"},"
            "\"delay_ms\":{\"type\":\"integer\",\"description\":\"Optional delay in ms after this step\"}},"
            "\"required\":[\"tool\",\"input\"]}}},"
            "\"required\":[\"name\",\"steps\"]}",
        .execute = tool_script_create_execute,
    };
    register_tool(&sc);

    mimi_tool_t sr2 = {
        .name = "script_run",
        .description = "Execute a stored script by name. Runs all steps sequentially without LLM involvement.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Script name to run\"}},"
            "\"required\":[\"name\"]}",
        .execute = tool_script_run_execute,
    };
    register_tool(&sr2);

    mimi_tool_t sl = {
        .name = "script_list",
        .description = "List all stored scripts with their step counts.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_script_list_execute,
    };
    register_tool(&sl);

    mimi_tool_t sm = {
        .name = "script_remove",
        .description = "Delete a stored script by name.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Script name to delete\"}},"
            "\"required\":[\"name\"]}",
        .execute = tool_script_remove_execute,
    };
    register_tool(&sm);

    /* Register Rule Engine tools */
    mimi_tool_t ra = {
        .name = "rule_add",
        .description = "Create a persistent rule that continuously monitors sensors and automatically executes actions when conditions are met. "
            "Use this when the user wants automatic responses like 'when obstacle < 30cm, escalate' or 'when tilted > 45 degrees, stop'. "
            "Rules run independently without LLM involvement, making them efficient for real-time control loops. "
            "Nav trigger types: ultrasonic_distance (channel: 0=left,1=front,2=right, value in cm), "
            "imu_tilt (channel: 0=|roll|,1=|pitch|, value in degrees), "
            "gps_distance_to (lat+lon required, value in meters). "
            "A rule consists of: trigger, condition (compare numeric value), actions (when true), optional else_actions (when false).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the rule\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Seconds between evaluations (e.g. 1 for fast response)\"},"
            "\"cooldown_s\":{\"type\":\"integer\",\"description\":\"Minimum seconds between firings (prevents jitter, default 0)\"},"
            "\"trigger\":{\"type\":\"object\",\"description\":\"Sensor to read\","
              "\"properties\":{"
                "\"type\":{\"type\":\"string\","
                  "\"enum\":[\"gpio_read\",\"gpio_read_all\",\"interval\","
                             "\"ultrasonic_distance\",\"imu_tilt\",\"gps_distance_to\"],"
                  "\"description\":\"gpio_read/gpio_read_all: GPIO; interval: time-based; "
                                   "ultrasonic_distance: HC-SR04 distance in cm; "
                                   "imu_tilt: |roll|(ch=0) or |pitch|(ch=1) in degrees; "
                                   "gps_distance_to: haversine distance in meters\"},"
                "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin (required for gpio_read/gpio_read_all)\"},"
                "\"channel\":{\"type\":\"integer\",\"description\":\"Channel: ultrasonic 0=left,1=front,2=right; imu 0=|roll|,1=|pitch|\"},"
                "\"lat\":{\"type\":\"number\",\"description\":\"Reference latitude (required for gps_distance_to)\"},"
                "\"lon\":{\"type\":\"number\",\"description\":\"Reference longitude (required for gps_distance_to)\"}"
              "},"
              "\"required\":[\"type\"]},"
            "\"condition\":{\"type\":\"object\",\"description\":\"Comparison against integer value\","
              "\"properties\":{"
                "\"op\":{\"type\":\"string\","
                  "\"enum\":[\"==\",\"!=\",\">\",\"<\",\">=\",\"<=\",\"any_high\",\"all_low\",\"mod_eq\",\"mod_ne\"],"
                  "\"description\":\"Comparison operator\"},"
                "\"value\":{\"type\":\"integer\",\"description\":\"Threshold: cm for ultrasonic, degrees for imu_tilt, meters for gps_distance_to\"}"
              "},"
              "\"required\":[\"op\",\"value\"]},"
            "\"actions\":{\"type\":\"array\",\"description\":\"Actions when condition is TRUE (max 4)\","
              "\"items\":{\"type\":\"object\","
                "\"properties\":{"
                  "\"type\":{\"type\":\"string\","
                    "\"enum\":[\"gpio_write\",\"pwm_set\",\"pwm_release\",\"script_run\",\"escalate\"]},"
                  "\"pin\":{\"type\":\"integer\"},"
                  "\"value\":{\"type\":\"integer\"},"
                  "\"script_name\":{\"type\":\"string\"},"
                  "\"escalate_msg\":{\"type\":\"string\"}"
                "},"
                "\"required\":[\"type\"]}},"
            "\"else_actions\":{\"type\":\"array\",\"description\":\"Actions when condition is FALSE (max 4)\","
              "\"items\":{\"type\":\"object\","
                "\"properties\":{"
                  "\"type\":{\"type\":\"string\","
                    "\"enum\":[\"gpio_write\",\"pwm_set\",\"pwm_release\",\"script_run\",\"escalate\"]},"
                  "\"pin\":{\"type\":\"integer\"},"
                  "\"value\":{\"type\":\"integer\"},"
                  "\"script_name\":{\"type\":\"string\"},"
                  "\"escalate_msg\":{\"type\":\"string\"}"
                "},"
                "\"required\":[\"type\"]}}"
            "},"
            "\"required\":[\"name\",\"interval_s\",\"trigger\",\"condition\",\"actions\"]}",
        .execute = tool_rule_add_execute,
    };
    register_tool(&ra);

    mimi_tool_t rr = {
        .name = "rule_remove",
        .description = "Remove a rule by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"rule_id\":{\"type\":\"string\",\"description\":\"8-character rule ID\"}},"
            "\"required\":[\"rule_id\"]}",
        .execute = tool_rule_remove_execute,
    };
    register_tool(&rr);

    mimi_tool_t rl = {
        .name = "rule_list",
        .description = "List all rules with their status, triggers, conditions, and action counts.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_rule_list_execute,
    };
    register_tool(&rl);

    mimi_tool_t ren = {
        .name = "rule_enable",
        .description = "Enable a disabled rule by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"rule_id\":{\"type\":\"string\",\"description\":\"8-character rule ID\"}},"
            "\"required\":[\"rule_id\"]}",
        .execute = tool_rule_enable_execute,
    };
    register_tool(&ren);

    mimi_tool_t rdis = {
        .name = "rule_disable",
        .description = "Disable a rule by its ID without removing it.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"rule_id\":{\"type\":\"string\",\"description\":\"8-character rule ID\"}},"
            "\"required\":[\"rule_id\"]}",
        .execute = tool_rule_disable_execute,
    };
    register_tool(&rdis);

    /* Register OTA update tool */
    mimi_tool_t ota = {
        .name = "ota_update",
        .description = "Perform an OTA firmware update from an HTTPS URL. "
            "Downloads the firmware binary and flashes it. Device reboots automatically on success. "
            "Only call this when the user explicitly requests a firmware update.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"HTTPS URL to the firmware .bin file\"}},"
            "\"required\":[\"url\"]}",
        .execute = tool_ota_execute,
    };
    register_tool(&ota);

    /* Register nav first (GPS filter must be ready before driver_gps_start) */
    tool_nav_init();

    /* Register Sensors tools */
    tool_sensors_init();

    mimi_tool_t ut = {
        .name = "ultrasonic_test",
        .description = "Test ultrasonic sensor readings. Leave input empty for 10 readings.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"continuous\":{\"type\":\"boolean\",\"description\":\"Run continuously\"},"
            "\"count\":{\"type\":\"integer\",\"description\":\"Number of measurements\"},"
            "\"delay_ms\":{\"type\":\"integer\",\"description\":\"Delay between measurements\"}},"
            "\"required\":[]}",
        .execute = tool_ultrasonic_test_execute,
    };
    register_tool(&ut);

    mimi_tool_t it = {
        .name = "imu_test",
        .description = "Test IMU (MPU6050) sensor readings. Leave input empty for 10 readings.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"continuous\":{\"type\":\"boolean\",\"description\":\"Run continuously\"},"
            "\"count\":{\"type\":\"integer\",\"description\":\"Number of measurements\"},"
            "\"delay_ms\":{\"type\":\"integer\",\"description\":\"Delay between measurements\"}},"
            "\"required\":[]}",
        .execute = tool_imu_test_execute,
    };
    register_tool(&it);

    mimi_tool_t gpt = {
        .name = "gps_test",
        .description = "Test GPS (NEO-6M) sensor readings. Leave input empty for 10 readings.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"continuous\":{\"type\":\"boolean\",\"description\":\"Run continuously\"},"
            "\"count\":{\"type\":\"integer\",\"description\":\"Number of measurements\"},"
            "\"delay_ms\":{\"type\":\"integer\",\"description\":\"Delay between measurements\"}},"
            "\"required\":[]}",
        .execute = tool_gps_test_execute,
    };
    register_tool(&gpt);

    /* Register LLM-facing sensor read tools */
    mimi_tool_t rd = {
        .name = "read_distance",
        .description = "Read the current ultrasonic distance sensor values (left / front / right). "
            "Returns distances in cm and data freshness. Use before navigation decisions.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_read_distance_execute,
    };
    register_tool(&rd);

    mimi_tool_t ri = {
        .name = "read_imu",
        .description = "Read the current IMU (MPU6050) orientation: roll, pitch, yaw angles and yaw rate. "
            "Yaw drifts without GPS correction; use GPS course when speed > 0.5 m/s.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_read_imu_execute,
    };
    register_tool(&ri);

    mimi_tool_t rg = {
        .name = "read_gps",
        .description = "Read the current GPS position from the NEO-6M module. "
            "Returns lat/lon, fix status, satellite count, speed, and course. "
            "Position is only reliable when fix=true and sats >= 4.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_read_gps_execute,
    };
    register_tool(&rg);

    /* Register nav waypoint + status tools */
    /* tool_nav_init() already called above before sensors */

    mimi_tool_t nsw = {
        .name = "nav_save_waypoint",
        .description = "Save the car's current GPS position as a named waypoint. "
            "Requires a valid GPS fix. The waypoint persists across reboots and can be used with nav_goto_waypoint. "
            "If a waypoint with the same name already exists it is overwritten.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Unique waypoint name (e.g. '快递柜')\"},"
            "\"notes\":{\"type\":\"string\",\"description\":\"Optional description or landmark notes\"}"
            "},"
            "\"required\":[\"name\"]}",
        .execute = tool_nav_save_waypoint_execute,
    };
    register_tool(&nsw);

    mimi_tool_t nlw = {
        .name = "nav_list_waypoints",
        .description = "List all saved waypoints with their names, GPS coordinates, and satellite count at save time.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_nav_list_waypoints_execute,
    };
    register_tool(&nlw);

    mimi_tool_t ndw = {
        .name = "nav_delete_waypoint",
        .description = "Delete a saved waypoint by name.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Waypoint name to delete\"}"
            "},"
            "\"required\":[\"name\"]}",
        .execute = tool_nav_delete_waypoint_execute,
    };
    register_tool(&ndw);

    mimi_tool_t nst = {
        .name = "nav_status",
        .description = "Get the current navigation status: FSM state, active goal, GPS position, distances, and IMU orientation. "
            "Call this after receiving a NAV escalate event to assess the situation.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_nav_status_execute,
    };
    register_tool(&nst);

    /* Phase 6: autonomous navigation control tools */
    mimi_tool_t ng = {
        .name = "nav_goto",
        .description = "Start autonomous navigation to an absolute GPS coordinate. "
            "The car will drive toward the target using GPS + IMU, avoiding obstacles. "
            "Returns immediately — navigation runs in background. "
            "You will be notified when the car arrives or encounters an unresolvable situation.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"lat\":{\"type\":\"number\",\"description\":\"Target latitude in decimal degrees\"},"
            "\"lon\":{\"type\":\"number\",\"description\":\"Target longitude in decimal degrees\"},"
            "\"speed_pct\":{\"type\":\"integer\",\"description\":\"Cruise speed percentage 1-100 (default 35)\"}"
            "},"
            "\"required\":[\"lat\",\"lon\"]}",
        .execute = tool_nav_goto_execute,
    };
    register_tool(&ng);

    mimi_tool_t ngw = {
        .name = "nav_goto_waypoint",
        .description = "Start autonomous navigation to a named waypoint. "
            "The waypoint must have been saved with nav_save_waypoint. "
            "Returns immediately — navigation runs in background.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Waypoint name (e.g. '快递柜')\"},"
            "\"speed_pct\":{\"type\":\"integer\",\"description\":\"Cruise speed percentage 1-100 (default 35)\"}"
            "},"
            "\"required\":[\"name\"]}",
        .execute = tool_nav_goto_waypoint_execute,
    };
    register_tool(&ngw);

    mimi_tool_t npause = {
        .name = "nav_pause",
        .description = "Pause autonomous navigation (car stops). Call nav_resume to continue from where it paused. "
            "Use before nav_manual_step to take temporary control.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_nav_pause_execute,
    };
    register_tool(&npause);

    mimi_tool_t nresume = {
        .name = "nav_resume",
        .description = "Resume autonomous navigation after nav_pause. "
            "Navigation continues from the same FSM state it was in before pausing.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_nav_resume_execute,
    };
    register_tool(&nresume);

    mimi_tool_t nabort = {
        .name = "nav_abort",
        .description = "Abort the current navigation task immediately. Car stops. "
            "Use when the situation is unsafe or the user wants to cancel. "
            "Call nav_goto or nav_goto_waypoint to start a new task.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_nav_abort_execute,
    };
    register_tool(&nabort);

    mimi_tool_t nms = {
        .name = "nav_manual_step",
        .description = "Temporarily override autonomous navigation with a direct motor command. "
            "L2 is paused for hold_ms, then automatically resumes. "
            "L1 emergency stop (< 20 cm) still applies during manual step. "
            "Useful for nudging the car out of a stuck situation.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"steer_pct\":{\"type\":\"integer\",\"description\":\"Steering: -100 (full left) to 100 (full right)\"},"
            "\"throttle_pct\":{\"type\":\"integer\",\"description\":\"Throttle: -100 (full reverse) to 100 (full forward)\"},"
            "\"hold_ms\":{\"type\":\"integer\",\"description\":\"Duration in milliseconds, max 1000\"}"
            "},"
            "\"required\":[\"steer_pct\",\"throttle_pct\",\"hold_ms\"]}",
        .execute = tool_nav_manual_step_execute,
    };
    register_tool(&nms);

    build_tools_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/*  Per-turn message origin (set by agent_loop, read by tool_nav)       */
/* ------------------------------------------------------------------ */

static tool_msg_origin_t s_current_origin = {0};

void tool_registry_set_origin(const char *channel, const char *chat_id)
{
    strncpy(s_current_origin.channel, channel ? channel : "",
            sizeof(s_current_origin.channel) - 1);
    strncpy(s_current_origin.chat_id, chat_id  ? chat_id  : "",
            sizeof(s_current_origin.chat_id)  - 1);
}

void tool_registry_get_origin(tool_msg_origin_t *out)
{
    if (!out) return;
    *out = s_current_origin;
}
