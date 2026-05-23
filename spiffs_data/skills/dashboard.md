# Dashboard

Manage the MimiClaw e-paper dashboard from chat.

## When to use
Use this skill when the user asks to show, save, update, or refresh dashboard content, weather city, weather summary, or todos.

## Tools
- `display_set_weather_city`: save the preferred weather city.
- `display_set_weather`: save a short weather summary after looking up weather.
- `display_set_todos`: replace dashboard todos with a concise list.
- `display_get_state`: inspect current dashboard state.
- `display_refresh`: request a refresh after saving display content.

## Guidance
1. Keep display text short and ASCII-friendly; Chinese font support is not implemented yet.
2. Do not create default cron jobs automatically.
3. For todo generation, produce up to 5 short action items, then call `display_set_todos`.
4. After saving weather or todos, call `display_refresh` with `{}` to queue a non-blocking refresh.
5. If the display hardware is unavailable, the tools still persist dashboard state; tell the user the state was saved.

## Examples
User: "Set my dashboard city to Tokyo"
→ `display_set_weather_city` with `{ "city": "Tokyo" }`
→ `display_refresh` with `{}`

User: "Generate today's todo list for my dashboard"
→ Create up to 5 concise todos
→ `display_set_todos` with `{ "todos": ["Review priorities", "Reply to urgent mail", "Plan workout"] }`
→ `display_refresh` with `{}`
