package model

// Bucket stores a 1-minute (or partial flush) summary of active AC operation.
// DRY mode is counted as cool_sec. Powered-off time is not recorded.
type Bucket struct {
	DeviceID        string   `json:"device_id"`
	TsUTC           int64    `json:"ts_utc"`
	SessionStartUTC int64    `json:"session_start_utc"`
	DurationSec     int      `json:"duration_sec"`
	CoolSec         int      `json:"cool_sec"`
	FanLowSec       int      `json:"fan_low_sec"`
	FanHighSec      int      `json:"fan_high_sec"`
	OfflineSec      int      `json:"offline_sec"`
	RoomTempAvg     *float64 `json:"room_temp_avg,omitempty"`
	RoomTempMin     *float64 `json:"room_temp_min,omitempty"`
	RoomTempMax     *float64 `json:"room_temp_max,omitempty"`
	AcTempAvg       *float64 `json:"ac_temp_avg,omitempty"`
	TargetTemp      *int     `json:"target_temp,omitempty"`
	AutoSec         int      `json:"auto_sec"`
}

// TierRate describes one progressive billing bracket in VND/kWh.
// UpperKWh <= 0 marks an unbounded final bracket.
type TierRate struct {
	Label    string  `json:"label"`
	UpperKWh float64 `json:"upper_kwh"`
	RateVND  float64 `json:"rate_vnd"`
}

// BillingSettings defines Non-Inverter wattage assumptions and electricity tariff rules.
type BillingSettings struct {
	CoolWatts    float64    `json:"cool_watts"`     // Rated input power in COOL/DRY mode (default: 700 W)
	FanLowWatts  float64    `json:"fan_low_watts"`  // Power in FAN Low (default: 30 W)
	FanHighWatts float64    `json:"fan_high_watts"` // Power in FAN High (default: 50 W)
	TariffMode   string     `json:"tariff_mode"`    // "evn" or "flat"
	FlatRateVND  float64    `json:"flat_rate_vnd"`
	VATPercent   float64    `json:"vat_percent"`
	BaseHomeKWh  float64    `json:"base_home_kwh"`  // Other home appliances kWh before AC tier
	EvnTiers     []TierRate `json:"evn_tiers"`
}

// SessionStat aggregates an unbroken run from power ON to power OFF.
type SessionStat struct {
	SessionID     string   `json:"session_id"`
	StartLocal    string   `json:"start_local"` // YYYY-MM-DD HH:mm
	EndLocal      string   `json:"end_local"`   // YYYY-MM-DD HH:mm
	DurationHours float64  `json:"duration_hours"`
	CoolHours     float64  `json:"cool_hours"`
	FanHours      float64  `json:"fan_hours"`
	CoolSharePct  float64  `json:"cool_share_pct"`
	AvgRoomTemp   *float64 `json:"avg_room_temp,omitempty"`
	MinRoomTemp   *float64 `json:"min_room_temp,omitempty"`
	MaxRoomTemp   *float64 `json:"max_room_temp,omitempty"`
	AvgTargetTemp *float64 `json:"avg_target_temp,omitempty"`
	TotalKWh      float64  `json:"total_kwh"`
	TotalCostVND  float64  `json:"total_cost_vnd"`
	AutoSharePct  float64  `json:"auto_share_pct"`
	IsActive      bool     `json:"is_active"` // true if last bucket is recent (< 5 min)
}

// DailyStat aggregates run time and metrics for one standard calendar day (00:00 - 23:59).
type DailyStat struct {
	Date          string   `json:"date"`        // YYYY-MM-DD
	TimeRanges    string   `json:"time_ranges"` // e.g. "13:30–15:45, 22:15–23:59"
	CoolHours     float64  `json:"cool_hours"`
	FanHours      float64  `json:"fan_hours"`
	TotalHours    float64  `json:"total_hours"`
	AvgRoomTemp   *float64 `json:"avg_room_temp,omitempty"`
	MinRoomTemp   *float64 `json:"min_room_temp,omitempty"`
	MaxRoomTemp   *float64 `json:"max_room_temp,omitempty"`
	TotalKWh      float64  `json:"total_kwh"`
	TotalCostVND  float64  `json:"total_cost_vnd"`
	SessionCount  int      `json:"session_count"`
}

// HourlyStat holds runtime across the 24 hours of the day (0..23).
type HourlyStat struct {
	Hour      int     `json:"hour"`
	CoolHours float64 `json:"cool_hours"`
	FanHours  float64 `json:"fan_hours"`
	TotalKWh  float64 `json:"total_kwh"`
}

// Summary aggregates metrics across the filtered datetime range.
type Summary struct {
	BucketCount     int      `json:"bucket_count"`
	SessionCount    int      `json:"session_count"`
	TotalRunHours   float64  `json:"total_run_hours"`
	CoolHours       float64  `json:"cool_hours"`
	FanHours        float64  `json:"fan_hours"`
	CoolSharePct    float64  `json:"cool_share_pct"`
	AvgRoomTemp     *float64 `json:"avg_room_temp,omitempty"`
	CoolAvgRoomTemp *float64 `json:"cool_avg_room_temp,omitempty"`
	MinRoomTemp     *float64 `json:"min_room_temp,omitempty"`
	MaxRoomTemp     *float64 `json:"max_room_temp,omitempty"`
	AvgTargetTemp   *float64 `json:"avg_target_temp,omitempty"`
	CoolKWh         float64  `json:"cool_kwh"`
	FanKWh          float64  `json:"fan_kwh"`
	TotalKWh        float64  `json:"total_kwh"`
	SubtotalVND     float64  `json:"subtotal_vnd"`
	VATVND          float64  `json:"vat_vnd"`
	TotalCostVND    float64  `json:"total_cost_vnd"`
	EffectiveRate   float64  `json:"effective_rate_vnd"`
	AutoSharePct    float64  `json:"auto_share_pct"`
}

// DashboardStats is the complete payload for GET /api/stats and template rendering.
type DashboardStats struct {
	DeviceID     string          `json:"device_id"`
	FromDateTime string          `json:"from_datetime"`
	ToDateTime   string          `json:"to_datetime"`
	Timezone     string          `json:"timezone"`
	DemoMode     bool            `json:"demo_mode"`
	Settings     BillingSettings `json:"settings"`
	Summary      Summary         `json:"summary"`
	Sessions     []SessionStat   `json:"sessions"`
	Daily        []DailyStat     `json:"daily"`
	Hourly       []HourlyStat    `json:"hourly"`
}
