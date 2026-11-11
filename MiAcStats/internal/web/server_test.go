package web

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"miacstats/internal/config"
	"miacstats/internal/model"
)

type memoryStore struct {
	buckets  []model.Bucket
	settings model.BillingSettings
}

func (m *memoryStore) EnsureSchema(context.Context) error { return nil }

func (m *memoryStore) QueryBuckets(_ context.Context, deviceID string, fromUTC, toUTC int64) ([]model.Bucket, error) {
	var out []model.Bucket
	for _, b := range m.buckets {
		if b.DeviceID == deviceID && b.TsUTC >= fromUTC && b.TsUTC < toUTC {
			out = append(out, b)
		}
	}
	return out, nil
}

func (m *memoryStore) UpsertBuckets(_ context.Context, buckets []model.Bucket) error {
	m.buckets = append(m.buckets, buckets...)
	return nil
}

func (m *memoryStore) LoadBillingSettings(_ context.Context, fallback model.BillingSettings) (model.BillingSettings, error) {
	if m.settings.CoolWatts == 0 {
		return fallback, nil
	}
	return m.settings, nil
}

func (m *memoryStore) SaveBillingSettings(_ context.Context, cfg model.BillingSettings) error {
	m.settings = cfg
	return nil
}

func TestDashboardRoutesAndQueries(t *testing.T) {
	loc := time.FixedZone("UTC+7", 7*3600)
	cfg := config.Config{
		Port:            "8080",
		DefaultDeviceID: "miac-main",
		Timezone:        "UTC+7",
		Location:        loc,
		DemoMode:        true,
		DefaultBilling:  config.Load().DefaultBilling,
	}

	store := &memoryStore{}
	h, err := NewServer(cfg, store)
	if err != nil {
		t.Fatalf("NewServer failed: %v", err)
	}

	// 1. Health endpoint
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/healthz", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), `"ok":true`) {
		t.Fatalf("unexpected /healthz response: %d %s", rec.Code, rec.Body.String())
	}

	// 2. Index HTML renders
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "MiAcStats") {
		t.Fatalf("unexpected / response: %d", rec.Code)
	}

	// 3. Stats query with populated memory store
	ts := time.Date(2026, 11, 11, 22, 0, 0, 0, loc).UTC().Unix()
	store.buckets = append(store.buckets, model.Bucket{
		DeviceID:        "miac-main",
		TsUTC:           ts,
		SessionStartUTC: ts,
		DurationSec:     60,
		CoolSec:         60,
		FanLowSec:       0,
		FanHighSec:      0,
		AutoSec:         60,
	})

	statsRec := httptest.NewRecorder()
	h.ServeHTTP(statsRec, httptest.NewRequest(http.MethodGet, "/api/stats?from=2026-11-11T20:00&to=2026-11-11T23:59", nil))
	if statsRec.Code != http.StatusOK {
		t.Fatalf("expected 200 on /api/stats, got %d %s", statsRec.Code, statsRec.Body.String())
	}
	var dash model.DashboardStats
	if err := json.Unmarshal(statsRec.Body.Bytes(), &dash); err != nil {
		t.Fatalf("unmarshal stats: %v", err)
	}
	if dash.Summary.BucketCount != 1 {
		t.Fatalf("expected 1 bucket in stats, got %d", dash.Summary.BucketCount)
	}
	if len(dash.Sessions) != 1 {
		t.Fatalf("expected 1 session, got %d", len(dash.Sessions))
	}
	if len(dash.Daily) != 1 {
		t.Fatalf("expected 1 daily record, got %d", len(dash.Daily))
	}

	// 4. Settings validation & persistence
	settingsReq := httptest.NewRequest(http.MethodPost, "/api/settings", bytes.NewBufferString(`{"cool_watts":700,"fan_low_watts":30,"fan_high_watts":50,"tariff_mode":"flat","flat_rate_vnd":3200,"vat_percent":8,"base_home_kwh":0}`))
	settingsRec := httptest.NewRecorder()
	h.ServeHTTP(settingsRec, settingsReq)
	if settingsRec.Code != http.StatusOK || store.settings.CoolWatts != 700 {
		t.Fatalf("settings update failed: %d %s", settingsRec.Code, settingsRec.Body.String())
	}
}
