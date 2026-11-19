package rollup

import (
	"context"
	"errors"
	"testing"
	"time"

	"miacstats/internal/config"
)

type mockBackend struct {
	watermark    int64
	lastRunAt    int64
	minRawTs     int64
	stateErr     error
	rollupErr    error
	calledRanges [][2]int64
	lastRunSaved int64
}

func (m *mockBackend) GetRollupState(_ context.Context) (int64, int64, int64, error) {
	return m.watermark, m.lastRunAt, m.minRawTs, m.stateErr
}

func (m *mockBackend) RollupWindow(_ context.Context, fromUTC, toUTC int64, _ bool) error {
	if m.rollupErr != nil {
		return m.rollupErr
	}
	m.calledRanges = append(m.calledRanges, [2]int64{fromUTC, toUTC})
	return nil
}

func (m *mockBackend) SetRollupLastRunAt(_ context.Context, ts int64) error {
	m.lastRunSaved = ts
	return nil
}

func TestRunnerDisabled(t *testing.T) {
	cfg := config.Config{RollupEnabled: false}
	backend := &mockBackend{}
	r := NewRunner(cfg, backend)

	if err := r.Run(context.Background(), false); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(backend.calledRanges) != 0 {
		t.Fatalf("backend called when disabled")
	}
}

func TestRunnerTimezoneGuard(t *testing.T) {
	// Kolkata is UTC+5:30 (offset 19800 sec, not divisible by 3600)
	loc := time.FixedZone("Asia/Kolkata", 5*3600+1800)
	cfg := config.Config{
		RollupEnabled: true,
		Location:      loc,
		Timezone:      "Asia/Kolkata",
	}
	backend := &mockBackend{}
	r := NewRunner(cfg, backend)

	if err := r.Run(context.Background(), false); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(backend.calledRanges) != 0 {
		t.Fatalf("expected rollup to be skipped for non-integer timezone offset")
	}
}

func TestRunnerMinIntervalAndForce(t *testing.T) {
	loc := time.FixedZone("UTC+7", 7*3600)
	fixedNow := time.Unix(1760000000, 0)
	cfg := config.Config{
		RollupEnabled:          true,
		Location:               loc,
		RollupKeepDays:         7,
		RollupMinIntervalHours: 24,
	}

	backend := &mockBackend{
		watermark: 1750000000,
		minRawTs:  1750000000,
		lastRunAt: fixedNow.Unix() - 3600, // ran 1 hour ago
	}
	r := NewRunner(cfg, backend)
	r.nowFn = func() time.Time { return fixedNow }

	// 1. Without force -> skipped
	if err := r.Run(context.Background(), false); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(backend.calledRanges) != 0 {
		t.Fatalf("expected skip when min interval not elapsed")
	}

	// 2. With force -> executed
	if err := r.Run(context.Background(), true); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(backend.calledRanges) == 0 {
		t.Fatalf("expected execution when force=true")
	}
}

func TestRunnerChunking(t *testing.T) {
	loc := time.FixedZone("UTC+7", 7*3600)
	// Base time: aligned to hour, 100 days before nowTime
	startTime := int64(1700000000 / 3600 * 3600)
	nowTime := startTime + int64(100*86400) // 100 days later

	cfg := config.Config{
		RollupEnabled:          true,
		Location:               loc,
		RollupKeepDays:         10, // keep 10 days, so 90 days to rollup
		RollupMinIntervalHours: 24,
	}

	backend := &mockBackend{
		watermark: startTime,
		minRawTs:  startTime,
		lastRunAt: 0,
	}
	r := NewRunner(cfg, backend)
	r.nowFn = func() time.Time { return time.Unix(nowTime, 0) }

	if err := r.Run(context.Background(), false); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	// 90 days / 30 days per chunk = 3 chunks
	if len(backend.calledRanges) != 3 {
		t.Fatalf("expected 3 chunks, got %d: %+v", len(backend.calledRanges), backend.calledRanges)
	}

	chunk1 := backend.calledRanges[0]
	chunk2 := backend.calledRanges[1]
	chunk3 := backend.calledRanges[2]

	chunkSize := int64(30 * 86400)
	if chunk1[0] != startTime || chunk1[1] != startTime+chunkSize {
		t.Errorf("unexpected chunk 1: %+v", chunk1)
	}
	if chunk2[0] != startTime+chunkSize || chunk2[1] != startTime+2*chunkSize {
		t.Errorf("unexpected chunk 2: %+v", chunk2)
	}
	if chunk3[0] != startTime+2*chunkSize || chunk3[1] != startTime+3*chunkSize {
		t.Errorf("unexpected chunk 3: %+v", chunk3)
	}
	if backend.lastRunSaved != nowTime {
		t.Errorf("expected last run saved %d, got %d", nowTime, backend.lastRunSaved)
	}
}

func TestRunnerContextCancellation(t *testing.T) {
	loc := time.FixedZone("UTC+7", 7*3600)
	startTime := int64(1700000000)
	nowTime := startTime + int64(100*86400)

	cfg := config.Config{
		RollupEnabled:  true,
		Location:       loc,
		RollupKeepDays: 10,
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel() // canceled before run

	backend := &mockBackend{
		watermark: startTime,
		minRawTs:  startTime,
	}
	r := NewRunner(cfg, backend)
	r.nowFn = func() time.Time { return time.Unix(nowTime, 0) }

	err := r.Run(ctx, false)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("expected context.Canceled error, got %v", err)
	}
}

func TestRunnerErrorStopsExecution(t *testing.T) {
	loc := time.FixedZone("UTC+7", 7*3600)
	startTime := int64(1700000000)
	nowTime := startTime + int64(100*86400)

	cfg := config.Config{
		RollupEnabled:  true,
		Location:       loc,
		RollupKeepDays: 10,
	}

	backend := &mockBackend{
		watermark: startTime,
		minRawTs:  startTime,
		rollupErr: errors.New("network failure"),
	}
	r := NewRunner(cfg, backend)
	r.nowFn = func() time.Time { return time.Unix(nowTime, 0) }

	err := r.Run(context.Background(), false)
	if err == nil {
		t.Fatal("expected error, got nil")
	}
	if backend.lastRunSaved != 0 {
		t.Errorf("lastRunSaved must not be updated on error")
	}
}
