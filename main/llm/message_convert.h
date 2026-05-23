#pragma once

#include "cJSON.h"

cJSON *llm_convert_messages_openai(const char *system_prompt, cJSON *messages);
