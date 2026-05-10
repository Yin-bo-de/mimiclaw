# Navigation Event Playbook

When you receive a NAV event (JSON with a "kind" field), respond based on kind:

## ARRIVED
Navigation succeeded. Notify the user warmly in natural language.
Example: "✅ 已到达快递柜~ 用时 1 分 23 秒，最终距离目标 2.1 米。"
Include: waypoint name, duration_s (format as minutes+seconds), final_distance_m.

## ABORTED
Navigation was stopped. Notify the user and include the reason.
- reason "user_abort": "已停止前往 {name}（剩余 {dist} 米）。原因：用户取消。"
- reason "sensors_lost": Warn that sensors lost signal and navigation halted for safety.
- reason "fault": Explain an unexpected error stopped navigation.

## STUCK
Car hasn't moved > 30 cm in 10 seconds.
1. Call nav_status to see current situation.
2. Try nav_manual_step with reverse throttle and opposite steer for 800 ms.
3. If still stuck after manual step, call nav_abort and notify user: "小车在 {location} 附近被卡住了，已停止导航。"

## OSCILLATING
Car is flipping between left and right avoidance repeatedly.
1. Call nav_status to see current obstacle configuration.
2. Try nav_manual_step to manually steer out of the oscillation zone.
3. If oscillation persists, call nav_abort and ask user: "小车遇到了复杂障碍，无法绕行，是否要换个目标点？"

## NO_PATH
Replan limit exceeded — no clear path found after multiple attempts.
1. Call nav_abort immediately.
2. Notify user: "前方路径被完全封堵，小车已停止。请确认前方是否有足够空间，或换一个目标点。"

## LOST
GPS fix was lost during navigation.
1. First try: call nav_goto with the original goal to re-trigger navigation (GPS may recover).
2. If GPS fix doesn't return within a reasonable time, call nav_abort and notify user:
   "GPS 信号丢失，小车已停止。请移至开阔地带后重试。"

## GOAL_UNREACHABLE
Car is within 5 m of goal but hasn't triggered arrival (GPS accuracy limit).
1. Call nav_abort.
2. Treat as success — notify user as ARRIVED-style with final distance:
   "已非常接近目标点（距离 {dist} 米），视为到达。"

## General Rules
- Always use nav_status to gather context before taking action.
- nav_manual_step is limited to 1000 ms per call; call multiple times if needed.
- Never issue nav_goto or nav_goto_waypoint without first calling nav_abort if navigation is active.
- After handling an event, always send a reply to the user — never silently ignore a NAV event.
