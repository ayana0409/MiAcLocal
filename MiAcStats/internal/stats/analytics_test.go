package stats

import (
	"math"
	"testing"
	"time"

	"miacstats/internal/config"
	"miacstats/internal/model"
)

func fptr(v float64) *float64 { return &v }
func iptr(v int) *int         { return &v }

func TestComputeDashboard700WNonInverterAndEVNTariff(t *testing.T) {
	loc := time.FixedZone("UTC+7", 7*3600)
	day1 := time.Date(2026, 11, 10, 22, 0, 0, 0, loc)
	sessionStart := day1.UTC().Unix()

	// 120 minutes total session:
	// - 90 minutes COOL (including DRY if any) @ 700W = 1.5h * 0.7 = 1.05 kWh
	// - 30 minutes FAN high @ 50W = 0.5h * 0.05 = 0.025 kWh
	// Total kWh = 1.075 kWh
	buckets := []model.Bucket{
		{
			DeviceID:        "miac-main",
			TsUTC:           day1.UTC().Unix(),
			SessionStartUTC: sessionStart,
			DurationSec:     5400, // 90 mins
			CoolSec:         5400,
			FanLowSec:       0,
			FanHighSec:      0,
			OfflineSec:      0,
			RoomTempAvg:     fptr(26.0),
			RoomTempMin:     fptr(25.0),
			RoomTempMax:     fptr(27.5),
			AcTempAvg:       fptr(27.0),
			TargetTemp:      iptr(26),
			AutoSec:         5400,
		},
		{
			DeviceID:        "miac-main",
			TsUTC:           day1.Add(90 * time.Minute).UTC().Unix(),
			SessionStartUTC: sessionStart,
			DurationSec:     1800, // 30 mins
			CoolSec:         0,
			FanLowSec:       0,
			FanHighSec:      1800,
			OfflineSec:      0,
			RoomTempAvg:     fptr(25.2),
			RoomTempMin:     fptr(25.0),
			RoomTempMax:     fptr(25.5),
			AcTempAvg:       fptr(26.0),
			TargetTemp:      iptr(26),
			AutoSec:         1800,
		},
		{
			// Offline fault bucket should not count active watts
			DeviceID:        "miac-main",
			TsUTC:           day1.Add(120 * time.Minute).UTC().Unix(),
			SessionStartUTC: sessionStart,
			DurationSec:     600,
			CoolSec:         0,
			FanLowSec:       0,
			FanHighSec:      0,
			OfflineSec:      600,
			RoomTempAvg:     nil, // sensor fault or unread
		},
	}

	cfg := model.BillingSettings{
		CoolWatts:    700,
		FanLowWatts:  30,
		FanHighWatts: 50,
		TariffMode:   "evn",
		FlatRateVND:  3000,
		VATPercent:   10,
		BaseHomeKWh:  99.0, // 1.0 kWh in Tier 2 (51-100), remaining 0.075 kWh in Tier 3 (101-200)
		EvnTiers:     config.DefaultEvnTiers(),
	}

	res := ComputeDashboard("miac-main", "2026-11-10T22:00", "2026-11-11T01:00", loc, false, buckets, cfg)

	if math.Abs(res.Summary.CoolHours-1.5) > 0.001 {
		t.Fatalf("expected CoolHours=1.5, got %v", res.Summary.CoolHours)
	}
	if math.Abs(res.Summary.FanHours-0.5) > 0.001 {
		t.Fatalf("expected FanHours=0.5, got %v", res.Summary.FanHours)
	}
	if math.Abs(res.Summary.CoolKWh-1.05) > 0.001 {
		t.Fatalf("expected CoolKWh=1.05, got %v", res.Summary.CoolKWh)
	}
	if math.Abs(res.Summary.FanKWh-0.025) > 0.001 {
		t.Fatalf("expected FanKWh=0.025, got %v", res.Summary.FanKWh)
	}
	if math.Abs(res.Summary.TotalKWh-1.075) > 0.001 {
		t.Fatalf("expected TotalKWh=1.075, got %v", res.Summary.TotalKWh)
	}

	// EVN calculation:
	// BaseHomeKWh = 99.0
	// 1.0 kWh fills Tier 2 @ 1956 = 1956 VND
	// 0.075 kWh enters Tier 3 @ 2271 = 170.325 VND
	// Pre-VAT = 2126.325 VND. With 10% VAT: 2126.325 * 1.1 = 2338.95 -> rounded 2339 VND
	if res.Summary.TotalCostVND != 2339 {
		t.Fatalf("expected TotalCostVND=2339, got %v", res.Summary.TotalCostVND)
	}

	// Check Session Table: all buckets have the same sessionStart, so exactly 1 session
	if len(res.Sessions) != 1 {
		t.Fatalf("expected 1 session, got %d", len(res.Sessions))
	}
	s := res.Sessions[0]
	if math.Abs(s.CoolHours-1.5) > 0.001 || math.Abs(s.TotalKWh-1.075) > 0.001 {
		t.Fatalf("session stat mismatch: %+v", s)
	}

	// Check Daily Table: runs across midnight (22:00 -> 23:30 on Nov 10, 23:30 -> 00:00 on Nov 11)
	if len(res.Daily) != 2 {
		t.Fatalf("expected 2 daily calendar rows across midnight, got %d", len(res.Daily))
	}
}

func TestOvernightSessionGrouping(t *testing.T) {
	loc := time.FixedZone("UTC+7", 7*3600)
	startLocal := time.Date(2026, 11, 10, 22, 15, 0, 0, loc)
	sessionStartUTC := startLocal.UTC().Unix()

	// 8 hours continuous overnight run: 22:15 Nov 10 -> 06:15 Nov 11
	// 480 1-minute buckets
	buckets := make([]model.Bucket, 0, 480)
	for m := 0; m < 480; m++ {
		bTs := startLocal.Add(time.Duration(m) * time.Minute).UTC().Unix()
		buckets = append(buckets, model.Bucket{
			DeviceID:        "miac-main",
			TsUTC:           bTs,
			SessionStartUTC: sessionStartUTC,
			DurationSec:     60,
			CoolSec:         45, // 45s cool, 15s fan
			FanLowSec:       15,
			AutoSec:         60,
			RoomTempAvg:     fptr(25.5),
		})
	}

	cfg := model.BillingSettings{
		CoolWatts:   700,
		FanLowWatts: 30,
		TariffMode:  "flat",
		FlatRateVND: 3000,
	}

	res := ComputeDashboard("miac-main", "2026-11-10T22:00", "2026-11-11T07:00", loc, false, buckets, cfg)

	if len(res.Sessions) != 1 {
		t.Fatalf("expected 1 continuous session, got %d", len(res.Sessions))
	}
	if math.Abs(res.Sessions[0].DurationHours-8.0) > 0.05 {
		t.Fatalf("expected session DurationHours~8.0, got %v", res.Sessions[0].DurationHours)
	}

	// In Daily view, it must be split into 2 calendar days (Nov 10: 22:15-23:59, Nov 11: 00:00-06:15)
	if len(res.Daily) != 2 {
		t.Fatalf("expected 2 calendar day rows in Daily view, got %d", len(res.Daily))
	}
	if res.Daily[0].Date != "2026-11-11" || res.Daily[1].Date != "2026-11-10" {
		t.Fatalf("expected Daily sorted newest first, got %s and %s", res.Daily[0].Date, res.Daily[1].Date)
	}
}

func TestCalculateSubtotalFlatTariff(t *testing.T) {
	cfg := model.BillingSettings{
		TariffMode:  "flat",
		FlatRateVND: 3500,
	}
	got := CalculateSubtotalVND(12.5, cfg)
	if math.Abs(got-43750) > 0.01 {
		t.Fatalf("expected 43750, got %v", got)
	}
}
