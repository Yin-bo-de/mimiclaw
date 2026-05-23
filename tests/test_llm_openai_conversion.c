#include <assert.h>
#include <string.h>

#include "cJSON.h"
#include "llm/message_convert.h"

static void preserves_reasoning_content_on_assistant_string_messages(void)
{
    cJSON *messages = cJSON_CreateArray();
    cJSON *assistant = cJSON_CreateObject();
    cJSON_AddStringToObject(assistant, "role", "assistant");
    cJSON_AddStringToObject(assistant, "content", "final text");
    cJSON_AddStringToObject(assistant, "reasoning_content", "thought trace");
    cJSON_AddItemToArray(messages, assistant);

    cJSON *converted = llm_convert_messages_openai("sys", messages);
    cJSON *converted_assistant = cJSON_GetArrayItem(converted, 1);
    cJSON *reasoning = cJSON_GetObjectItem(converted_assistant, "reasoning_content");

    assert(reasoning && cJSON_IsString(reasoning));
    assert(strcmp(reasoning->valuestring, "thought trace") == 0);

    cJSON_Delete(converted);
    cJSON_Delete(messages);
}

static void preserves_reasoning_content_on_assistant_tool_call_messages(void)
{
    cJSON *messages = cJSON_CreateArray();
    cJSON *assistant = cJSON_CreateObject();
    cJSON_AddStringToObject(assistant, "role", "assistant");
    cJSON_AddStringToObject(assistant, "reasoning_content", "thought trace");

    cJSON *content = cJSON_CreateArray();
    cJSON *tool_use = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_use, "type", "tool_use");
    cJSON_AddStringToObject(tool_use, "id", "call_1");
    cJSON_AddStringToObject(tool_use, "name", "get_time");
    cJSON_AddItemToObject(tool_use, "input", cJSON_CreateObject());
    cJSON_AddItemToArray(content, tool_use);
    cJSON_AddItemToObject(assistant, "content", content);
    cJSON_AddItemToArray(messages, assistant);

    cJSON *converted = llm_convert_messages_openai("sys", messages);
    cJSON *converted_assistant = cJSON_GetArrayItem(converted, 1);
    cJSON *reasoning = cJSON_GetObjectItem(converted_assistant, "reasoning_content");

    assert(reasoning && cJSON_IsString(reasoning));
    assert(strcmp(reasoning->valuestring, "thought trace") == 0);

    cJSON_Delete(converted);
    cJSON_Delete(messages);
}

int main(void)
{
    preserves_reasoning_content_on_assistant_string_messages();
    preserves_reasoning_content_on_assistant_tool_call_messages();
    return 0;
}
