package stats

import (
	"fmt"
	"math"
	"sort"
	"strings"
	"time"

	"miacstats/internal/model"
)

// NormalizeSettings clamps non-sensical inputs while preserving defaults.
func NormalizeSettings(in model.BillingSettings, fallback model.BillingSettings) model.BillingSettings {
	out := in
	if out.CoolWatts <= 0 {
		out.CoolWatts = fallback.CoolWatts
	}
	if out.FanLowWatts < 0 {
		out.FanLowWatts = fallback.FanLowWatts
	}
	if out.FanHighWatts < 0 {
		out.FanHighWatts = fallback.FanHighWatts
	}
	if out.TariffMode != "flat" {
		out.TariffMode = "evn"
	}
	if out.FlatRateVND < 0 {
		out.FlatRateVND = fallback.FlatRateVND
	}
	if out.VATPercent < 0 {
		out.VATPercent = 0
	}
	if out.VATPercent > 100 {
		out.VATPercent = 100
	}
	if out.BaseHomeKWh < 0 {
		out.BaseHomeKWh = 0
	}
	if len(out.EvnTiers) == 0 {
		out.EvnTiers = append([]model.TierRate(nil), fallback.EvnTiers...)
	}
	return out
}

type dailyRange struct {
	startLocal time.Time
	endLocal   time.Time
}

// ComputeDashboard builds summary, session list, calendar daily breakdown, and 24h hourly stats.
func ComputeDashboard(
	deviceID string,
	fromDateTime string,
	toDateTime string,
	loc *time.Location,
	demoMode bool,
	buckets []model.Bucket,
	cfg model.BillingSettings,
) model.DashboardStats {
	if loc == nil {
		loc = time.UTC
	}

	hourly := make([]model.HourlyStat, 24)
	for h := 0; h < 24; h++ {
		hourly[h].Hour = h
	}

	var (
		totalCoolSec    int
		totalFanLowSec  int
		totalFanHighSec int
		totalOfflineSec int
		totalAutoSec    int

		roomTempSum    float64
		roomTempWeight int
		coolTempSum    float64
		coolTempWeight int
		minRoomTemp    *float64
		maxRoomTemp    *float64
		targetTempSum  float64
		targetTempW    int
	)

	// --- 1. Session Grouping ---
	type sessionAcc struct {
		startUTC    int64
		firstTs     int64
		lastTs      int64
		coolSec     int
		fanLowSec   int
		fanHighSec  int
		offlineSec  int
		autoSec     int
		tempSum     float64
		tempWeight  int
		minTemp     *float64
		maxTemp     *float64
		targetSum   float64
		targetW     int
		latestTsUTC int64
	}
	sessionsMap := make(map[int64]*sessionAcc)

	// --- 2. Calendar Daily Grouping (00:00 - 23:59) ---
	type dailyAcc struct {
		dateKey    string
		coolSec    int
		fanLowSec  int
		fanHighSec int
		offlineSec int
		tempSum    float64
		tempWeight int
		minTemp    *float64
		maxTemp    *float64
		ranges     []dailyRange
		sessions   map[int64]bool
	}
	dailyMap := make(map[string]*dailyAcc)

	nowUTC := time.Now().UTC().Unix()

	for _, b := range buckets {
		cool := clampNonNeg(b.CoolSec)
		fanLow := clampNonNeg(b.FanLowSec)
		fanHigh := clampNonNeg(b.FanHighSec)
		offline := clampNonNeg(b.OfflineSec)
		dur := b.DurationSec
		if dur <= 0 {
			dur = cool + fanLow + fanHigh + offline
		}
		autoS := clampNonNeg(b.AutoSec)
		if autoS > dur {
			autoS = dur
		}

		totalCoolSec += cool
		totalFanLowSec += fanLow
		totalFanHighSec += fanHigh
		totalOfflineSec += offline
		totalAutoSec += autoS

		localTs := time.Unix(b.TsUTC, 0).In(loc)
		dateKey := localTs.Format("2006-01-02")
		hour := localTs.Hour()

		// Hourly stats
		hourly[hour].CoolHours += float64(cool) / 3600.0
		hourly[hour].FanHours += float64(fanLow+fanHigh) / 3600.0
		hourly[hour].TotalKWh += (float64(cool)/3600.0)*(cfg.CoolWatts/1000.0) +
			(float64(fanLow)/3600.0)*(cfg.FanLowWatts/1000.0) +
			(float64(fanHigh)/3600.0)*(cfg.FanHighWatts/1000.0)

		// Temperature tracking
		activeSec := cool + fanLow + fanHigh
		if activeSec > 0 && b.RoomTempAvg != nil {
			roomTempSum += (*b.RoomTempAvg) * float64(activeSec)
			roomTempWeight += activeSec
			if cool > 0 {
				coolTempSum += (*b.RoomTempAvg) * float64(cool)
				coolTempWeight += cool
			}
		}
		if b.RoomTempMin != nil {
			minRoomTemp = pickMin(minRoomTemp, *b.RoomTempMin)
		} else if b.RoomTempAvg != nil {
			minRoomTemp = pickMin(minRoomTemp, *b.RoomTempAvg)
		}
		if b.RoomTempMax != nil {
			maxRoomTemp = pickMax(maxRoomTemp, *b.RoomTempMax)
		} else if b.RoomTempAvg != nil {
			maxRoomTemp = pickMax(maxRoomTemp, *b.RoomTempAvg)
		}
		if activeSec > 0 && b.TargetTemp != nil {
			targetTempSum += float64(*b.TargetTemp) * float64(activeSec)
			targetTempW += activeSec
		}

		// Accumulate into Session
		sKey := b.SessionStartUTC
		if sKey <= 0 {
			sKey = b.TsUTC
		}
		sAcc, exists := sessionsMap[sKey]
		if !exists {
			sAcc = &sessionAcc{
				startUTC: sKey,
				firstTs:  b.TsUTC,
			}
			sessionsMap[sKey] = sAcc
		}
		if b.TsUTC < sAcc.firstTs {
			sAcc.firstTs = b.TsUTC
		}
		bucketEnd := b.TsUTC + int64(dur)
		if bucketEnd > sAcc.lastTs {
			sAcc.lastTs = bucketEnd
		}
		if b.TsUTC > sAcc.latestTsUTC {
			sAcc.latestTsUTC = b.TsUTC
		}
		sAcc.coolSec += cool
		sAcc.fanLowSec += fanLow
		sAcc.fanHighSec += fanHigh
		sAcc.offlineSec += offline
		sAcc.autoSec += autoS
		if activeSec > 0 && b.RoomTempAvg != nil {
			sAcc.tempSum += (*b.RoomTempAvg) * float64(activeSec)
			sAcc.tempWeight += activeSec
		}
		if b.RoomTempMin != nil {
			sAcc.minTemp = pickMin(sAcc.minTemp, *b.RoomTempMin)
		}
		if b.RoomTempMax != nil {
			sAcc.maxTemp = pickMax(sAcc.maxTemp, *b.RoomTempMax)
		}
		if activeSec > 0 && b.TargetTemp != nil {
			sAcc.targetSum += float64(*b.TargetTemp) * float64(activeSec)
			sAcc.targetW += activeSec
		}

		// Accumulate into Calendar Day
		dAcc, dExists := dailyMap[dateKey]
		if !dExists {
			dAcc = &dailyAcc{
				dateKey:  dateKey,
				sessions: make(map[int64]bool),
			}
			dailyMap[dateKey] = dAcc
		}
		dAcc.coolSec += cool
		dAcc.fanLowSec += fanLow
		dAcc.fanHighSec += fanHigh
		dAcc.offlineSec += offline
		dAcc.sessions[sKey] = true
		if activeSec > 0 && b.RoomTempAvg != nil {
			dAcc.tempSum += (*b.RoomTempAvg) * float64(activeSec)
			dAcc.tempWeight += activeSec
		}
		if b.RoomTempMin != nil {
			dAcc.minTemp = pickMin(dAcc.minTemp, *b.RoomTempMin)
		}
		if b.RoomTempMax != nil {
			dAcc.maxTemp = pickMax(dAcc.maxTemp, *b.RoomTempMax)
		}
		// Merge active time segments for the daily timeline
		dAcc.ranges = append(dAcc.ranges, dailyRange{
			startLocal: localTs,
			endLocal:   localTs.Add(time.Duration(dur) * time.Second),
		})
	}

	coolKWh := (float64(totalCoolSec) / 3600.0) * (cfg.CoolWatts / 1000.0)
	fanKWh := (float64(totalFanLowSec)/3600.0)*(cfg.FanLowWatts/1000.0) +
		(float64(totalFanHighSec)/3600.0)*(cfg.FanHighWatts/1000.0)
	totalKWh := coolKWh + fanKWh

	subtotalVND := CalculateSubtotalVND(totalKWh, cfg)
	vatVND := subtotalVND * (cfg.VATPercent / 100.0)
	totalCostVND := subtotalVND + vatVND

	totalFanSec := totalFanLowSec + totalFanHighSec
	totalRunSec := totalCoolSec + totalFanSec
	coolSharePct := 0.0
	if totalRunSec > 0 {
		coolSharePct = (float64(totalCoolSec) / float64(totalRunSec)) * 100.0
	}
	autoSharePct := 0.0
	if totalRunSec > 0 {
		autoSharePct = (float64(totalAutoSec) / float64(totalRunSec)) * 100.0
	}
	effectiveRate := 0.0
	if totalKWh > 0 {
		effectiveRate = totalCostVND / totalKWh
	}

	// Build sorted Sessions (newest first)
	sessionKeys := make([]int64, 0, len(sessionsMap))
	for k := range sessionsMap {
		sessionKeys = append(sessionKeys, k)
	}
	sort.Slice(sessionKeys, func(i, j int) bool {
		return sessionKeys[i] > sessionKeys[j]
	})

	sessions := make([]model.SessionStat, 0, len(sessionKeys))
	for _, sKey := range sessionKeys {
		sAcc := sessionsMap[sKey]
		sCool := sAcc.coolSec
		sFan := sAcc.fanLowSec + sAcc.fanHighSec
		sRun := sCool + sFan
		sKWh := (float64(sCool)/3600.0)*(cfg.CoolWatts/1000.0) +
			(float64(sAcc.fanLowSec)/3600.0)*(cfg.FanLowWatts/1000.0) +
			(float64(sAcc.fanHighSec)/3600.0)*(cfg.FanHighWatts/1000.0)
		sCost := sKWh * effectiveRate

		sCoolShare := 0.0
		if sRun > 0 {
			sCoolShare = (float64(sCool) / float64(sRun)) * 100.0
		}
		sAutoShare := 0.0
		if sRun > 0 {
			sAutoShare = (float64(sAcc.autoSec) / float64(sRun)) * 100.0
		}

		startT := time.Unix(sAcc.firstTs, 0).In(loc)
		endT := time.Unix(sAcc.lastTs, 0).In(loc)
		isActive := (nowUTC - sAcc.latestTsUTC) < 300 // last packet within 5 minutes

		sessions = append(sessions, model.SessionStat{
			SessionID:     fmt.Sprintf("S-%d", sKey),
			StartLocal:    startT.Format("2006-01-02 15:04"),
			EndLocal:      endT.Format("2006-01-02 15:04"),
			DurationHours: round2(float64(sRun+sAcc.offlineSec) / 3600.0),
			CoolHours:     round2(float64(sCool) / 3600.0),
			FanHours:      round2(float64(sFan) / 3600.0),
			CoolSharePct:  round1(sCoolShare),
			AvgRoomTemp:   weightedAvg(sAcc.tempSum, sAcc.tempWeight),
			MinRoomTemp:   roundPtr1(sAcc.minTemp),
			MaxRoomTemp:   roundPtr1(sAcc.maxTemp),
			AvgTargetTemp: weightedAvg(sAcc.targetSum, sAcc.targetW),
			TotalKWh:      round3(sKWh),
			TotalCostVND:  math.Round(sCost),
			AutoSharePct:  round1(sAutoShare),
			IsActive:      isActive,
		})
	}

	// Build sorted Daily breakdown (newest first)
	dailyKeys := make([]string, 0, len(dailyMap))
	for k := range dailyMap {
		dailyKeys = append(dailyKeys, k)
	}
	sort.Sort(sort.Reverse(sort.StringSlice(dailyKeys)))

	daily := make([]model.DailyStat, 0, len(dailyKeys))
	for _, dKey := range dailyKeys {
		dAcc := dailyMap[dKey]
		dCool := dAcc.coolSec
		dFan := dAcc.fanLowSec + dAcc.fanHighSec
		dTotal := dCool + dFan
		dKWh := (float64(dCool)/3600.0)*(cfg.CoolWatts/1000.0) +
			(float64(dAcc.fanLowSec)/3600.0)*(cfg.FanLowWatts/1000.0) +
			(float64(dAcc.fanHighSec)/3600.0)*(cfg.FanHighWatts/1000.0)
		dCost := dKWh * effectiveRate

		daily = append(daily, model.DailyStat{
			Date:         dKey,
			TimeRanges:   formatTimeRanges(dAcc.ranges),
			CoolHours:    round2(float64(dCool) / 3600.0),
			FanHours:     round2(float64(dFan) / 3600.0),
			TotalHours:   round2(float64(dTotal) / 3600.0),
			AvgRoomTemp:  weightedAvg(dAcc.tempSum, dAcc.tempWeight),
			MinRoomTemp:  roundPtr1(dAcc.minTemp),
			MaxRoomTemp:  roundPtr1(dAcc.maxTemp),
			TotalKWh:     round3(dKWh),
			TotalCostVND: math.Round(dCost),
			SessionCount: len(dAcc.sessions),
		})
	}

	for h := 0; h < 24; h++ {
		hourly[h].CoolHours = round2(hourly[h].CoolHours)
		hourly[h].FanHours = round2(hourly[h].FanHours)
		hourly[h].TotalKWh = round3(hourly[h].TotalKWh)
	}

	summary := model.Summary{
		BucketCount:     len(buckets),
		SessionCount:    len(sessions),
		TotalRunHours:   round2(float64(totalRunSec) / 3600.0),
		CoolHours:       round2(float64(totalCoolSec) / 3600.0),
		FanHours:        round2(float64(totalFanSec) / 3600.0),
		CoolSharePct:    round1(coolSharePct),
		AvgRoomTemp:     weightedAvg(roomTempSum, roomTempWeight),
		CoolAvgRoomTemp: weightedAvg(coolTempSum, coolTempWeight),
		MinRoomTemp:     roundPtr1(minRoomTemp),
		MaxRoomTemp:     roundPtr1(maxRoomTemp),
		AvgTargetTemp:   weightedAvg(targetTempSum, targetTempW),
		CoolKWh:         round3(coolKWh),
		FanKWh:          round3(fanKWh),
		TotalKWh:        round3(totalKWh),
		SubtotalVND:     math.Round(subtotalVND),
		VATVND:          math.Round(vatVND),
		TotalCostVND:    math.Round(totalCostVND),
		EffectiveRate:   math.Round(effectiveRate),
		AutoSharePct:    round1(autoSharePct),
	}

	return model.DashboardStats{
		DeviceID:     deviceID,
		FromDateTime: fromDateTime,
		ToDateTime:   toDateTime,
		Timezone:     loc.String(),
		DemoMode:     demoMode,
		Settings:     cfg,
		Summary:      summary,
		Sessions:     sessions,
		Daily:        daily,
		Hourly:       hourly,
	}
}

// CalculateSubtotalVND computes pre-VAT cost in VND for totalKWh.
func CalculateSubtotalVND(totalKWh float64, cfg model.BillingSettings) float64 {
	if totalKWh <= 0 {
		return 0
	}
	if cfg.TariffMode == "flat" {
		return totalKWh * cfg.FlatRateVND
	}
	if len(cfg.EvnTiers) == 0 {
		return totalKWh * cfg.FlatRateVND
	}
	return progressiveCost(cfg.BaseHomeKWh+totalKWh, cfg.EvnTiers) -
		progressiveCost(cfg.BaseHomeKWh, cfg.EvnTiers)
}

func progressiveCost(totalKWh float64, tiers []model.TierRate) float64 {
	if totalKWh <= 0 {
		return 0
	}
	var cost float64
	var prevUpper float64
	remaining := totalKWh

	for _, t := range tiers {
		if remaining <= 0 {
			break
		}
		if t.UpperKWh <= 0 || t.UpperKWh <= prevUpper {
			cost += remaining * t.RateVND
			remaining = 0
			break
		}
		span := t.UpperKWh - prevUpper
		used := math.Min(remaining, span)
		cost += used * t.RateVND
		remaining -= used
		prevUpper = t.UpperKWh
	}
	if remaining > 0 && len(tiers) > 0 {
		cost += remaining * tiers[len(tiers)-1].RateVND
	}
	return cost
}

func formatTimeRanges(ranges []dailyRange) string {
	if len(ranges) == 0 {
		return "--"
	}
	sort.Slice(ranges, func(i, j int) bool {
		return ranges[i].startLocal.Before(ranges[j].startLocal)
	})

	type mergedRange struct {
		start time.Time
		end   time.Time
	}
	var merged []mergedRange
	for _, r := range ranges {
		if len(merged) == 0 {
			merged = append(merged, mergedRange{start: r.startLocal, end: r.endLocal})
			continue
		}
		last := &merged[len(merged)-1]
		// If continuous or gap <= 3 minutes, merge
		if r.startLocal.Sub(last.end) <= 3*time.Minute {
			if r.endLocal.After(last.end) {
				last.end = r.endLocal
			}
		} else {
			merged = append(merged, mergedRange{start: r.startLocal, end: r.endLocal})
		}
	}

	parts := make([]string, 0, len(merged))
	for _, m := range merged {
		parts = append(parts, fmt.Sprintf("%s–%s", m.start.Format("15:04"), m.end.Format("15:04")))
	}
	return strings.Join(parts, ", ")
}

// DemoBuckets generates realistic 7-day demo sessions:
// - Overnight runs: 22:15 to 06:15 next morning (crosses midnight)
// - Afternoon runs: 13:30 to 15:30 on hot days
// - 1 day with zero AC usage
func DemoBuckets(now time.Time, loc *time.Location, deviceID string) []model.Bucket {
	if loc == nil {
		loc = time.UTC
	}
	localNow := now.In(loc)
	startDay := time.Date(localNow.Year(), localNow.Month(), localNow.Day()-6, 0, 0, 0, 0, loc)
	out := make([]model.Bucket, 0, 3000)

	for dayOffset := 0; dayOffset < 7; dayOffset++ {
		if dayOffset == 3 {
			continue // zero-usage day
		}
		curDay := startDay.AddDate(0, 0, dayOffset)

		// Night session: 22:15 -> 06:15 (480 minutes, crossing midnight)
		nightStart := curDay.Add(22*time.Hour + 15*time.Minute)
		appendSession(&out, deviceID, nightStart, 480, 0.60, 25.4, 26)

		// Afternoon session: 13:30 -> 15:30 (120 minutes)
		if dayOffset%2 == 0 {
			afternoonStart := curDay.Add(13*time.Hour + 30*time.Minute)
			appendSession(&out, deviceID, afternoonStart, 120, 0.75, 26.8, 26)
		}
	}
	return out
}

func appendSession(
	out *[]model.Bucket,
	deviceID string,
	start time.Time,
	minutes int,
	coolRatio float64,
	baseTemp float64,
	target int,
) {
	sessionStartUTC := start.UTC().Unix()
	coolBlock := int(math.Round(10 * coolRatio))

	for m := 0; m < minutes; m++ {
		ts := start.Add(time.Duration(m) * time.Minute)
		if ts.After(time.Now()) {
			break
		}
		inCool := (m % 10) < coolBlock
		coolSec, fanLowSec := 0, 60
		if inCool {
			coolSec = 60
			fanLowSec = 0
		}
		temp := baseTemp + 0.6*math.Sin(float64(m)/8.0)
		minT := round1(temp - 0.2)
		maxT := round1(temp + 0.2)
		avgT := round1(temp)
		acT := round1(temp + 1.0)
		tgt := target

		*out = append(*out, model.Bucket{
			DeviceID:        deviceID,
			TsUTC:           ts.UTC().Unix(),
			SessionStartUTC: sessionStartUTC,
			DurationSec:     60,
			CoolSec:         coolSec,
			FanLowSec:       fanLowSec,
			FanHighSec:      0,
			RoomTempAvg:     &avgT,
			RoomTempMin:     &minT,
			RoomTempMax:     &maxT,
			TargetTemp:      &tgt,
			AcTempAvg:       &acT,
			AutoSec:         60,
		})
	}
}

func clampNonNeg(v int) int {
	if v < 0 {
		return 0
	}
	return v
}

func pickMin(cur *float64, v float64) *float64 {
	if cur == nil || v < *cur {
		nv := v
		return &nv
	}
	return cur
}

func pickMax(cur *float64, v float64) *float64 {
	if cur == nil || v > *cur {
		nv := v
		return &nv
	}
	return cur
}

func weightedAvg(sum float64, weight int) *float64 {
	if weight <= 0 {
		return nil
	}
	v := round1(sum / float64(weight))
	return &v
}

func roundPtr1(p *float64) *float64 {
	if p == nil {
		return nil
	}
	v := round1(*p)
	return &v
}

func round1(v float64) float64 {
	return math.Round(v*10) / 10
}

func round2(v float64) float64 {
	return math.Round(v*100) / 100
}

func round3(v float64) float64 {
	return math.Round(v*1000) / 1000
}
