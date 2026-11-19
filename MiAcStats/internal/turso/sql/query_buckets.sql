SELECT device_id, ts_utc, session_start_utc, duration_sec, cool_sec, fan_low_sec, fan_high_sec,
       offline_sec, room_temp_avg, room_temp_min, room_temp_max, ac_temp_avg, target_temp, auto_sec
FROM (
  SELECT * FROM ac_buckets_hourly
   WHERE device_id = ? AND ts_utc >= ? AND ts_utc < ?
     AND ts_utc < COALESCE((SELECT CAST(value AS INTEGER) FROM app_config WHERE key = 'rollup_watermark'), 0)
  UNION ALL
  SELECT * FROM ac_buckets
   WHERE device_id = ? AND ts_utc >= ? AND ts_utc < ?
     AND ts_utc >= COALESCE((SELECT CAST(value AS INTEGER) FROM app_config WHERE key = 'rollup_watermark'), 0)
)
ORDER BY ts_utc ASC;
