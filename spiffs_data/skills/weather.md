# Weather

Get current weather and forecasts using web_search.

## When to use
When the user asks about weather, temperature, or forecasts.

## How to use
1. Use get_current_time to know the current date
2. Use web_search with a query like "weather in [city] today"
3. Extract temperature, conditions, and forecast from results
4. Present in a concise, friendly format
5. If the user has a dashboard city or asks to update the dashboard, call display_set_weather with a short ASCII-friendly summary, then display_refresh with {}

## Dashboard guidance
- Use display_set_weather_city when the user chooses or changes their default dashboard weather city.
- Use display_get_state to check the saved dashboard city when the user asks for "my weather" or "dashboard weather" without naming a city.
- Keep dashboard weather summaries short, e.g. "8C cloudy H12 L4".
- Do not create default cron jobs automatically.

## Example
User: "What's the weather in Tokyo?"
→ get_current_time
→ web_search "weather Tokyo today February 2026"
→ display_set_weather {"city":"Tokyo","summary":"8C partly cloudy H12 L4"}
→ display_refresh {}
→ "Tokyo: 8°C, partly cloudy. High 12°C, low 4°C. Light wind from the north. I also saved it to the dashboard."
